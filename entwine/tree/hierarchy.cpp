/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#include <entwine/tree/hierarchy.hpp>

#include <entwine/tree/climber.hpp>
#include <entwine/tree/cold.hpp>
#include <entwine/tree/heuristics.hpp>
#include <entwine/util/unique.hpp>

namespace entwine
{

namespace
{
    const std::string countKey("n");
}

Hierarchy::Hierarchy(
        const Metadata& metadata,
        const arbiter::Endpoint& ep,
        const arbiter::Endpoint* out,
        const bool exists)
    : Splitter(metadata.hierarchyStructure())
    , m_pool(heuristics::poolBlockSize)
    , m_metadata(metadata)
    , m_bounds(metadata.bounds())
    , m_structure(metadata.hierarchyStructure())
    , m_endpoint(ep.getSubEndpoint("h"))
    , m_outpoint(out ?
            makeUnique<arbiter::Endpoint>(out->getSubEndpoint("h")) :
            nullptr)
{
    m_base.mark = true;

    if (!exists)
    {
        m_base.t = HierarchyBlock::create(
                m_pool,
                m_metadata,
                0,
                m_outpoint.get(),
                m_structure.baseIndexSpan());
    }
    else
    {
        m_base.t = HierarchyBlock::create(
                m_pool,
                m_metadata,
                0,
                m_outpoint.get(),
                m_structure.baseIndexSpan(),
                m_endpoint.getBinary("0" + metadata.postfix()));
    }

    if (exists)
    {
        const Json::Value json(
                parse(m_endpoint.get("ids" + m_metadata.postfix())));

        Id chunkId(0);

        for (Json::ArrayIndex i(0); i < json.size(); ++i)
        {
            chunkId = Id(json[i].asString());

            const ChunkInfo chunkInfo(m_structure.getInfo(chunkId));
            const std::size_t chunkNum(chunkInfo.chunkNum());

            mark(chunkId, chunkNum);
        }
    }
}

HierarchyCell& Hierarchy::count(const PointState& pointState, const int delta)
{
    if (m_structure.isWithinBase(pointState.depth()))
    {
        return m_base.t->count(pointState.index(), pointState.tick(), delta);
    }
    else
    {
        auto& slot(getOrCreate(pointState.chunkId(), pointState.chunkNum()));
        std::unique_ptr<HierarchyBlock>& block(slot.t);

        SpinGuard lock(slot.spinner);
        if (!block)
        {
            if (slot.mark)
            {
                block = HierarchyBlock::create(
                        m_pool,
                        m_metadata,
                        pointState.chunkId(),
                        m_outpoint.get(),
                        pointState.pointsPerChunk(),
                        m_endpoint.getBinary(pointState.chunkId().str()));
            }
            else
            {
                slot.mark = true;
                block = HierarchyBlock::create(
                        m_pool,
                        m_metadata,
                        pointState.chunkId(),
                        m_outpoint.get(),
                        pointState.pointsPerChunk());
            }
        }

        return block->count(pointState.index(), pointState.tick(), delta);
    }
}

uint64_t Hierarchy::tryGet(const PointState& s) const
{
    if (const Slot* slotPtr = tryGet(s.chunkId(), s.chunkNum(), s.depth()))
    {
        const Slot& slot(*slotPtr);
        std::unique_ptr<HierarchyBlock>& block(slot.t);

        if (!slot.mark) return 0;

        SpinGuard lock(slot.spinner);
        if (!block)
        {
            std::cout <<
                "\tHierarchy awaken: " << s.chunkId() <<
                " at depth " << s.depth() <<
                ", fast? " << (s.chunkNum() < m_fast.size()) << std::endl;

            block = HierarchyBlock::create(
                    m_pool,
                    m_metadata,
                    s.chunkId(),
                    m_outpoint.get(),
                    s.pointsPerChunk(),
                    m_endpoint.getBinary(s.chunkId().str()));

            if (!block)
            {
                throw std::runtime_error(
                        "Failed awaken " + s.chunkId().str());
            }
        }

        return block->get(s.index(), s.tick());
    }

    return 0;
}

void Hierarchy::save()
{
    if (!m_outpoint) return;

    const std::string basePostfix(m_metadata.postfix());
    m_base.t->save(*m_outpoint, basePostfix);

    const std::string coldPostfix(m_metadata.postfix(true));
    iterateCold([this, &coldPostfix](const Id& chunkId, const Slot& slot)
    {
        if (slot.t) slot.t->save(*m_outpoint, coldPostfix);
    });

    Json::Value json;
    for (const auto& id : ids()) json.append(id.str());
    m_outpoint->put("ids" + basePostfix, toFastString(json));
}

Hierarchy::QueryResults Hierarchy::query(
        const Bounds& queryBounds,
        const std::size_t depthBegin,
        const std::size_t depthEnd)
{
    if (depthBegin < m_structure.baseDepthBegin())
    {
        throw std::runtime_error(
                "Request was less than hierarchy base depth");
    }

    // To get rid of any possible floating point mismatches, grow the bounds by
    // a bit and only include nodes that are entirely encapsulated by the
    // queryBounds.  Also normalize depths to match our internal mapping.
    Query query(
            queryBounds.growBy(.01),
            depthBegin - m_structure.startDepth(),
            depthEnd - m_structure.startDepth());

    PointState pointState(m_structure, m_bounds, m_structure.startDepth());
    std::deque<Dir> lag;

    QueryResults results;
    traverse(results.json, results.touched, query, pointState, lag);
    return results;
}

void Hierarchy::traverse(
        Json::Value& json,
        Slots& ids,
        const Hierarchy::Query& query,
        const PointState& pointState,
        std::deque<Dir>& lag)
{
    maybeTouch(ids, pointState);

    const uint64_t inc(tryGet(pointState));
    if (!inc) return;

    if (pointState.depth() < query.depthBegin())
    {
        if (query.bounds().contains(pointState.bounds()))
        {
            // We've arrived at our query bounds.  All subsequent calls will
            // capture all children.
            //
            // In this case, the query base depth is higher than ours, meaning
            // we'll need to aggregate multiple nodes into a single node to
            // match the reference frame of the incoming query.
            for (std::size_t i(0); i < dirEnd(); ++i)
            {
                const Dir dir(toDir(i));
                auto curlag(lag);
                curlag.push_back(dir);
                traverse(json, ids, query, pointState.getClimb(dir), curlag);
            }
        }
        else
        {
            // Query bounds is smaller than our current position's bounds, so
            // we need to split our bounds in a single direction.
            const Dir dir(
                    getDirection(
                        pointState.bounds().mid(),
                        query.bounds().mid()));

            traverse(json, ids, query, pointState.getClimb(dir), lag);
        }
    }
    else if (
            query.bounds().contains(pointState.bounds()) &&
            pointState.depth() < query.depthEnd())
    {
        accumulate(json, ids, query, pointState, lag, inc);
    }
}

void Hierarchy::accumulate(
        Json::Value& json,
        Slots& ids,
        const Hierarchy::Query& query,
        const PointState& pointState,
        std::deque<Dir>& lag,
        uint64_t inc)
{
    // Caller should not call if inc == 0 to avoid creating null keys.
    maybeTouch(ids, pointState);
    json[countKey] = json[countKey].asUInt64() + inc;

    if (pointState.depth() + 1 >= query.depthEnd()) return;

    if (lag.empty())
    {
        for (std::size_t i(0); i < dirEnd(); ++i)
        {
            const Dir dir(toDir(i));
            const PointState nextState(pointState.getClimb(dir));

            if (const uint64_t inc = tryGet(nextState))
            {
                Json::Value& nextJson(json[dirToString(dir)]);
                accumulate(nextJson, ids, query, nextState, lag, inc);
            }
        }
    }
    else
    {
        const Dir lagdir(lag.front());
        lag.pop_front();

        // Don't traverse into lagdir until we've confirmed that a child exists.
        Json::Value* nextJson(nullptr);

        for (std::size_t i(0); i < dirEnd(); ++i)
        {
            const Dir curdir(toDir(i));
            const PointState nextState(pointState.getClimb(curdir));

            if (const uint64_t inc = tryGet(nextState))
            {
                if (!nextJson) nextJson = &json[dirToString(lagdir)];
                auto curlag(lag);
                curlag.push_back(curdir);

                accumulate(*nextJson, ids, query, nextState, curlag, inc);
            }
        }

        lag.pop_back();
    }
}

void Hierarchy::maybeTouch(Slots& ids, const PointState& pointState) const
{
    if (!isWithinBase(pointState.depth()))
    {
        if (const Slot* slot = tryGet(
                    pointState.chunkId(),
                    pointState.chunkNum(),
                    pointState.depth()))
        {
            if (slot->mark) ids.insert(slot);
        }
    }
}

Structure Hierarchy::structure(const Structure& treeStructure)
{
    static const std::size_t minStartDepth(6);

    const std::size_t nullDepth(0);
    const std::size_t baseDepth(
            std::max<std::size_t>(treeStructure.baseDepthEnd(), 10));
    const std::size_t coldDepth(0);
    const std::size_t pointsPerChunk(treeStructure.basePointsPerChunk());
    const std::size_t dimensions(treeStructure.dimensions());
    const std::size_t numPointsHint(treeStructure.numPointsHint());
    const bool tubular(treeStructure.tubular());
    const bool dynamicChunks(true);
    const bool prefixIds(false);

    const std::size_t startDepth(
            std::max(minStartDepth, treeStructure.baseDepthBegin()));

    const std::size_t mappedDepth(baseDepth);

    const std::size_t sparseDepth(
        treeStructure.mappedDepthBegin() > startDepth ?
            treeStructure.mappedDepthBegin() - startDepth : 0);

    return Structure(
            nullDepth,
            baseDepth,
            coldDepth,
            pointsPerChunk,
            dimensions,
            numPointsHint,
            tubular,
            dynamicChunks,
            prefixIds,
            mappedDepth,
            startDepth,
            sparseDepth);
}

} // namespace entwine

