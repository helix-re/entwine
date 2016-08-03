/******************************************************************************
* Copyright (c) 2016, Connor Manning (connor@hobu.co)
*
* Entwine -- Point cloud indexing
*
* Entwine is available under the terms of the LGPL2 license. See COPYING
* for specific license text and more information.
*
******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <entwine/third/json/json.hpp>
#include <entwine/tree/hierarchy-block.hpp>
#include <entwine/tree/splitter.hpp>
#include <entwine/types/bounds.hpp>
#include <entwine/types/metadata.hpp>
#include <entwine/types/structure.hpp>
#include <entwine/util/json.hpp>
#include <entwine/util/spin-lock.hpp>

namespace entwine
{

class PointState;

class Hierarchy : public Splitter<HierarchyBlock>
{
public:
    Hierarchy(
            const Metadata& metadata,
            const arbiter::Endpoint& top,
            const arbiter::Endpoint* topOut,
            bool exists = false);

    ~Hierarchy()
    {
        m_base.t.reset();

        iterateCold([this](const Id& chunkId, const Slot& slot)
        {
            // We need to release our pooled block nodes back into our pool
            // so the parent destructor doesn't release them into a stale pool.
            slot.t.reset();
        });
    }

    using Splitter::tryGet;

    void countBase(std::size_t index, std::size_t tick, int delta)
    {
        m_base.t->count(index, tick, delta);
    }

    HierarchyCell& count(const PointState& state, int delta);

    uint64_t tryGet(const PointState& pointState) const;

    void save();

    void awakenAll()
    {
        iterateCold([this](const Id& chunkId, const Slot& slot)
        {
            slot.t = HierarchyBlock::create(
                    m_pool,
                    m_metadata,
                    chunkId,
                    m_outpoint.get(),
                    m_structure.getInfo(chunkId).pointsPerChunk(),
                    m_endpoint.getBinary(chunkId.str() + m_metadata.postfix()));
        });
    }

    void merge(const Hierarchy& other)
    {
        dynamic_cast<ContiguousBlock&>(*m_base.t).merge(
                dynamic_cast<const ContiguousBlock&>(*other.m_base.t));

        Splitter::merge(other.ids());
    }

    using Slots = std::set<const Slot*>;
    struct QueryResults
    {
        Json::Value json;
        Slots touched;
    };

    QueryResults query(
            const Bounds& queryBounds,
            std::size_t depthBegin,
            std::size_t depthEnd);

    static Structure structure(const Structure& treeStructure);

private:
    class Query
    {
    public:
        Query(
                const Bounds& bounds,
                std::size_t depthBegin,
                std::size_t depthEnd)
            : m_bounds(bounds)
            , m_depthBegin(depthBegin)
            , m_depthEnd(depthEnd)
        { }

        const Bounds& bounds() const { return m_bounds; }
        std::size_t depthBegin() const { return m_depthBegin; }
        std::size_t depthEnd() const { return m_depthEnd; }

    private:
        const Bounds m_bounds;
        const std::size_t m_depthBegin;
        const std::size_t m_depthEnd;
    };

    void traverse(
            Json::Value& json,
            Slots& ids,
            const Query& query,
            const PointState& pointState,
            std::deque<Dir>& lag);

    void accumulate(
            Json::Value& json,
            Slots& ids,
            const Query& query,
            const PointState& pointState,
            std::deque<Dir>& lag,
            uint64_t inc);

    void maybeTouch(Slots& ids, const PointState& pointState) const;

    mutable HierarchyCell::Pool m_pool;
    const Metadata& m_metadata;
    const Bounds& m_bounds;
    const Structure& m_structure;
    const arbiter::Endpoint m_endpoint;
    const std::unique_ptr<arbiter::Endpoint> m_outpoint;
};

} // namespace entwine

