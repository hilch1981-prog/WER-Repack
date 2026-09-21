/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - "Legend Roads" bot pathway network.
 *
 * A precomputed, slope-capped waypoint graph per continent. The generator
 * samples the world on a ~30yd grid (terrain + vmap heights, liquid), keeps
 * only stand-able points, and connects neighbours whose grade stays under a
 * configurable climb angle - so a route over this graph can NEVER
 * mountain-goat. Water is crossable but cost-weighted, so bots swim only
 * when the land detour would be far longer, and cross at the narrowest
 * point; lava, slime and fatigue (deep sea) water are never routed through.
 * Consumed by mod-playerbots (the Guide and MoveFarTo) through the extern
 * surface near the bottom.
 *
 * Sampling relies on Map::GetHeight/GetLiquidData, which only CREATE grids
 * (terrain + vmap + mmap tiles) - object/creature loading is a separate
 * path (MapGridManager::LoadGrid), so a build sweep spawns nothing. The
 * build runs as a CHUNKED job on the world thread (a time-budgeted slice
 * per world tick, in the same update phase where map workers idle at the
 * barrier): the server stays responsive and freeze detectors never trip.
 * Created grids linger until restart - restart when convenient afterwards.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "DBCStores.h"
#include "GridTerrainData.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "World.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    /* ------------------------------------------------------------------ */
    /*  Config                                                             */
    /* ------------------------------------------------------------------ */
    std::atomic<bool>  s_enabled{ true };
    std::atomic<bool>  s_allBots{ true };
    std::atomic<float> s_comfortDeg{ 20.0f };  // grade where climb cost starts biting
    std::atomic<float> s_climbWeight{ 10.0f }; // how hard routes avoid steep ground
    std::atomic<float> s_waterCost{ 5.0f };

    void LoadPathwaysConfig()
    {
        s_enabled = sConfigMgr->GetOption<bool>("WowLegends.BotPathways.Enabled", true);
        s_allBots = sConfigMgr->GetOption<bool>("WowLegends.BotPathways.AllBots", true);
        // MaxSlopeDegrees is the COMFORT knee, not a hard wall: steeper ground
        // is still traversable, just priced up. Hard-rejecting steep edges (the
        // old behaviour) shattered continents into thousands of islands because
        // a winding pass reads as a cliff in straight-line terms - the bot
        // walks each leg on the real navmesh, so an edge only needs the nodes
        // reachable, never the straight line gentle.
        float slope = sConfigMgr->GetOption<float>("WowLegends.BotPathways.MaxSlopeDegrees", 20.0f);
        s_comfortDeg = std::min(std::max(slope, 5.0f), 45.0f);
        float climb = sConfigMgr->GetOption<float>("WowLegends.BotPathways.ClimbWeight", 10.0f);
        s_climbWeight = std::min(std::max(climb, 0.0f), 100.0f);
        float water = sConfigMgr->GetOption<float>("WowLegends.BotPathways.WaterCostFactor", 5.0f);
        s_waterCost = std::min(std::max(water, 1.0f), 50.0f);
    }

    /* ------------------------------------------------------------------ */
    /*  Graph model                                                        */
    /* ------------------------------------------------------------------ */
    constexpr uint32 WLP_MAGIC   = 0x57504C57; // "WLPW"
    // v2 = soft-slope edge set + climb-weighted costs. Bumped so a stale v1
    // file (built by the old hard-slope-reject model - a shattered graph with
    // pure-geometric costs) is REJECTED on load rather than silently loaded,
    // which would re-goat and re-fragment. Bump this on any change to edge
    // selection or cost semantics, not just the byte layout.
    constexpr uint32 WLP_VERSION = 2;
    constexpr float  SPACING     = SIZE_OF_GRIDS / 18.0f; // ~29.63 yd
    constexpr float  HALF_MAP    = MAP_SIZE / 2.0f;
    constexpr uint32 EDGE_WATER  = 0x80000000; // high bit of neighbour index
    constexpr uint8  NODE_WATER  = 0x01;

    struct PwNode
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        uint8 flags = 0;
    };

    struct PathwayGraph
    {
        uint32 mapId = 0;
        std::vector<PwNode> nodes;
        // CSR adjacency: edges for node i live in [adjOffset[i], adjOffset[i+1])
        std::vector<uint32> adjOffset;
        std::vector<std::pair<uint32, float>> adjData; // (neighbour|EDGE_WATER, raw cost)
        // connected component per node: lets a route between disconnected
        // landmasses (Teldrassil -> Kalimdor mainland) fail in O(1) instead
        // of flooding A* to its expansion cap on a map worker thread
        std::vector<uint32> comp;
        // spatial hash (cellKey -> node indices), cell = 2*SPACING
        std::unordered_map<uint64, std::vector<uint32>> cells;

        static uint64 CellKey(float x, float y)
        {
            int32 const cx = int32(std::floor(x / (SPACING * 2.0f)));
            int32 const cy = int32(std::floor(y / (SPACING * 2.0f)));
            return (uint64(uint32(cx)) << 32) | uint64(uint32(cy));
        }

        void BuildCells()
        {
            cells.clear();
            for (uint32 i = 0; i < nodes.size(); ++i)
                cells[CellKey(nodes[i].x, nodes[i].y)].push_back(i);
        }

        void BuildComponents()
        {
            comp.assign(nodes.size(), 0xFFFFFFFF);
            uint32 next = 0;
            std::vector<uint32> stack;
            for (uint32 seed = 0; seed < nodes.size(); ++seed)
            {
                if (comp[seed] != 0xFFFFFFFF)
                    continue;
                comp[seed] = next;
                stack.push_back(seed);
                while (!stack.empty())
                {
                    uint32 const cur = stack.back();
                    stack.pop_back();
                    for (uint32 e = adjOffset[cur]; e < adjOffset[cur + 1]; ++e)
                    {
                        uint32 const nb = adjData[e].first & ~EDGE_WATER;
                        if (comp[nb] == 0xFFFFFFFF)
                        {
                            comp[nb] = next;
                            stack.push_back(nb);
                        }
                    }
                }
                ++next;
            }
        }

        // nearest stand-able node within maxDist (2D), z within 30yd so a
        // street query never snaps to the bridge deck above it
        uint32 Nearest(float x, float y, float z, float maxDist) const
        {
            uint32 best = 0xFFFFFFFF;
            float bestD = maxDist * maxDist;
            int32 const range = int32(maxDist / (SPACING * 2.0f)) + 1;
            int32 const cx = int32(std::floor(x / (SPACING * 2.0f)));
            int32 const cy = int32(std::floor(y / (SPACING * 2.0f)));
            for (int32 dx = -range; dx <= range; ++dx)
            {
                for (int32 dy = -range; dy <= range; ++dy)
                {
                    uint64 const key = (uint64(uint32(cx + dx)) << 32) | uint64(uint32(cy + dy));
                    auto it = cells.find(key);
                    if (it == cells.end())
                        continue;
                    for (uint32 idx : it->second)
                    {
                        PwNode const& n = nodes[idx];
                        if (std::fabs(n.z - z) > 30.0f)
                            continue;
                        float const d = (n.x - x) * (n.x - x) + (n.y - y) * (n.y - y);
                        if (d < bestD)
                        {
                            bestD = d;
                            best = idx;
                        }
                    }
                }
            }
            return best;
        }
    };

    // read-mostly registry: mutated only at startup and from the world
    // thread's update phase (map workers idle at the barrier in both
    // windows); map threads read routes through the shared_ptr they grab
    std::mutex s_graphMutex;
    std::unordered_map<uint32, std::shared_ptr<PathwayGraph const>> s_graphs;

    std::shared_ptr<PathwayGraph const> GetGraph(uint32 mapId)
    {
        std::lock_guard<std::mutex> lock(s_graphMutex);
        auto it = s_graphs.find(mapId);
        return it != s_graphs.end() ? it->second : nullptr;
    }

    // Route cache: on a busy server hundreds of bots stream toward the same
    // hubs (everyone heading to Orgrimmar), and each A* runs on the map thread.
    // Keying by a coarse 128yd start+goal cell lets bots in the same area
    // heading the same way reuse one computed route; NEGATIVE results (no
    // route) are cached too, so a known-unreachable pair never re-pays the
    // 20ms worst case. Graphs are immutable once built, so the cache is
    // cleared on (re)build/load; a 60s TTL bounds staleness/memory otherwise.
    //
    // Negative entries are SHORT-lived (15s) and only stored for STRUCTURAL
    // failures (off the network / disconnected islands). A budget bail
    // (deadline / expansion cap under load) is NOT "no route" and is never
    // cached - caching it poisoned every retry from the same 128yd cell for
    // a full minute, which field-tested as "the guide walks in circles and
    // never recovers" (Kneuma, Durotar->Tanaris, 2026-07-15).
    struct RouteCacheEntry
    {
        std::vector<std::array<float, 3>> path; // empty = cached "no route"
        uint32 ts = 0;
    };
    std::mutex s_routeCacheMutex;
    std::unordered_map<uint64, RouteCacheEntry> s_routeCache;

    uint64 RouteKey(uint32 mapId, float sx, float sy, float dx, float dy)
    {
        auto q = [](float v) -> uint64
        {
            // 128yd cells, +512 offset keeps the full +-17066 map range
            // inside 10 bits (0..1023)
            int32 const c = int32(std::floor(v / 128.0f)) + 512;
            return uint64(std::min(std::max(c, 0), 1023));
        };
        return (uint64(mapId) << 40) | (q(sx) << 30) | (q(sy) << 20)
             | (q(dx) << 10) | q(dy);
    }

    void ClearRouteCache()
    {
        std::lock_guard<std::mutex> lock(s_routeCacheMutex);
        s_routeCache.clear();
    }

    /* ------------------------------------------------------------------ */
    /*  Liquid classification (nodes AND edge midpoints)                   */
    /* ------------------------------------------------------------------ */
    struct LiquidSample
    {
        bool reject = false; // lava, slime, fatigue water, deep open sea
        bool water = false;  // swim-depth: node z snaps to the surface
        float surface = 0.0f;
    };

    LiquidSample SampleLiquid(Map* map, float x, float y, float z)
    {
        LiquidSample out;
        LiquidData const liquid = map->GetLiquidData(PHASEMASK_NORMAL, x, y, z + 2.0f, 2.03f, {});
        if (liquid.Level <= INVALID_HEIGHT)
            return out;

        // lava/slime only counts when its surface is at or above our feet -
        // a dry deck or ledge safely ABOVE it must stay routable
        if ((liquid.Flags & (MAP_LIQUID_TYPE_MAGMA | MAP_LIQUID_TYPE_SLIME))
            && liquid.Level > z - 2.0f)
        {
            out.reject = true;
            return out;
        }

        float const depth = liquid.Level - z;
        if (depth > 1.6f)
        {
            // fatigue water kills swimmers; deep open sea is not a road.
            // Coasts, rivers and lakes stay crossable at the surface.
            if (liquid.Flags & MAP_LIQUID_TYPE_DARK_WATER)
            {
                out.reject = true;
                return out;
            }
            if ((liquid.Flags & MAP_LIQUID_TYPE_OCEAN) && depth > 100.0f)
            {
                out.reject = true;
                return out;
            }
            out.water = true;
            out.surface = liquid.Level;
        }
        return out;
    }

    /* ------------------------------------------------------------------ */
    /*  File IO                                                            */
    /* ------------------------------------------------------------------ */
    std::string PathwaysDir()
    {
        return sWorld->GetDataPath() + "pathways/";
    }

    std::string PathwaysFile(uint32 mapId)
    {
        return PathwaysDir() + "map" + std::to_string(mapId) + ".wlp";
    }

    bool SaveGraph(PathwayGraph const& g)
    {
        std::error_code ec;
        std::filesystem::create_directories(PathwaysDir(), ec);
        // write to a temp name and rename over the old file only once the
        // new one is complete - a failed build must never destroy good data
        std::string const finalName = PathwaysFile(g.mapId);
        std::string const tmpName = finalName + ".tmp";
        {
            std::ofstream f(tmpName, std::ios::binary | std::ios::trunc);
            if (!f)
                return false;

            uint32 const nodeCount = uint32(g.nodes.size());
            uint32 const adjCount = uint32(g.adjData.size());
            f.write(reinterpret_cast<char const*>(&WLP_MAGIC), 4);
            f.write(reinterpret_cast<char const*>(&WLP_VERSION), 4);
            f.write(reinterpret_cast<char const*>(&g.mapId), 4);
            f.write(reinterpret_cast<char const*>(&nodeCount), 4);
            f.write(reinterpret_cast<char const*>(&adjCount), 4);
            for (PwNode const& n : g.nodes)
            {
                f.write(reinterpret_cast<char const*>(&n.x), 4);
                f.write(reinterpret_cast<char const*>(&n.y), 4);
                f.write(reinterpret_cast<char const*>(&n.z), 4);
                f.write(reinterpret_cast<char const*>(&n.flags), 1);
            }
            f.write(reinterpret_cast<char const*>(g.adjOffset.data()), std::streamsize(g.adjOffset.size() * 4));
            for (auto const& e : g.adjData)
            {
                f.write(reinterpret_cast<char const*>(&e.first), 4);
                f.write(reinterpret_cast<char const*>(&e.second), 4);
            }
            if (!f.good())
                return false;
        }
        std::filesystem::rename(tmpName, finalName, ec);
        return !ec;
    }

    std::shared_ptr<PathwayGraph> LoadGraphFile(uint32 mapId)
    {
        std::ifstream f(PathwaysFile(mapId), std::ios::binary);
        if (!f)
            return nullptr;

        uint32 magic = 0;
        uint32 version = 0;
        uint32 fileMap = 0;
        uint32 nodeCount = 0;
        uint32 adjCount = 0;
        f.read(reinterpret_cast<char*>(&magic), 4);
        f.read(reinterpret_cast<char*>(&version), 4);
        f.read(reinterpret_cast<char*>(&fileMap), 4);
        f.read(reinterpret_cast<char*>(&nodeCount), 4);
        f.read(reinterpret_cast<char*>(&adjCount), 4);
        if (!f.good() || magic != WLP_MAGIC || version != WLP_VERSION || fileMap != mapId)
        {
            LOG_ERROR("server.loading", "[pathways] {} is corrupt or wrong version - ignoring", PathwaysFile(mapId));
            return nullptr;
        }
        // sanity caps: ~34k x 34k map at ~30yd spacing can never exceed these
        if (nodeCount > 6000000 || adjCount > 60000000)
            return nullptr;

        auto g = std::make_shared<PathwayGraph>();
        g->mapId = mapId;
        g->nodes.resize(nodeCount);
        for (PwNode& n : g->nodes)
        {
            f.read(reinterpret_cast<char*>(&n.x), 4);
            f.read(reinterpret_cast<char*>(&n.y), 4);
            f.read(reinterpret_cast<char*>(&n.z), 4);
            f.read(reinterpret_cast<char*>(&n.flags), 1);
        }
        g->adjOffset.resize(size_t(nodeCount) + 1);
        f.read(reinterpret_cast<char*>(g->adjOffset.data()), std::streamsize(g->adjOffset.size() * 4));
        g->adjData.resize(adjCount);
        for (auto& e : g->adjData)
        {
            f.read(reinterpret_cast<char*>(&e.first), 4);
            f.read(reinterpret_cast<char*>(&e.second), 4);
        }
        if (!f.good() || g->adjOffset.back() != adjCount)
        {
            LOG_ERROR("server.loading", "[pathways] {} truncated - ignoring", PathwaysFile(mapId));
            return nullptr;
        }
        // validate structure before any map thread runs A* over it: offsets
        // monotonic, every neighbour index in range - a corrupt file must
        // fail load, not crash a worker thread later
        for (size_t i = 0; i + 1 < g->adjOffset.size(); ++i)
        {
            if (g->adjOffset[i] > g->adjOffset[i + 1])
            {
                LOG_ERROR("server.loading", "[pathways] {} has corrupt adjacency offsets - ignoring", PathwaysFile(mapId));
                return nullptr;
            }
        }
        for (auto const& e : g->adjData)
        {
            if ((e.first & ~EDGE_WATER) >= nodeCount)
            {
                LOG_ERROR("server.loading", "[pathways] {} has out-of-range neighbour indices - ignoring", PathwaysFile(mapId));
                return nullptr;
            }
        }
        g->BuildCells();
        g->BuildComponents();
        return g;
    }

    /* ------------------------------------------------------------------ */
    /*  Chunked generator (time-budgeted slices on the world thread)       */
    /* ------------------------------------------------------------------ */
    constexpr uint32 BUILD_SLICE_MS = 30; // per world tick

    // absolute per-step vertical guard: two grid-adjacent nodes more than this
    // far apart in Z are a cliff/teleport artifact, not walkable terrain -
    // dropped outright. Everything gentler is kept and priced by climb cost.
    constexpr float CLIFF_DZ = 45.0f;

    // A gorge/ravine narrower than the sample spacing sits BETWEEN two nodes
    // of near-equal rim height: bestDz is ~0 so the edge looks flat and cheap,
    // yet no bot can walk rim-to-rim across it. Reject when the terrain along
    // the edge dips this far BELOW the straight line joining the endpoints (a
    // downward chasm). One-sided on purpose: a midpoint at or ABOVE the line
    // is a winding pass/hill the navmesh handles - those must survive.
    constexpr float GORGE_DIP = SIZE_OF_GRIDS / 30.0f; // ~17.8 yd

    struct BuildJob
    {
        uint32 mapId = 0;
        int phase = 0;   // 0 = nodes, 1 = edges, 2 = finalize
        uint32 iy = 0;   // row cursor for phases 0/1
        uint32 maxIdx = 0;
        float tanComfort = 0.0f; // tan(MaxSlopeDegrees): climb-cost knee
        float climbWeight = 0.0f;
        uint32 startMs = 0;

        std::shared_ptr<PathwayGraph> g;
        std::unordered_map<uint64, uint32> keyToNode;
        std::vector<std::vector<std::pair<uint32, float>>> adj;

        // stats
        uint32 samples = 0;
        uint32 waterNodes = 0;
        uint32 edges = 0;
        uint32 cliffRejects = 0; // edges dropped for a >CLIFF_DZ vertical step
        uint32 gapRejects = 0;
        uint32 liquidRejects = 0;
    };

    std::unique_ptr<BuildJob> s_job; // world thread only

    uint64 NodeKey(uint32 ix, uint32 iy, uint32 lvl)
    {
        return (uint64(ix) << 24) | (uint64(iy) << 4) | uint64(lvl);
    }

    void BuildNodesRow(Map* map, BuildJob& job)
    {
        float const y = HALF_MAP - float(job.iy) * SPACING;
        for (uint32 ix = 0; ix <= job.maxIdx; ++ix)
        {
            float const x = HALF_MAP - float(ix) * SPACING;
            ++job.samples;

            float const z1 = map->GetHeight(x, y, 3000.0f, true, 4000.0f);
            if (z1 <= INVALID_HEIGHT)
                continue;

            float levels[2];
            uint32 levelCount = 1;
            levels[0] = z1;
            float const z2 = map->GetHeight(x, y, z1 - 8.0f, true, 4000.0f);
            if (z2 > INVALID_HEIGHT && z1 - z2 > 8.0f)
                levels[levelCount++] = z2;

            float placedZ[2] = { 0.0f, 0.0f };
            uint32 placed = 0;
            for (uint32 lvl = 0; lvl < levelCount; ++lvl)
            {
                float z = levels[lvl];
                uint8 flags = 0;
                LiquidSample const liq = SampleLiquid(map, x, y, z);
                if (liq.reject)
                {
                    ++job.liquidRejects;
                    continue;
                }
                if (liq.water)
                {
                    z = liq.surface;
                    flags |= NODE_WATER;
                }
                // both probe levels can collapse onto the same surface (e.g.
                // a submerged deck and the seabed both snapping to the water
                // surface) - keep one node, not coincident duplicates
                bool dup = false;
                for (uint32 p = 0; p < placed; ++p)
                    if (std::fabs(placedZ[p] - z) < 2.0f)
                        dup = true;
                if (dup)
                    continue;

                job.keyToNode[NodeKey(ix, job.iy, lvl)] = uint32(job.g->nodes.size());
                job.g->nodes.push_back({ x, y, z, flags });
                placedZ[placed++] = z;
                if (flags & NODE_WATER)
                    ++job.waterNodes;
            }
        }
    }

    void BuildEdgesRow(Map* map, BuildJob& job)
    {
        static int32 const DIRS[4][2] = { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 1, -1 } };

        auto nodeAt = [&](uint32 ix, uint32 iy, uint32 lvl) -> int64
        {
            auto it = job.keyToNode.find(NodeKey(ix, iy, lvl));
            return it == job.keyToNode.end() ? -1 : int64(it->second);
        };

        uint32 const iy = job.iy;
        for (uint32 ix = 0; ix <= job.maxIdx; ++ix)
        {
            for (uint32 lvl = 0; lvl < 2; ++lvl)
            {
                int64 const ai = nodeAt(ix, iy, lvl);
                if (ai < 0)
                    continue;
                PwNode const& a = job.g->nodes[size_t(ai)];

                for (auto const& d : DIRS)
                {
                    int64 const nx = int64(ix) + d[0];
                    int64 const ny = int64(iy) + d[1];
                    if (nx < 0 || ny < 0 || nx > int64(job.maxIdx) || ny > int64(job.maxIdx))
                        continue;

                    // pick the neighbour level closest in height
                    int64 bi = -1;
                    float bestDz = 1.0e9f;
                    for (uint32 nlvl = 0; nlvl < 2; ++nlvl)
                    {
                        int64 const ci = nodeAt(uint32(nx), uint32(ny), nlvl);
                        if (ci < 0)
                            continue;
                        float const dz = std::fabs(job.g->nodes[size_t(ci)].z - a.z);
                        if (dz < bestDz)
                        {
                            bestDz = dz;
                            bi = ci;
                        }
                    }
                    if (bi < 0)
                        continue;
                    PwNode const& b = job.g->nodes[size_t(bi)];

                    float const run = std::sqrt((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
                    // only a genuine cliff/artifact is rejected outright. Steep
                    // walkable ground is KEPT and priced by climb cost below -
                    // hard-rejecting on straight-line grade shattered the graph
                    // (a winding pass climbs gently but reads steep crow-flies).
                    if (bestDz > CLIFF_DZ)
                    {
                        ++job.cliffRejects;
                        continue;
                    }

                    bool waterEdge = (a.flags & NODE_WATER) || (b.flags & NODE_WATER);
                    float const mx = (a.x + b.x) * 0.5f;
                    float const my = (a.y + b.y) * 0.5f;

                    if (!waterEdge)
                    {
                        // sanity-sample the ground under the edge at 1/4, 1/2,
                        // 3/4. Reject on: a TRUE void (no ground = a chasm the
                        // bot falls into), or a downward DIP far below the
                        // straight line (a narrow gorge/ravine between two
                        // near-equal rims - reads flat and cheap but is not
                        // walkable rim-to-rim). One-sided: terrain AT or ABOVE
                        // the line is a winding pass/hill the navmesh handles.
                        bool bad = false;
                        float worstDip = -1.0e9f; // first sample always wins
                        float dipMx = mx;
                        float dipMy = my;
                        float dipHm = a.z;
                        for (int s = 1; s <= 3; ++s)
                        {
                            float const t = float(s) * 0.25f;
                            float const px = a.x + (b.x - a.x) * t;
                            float const py = a.y + (b.y - a.y) * t;
                            float const lineZ = a.z + (b.z - a.z) * t;
                            float const hs = map->GetHeight(px, py, lineZ + 6.0f, true, 40.0f);
                            if (hs <= INVALID_HEIGHT)
                            {
                                bad = true;
                                break;
                            }
                            float const dip = lineZ - hs; // >0 = terrain below line
                            if (dip > worstDip)
                            {
                                worstDip = dip;
                                dipMx = px;
                                dipMy = py;
                                dipHm = hs;
                            }
                        }
                        if (bad)
                        {
                            ++job.gapRejects;
                            continue;
                        }
                        if (worstDip > GORGE_DIP)
                        {
                            ++job.gapRejects;
                            continue;
                        }
                        // liquid under the edge (deepest sampled point): a lava
                        // stream or a river narrower than the spacing must not
                        // become an unflagged land link
                        LiquidSample const liq = SampleLiquid(map, dipMx, dipMy, dipHm);
                        if (liq.reject)
                        {
                            ++job.liquidRejects;
                            continue;
                        }
                        if (liq.water)
                            waterEdge = true;
                    }
                    else
                    {
                        // water edges skip the height probe (the surface IS
                        // flat) but still refuse lava/fatigue at the midpoint
                        float const mzWater = std::max(a.z, b.z);
                        LiquidSample const liq = SampleLiquid(map, mx, my, mzWater - 1.0f);
                        if (liq.reject)
                        {
                            ++job.liquidRejects;
                            continue;
                        }
                    }

                    // climb-weighted cost: flat ground costs its true distance,
                    // steeper ground grows quadratically so A* hugs valleys and
                    // roads (the flattest corridors) and only ever takes a steep
                    // edge when it is the sole way through. Water stays a
                    // query-time multiplier (EDGE_WATER) so it is live-tunable.
                    float const grade = bestDz / run;
                    float const knee = grade / job.tanComfort;
                    float const cost = run * (1.0f + job.climbWeight * knee * knee);
                    uint32 const mark = waterEdge ? EDGE_WATER : 0;
                    job.adj[size_t(ai)].push_back({ uint32(bi) | mark, cost });
                    job.adj[size_t(bi)].push_back({ uint32(ai) | mark, cost });
                    ++job.edges;
                }
            }
        }
    }

    void FinishBuild(BuildJob& job)
    {
        PathwayGraph& g = *job.g;
        g.adjOffset.assign(g.nodes.size() + 1, 0);
        for (size_t i = 0; i < job.adj.size(); ++i)
            g.adjOffset[i + 1] = g.adjOffset[i] + uint32(job.adj[i].size());
        g.adjData.reserve(g.adjOffset.back());
        for (auto const& list : job.adj)
            for (auto const& e : list)
                g.adjData.push_back(e);
        g.BuildCells();
        g.BuildComponents();

        uint32 const tookMs = GetMSTimeDiffToNow(job.startMs);
        if (g.nodes.empty())
        {
            LOG_ERROR("server", "[pathways] map {} build produced no walkable nodes - nothing saved", job.mapId);
            return;
        }
        if (!SaveGraph(g))
        {
            LOG_ERROR("server", "[pathways] failed to write {} - check permissions", PathwaysFile(job.mapId));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(s_graphMutex);
            s_graphs[job.mapId] = job.g;
        }
        ClearRouteCache(); // the new graph invalidates any cached routes
        LOG_INFO("server", "[pathways] map {} built and live: {} nodes ({} water), {} edges, {:.1f}s "
                 "({} samples, {} cliff, {} gap, {} liquid rejects). Saved to {}. "
                 "Restart when convenient - the sweep left terrain grids created.",
                 job.mapId, uint32(g.nodes.size()), job.waterNodes, job.edges, tookMs / 1000.0f,
                 job.samples, job.cliffRejects, job.gapRejects, job.liquidRejects,
                 PathwaysFile(job.mapId));
    }

    // one time-budgeted slice per world tick; returns true while running
    void AdvanceBuildJob()
    {
        if (!s_job)
            return;

        Map* map = sMapMgr->FindBaseMap(s_job->mapId);
        if (!map)
        {
            LOG_ERROR("server", "[pathways] map {} vanished mid-build - job abandoned", s_job->mapId);
            s_job.reset();
            return;
        }

        try
        {
            uint32 const sliceStart = getMSTime();
            while (GetMSTimeDiffToNow(sliceStart) < BUILD_SLICE_MS)
            {
                if (s_job->phase == 0)
                {
                    BuildNodesRow(map, *s_job);
                    if (++s_job->iy > s_job->maxIdx)
                    {
                        s_job->phase = 1;
                        s_job->iy = 0;
                        s_job->adj.resize(s_job->g->nodes.size());
                        LOG_INFO("server", "[pathways] map {}: {} nodes sampled, linking...",
                                 s_job->mapId, uint32(s_job->g->nodes.size()));
                    }
                }
                else if (s_job->phase == 1)
                {
                    BuildEdgesRow(map, *s_job);
                    if (++s_job->iy > s_job->maxIdx)
                        s_job->phase = 2;
                }
                else
                {
                    FinishBuild(*s_job);
                    s_job.reset();
                    return;
                }
            }
        }
        catch (std::bad_alloc const&)
        {
            LOG_ERROR("server", "[pathways] out of memory building map {} - job abandoned", s_job->mapId);
            s_job.reset();
        }
    }

    /* ------------------------------------------------------------------ */
    /*  A* query (thread-safe: graph is read-only, all state is local)     */
    /* ------------------------------------------------------------------ */
    // Why a route query came back empty. STRUCTURAL reasons are stable facts
    // about the position pair (cacheable); BUDGET is a transient bail under
    // load and must never be cached as "no route".
    enum class RouteFail : uint8
    {
        None,
        NoEntry,   // no graph node within reach of the start
        NoExit,    // no graph node within reach of the goal
        SameNode,  // both endpoints resolve to one node - below resolution
        Island,    // start and goal on disconnected components
        Budget     // deadline / expansion cap hit - transient
    };

    char const* RouteFailName(RouteFail f)
    {
        switch (f)
        {
            case RouteFail::NoEntry:  return "no road near start";
            case RouteFail::NoExit:   return "no road near goal";
            case RouteFail::SameNode: return "trip below network resolution";
            case RouteFail::Island:   return "disconnected landmasses";
            case RouteFail::Budget:   return "budget bail (transient)";
            default:                  return "ok";
        }
    }

    bool RouteOnGraph(PathwayGraph const& g, float sx, float sy, float sz,
                      float dx, float dy, float dz,
                      std::vector<std::array<float, 3>>& out, RouteFail& why)
    {
        why = RouteFail::None;
        // 180yd entry reach: the bot navmesh-walks to the first node anyway,
        // and 90yd field-tested too tight - starts inside buildings/rough
        // terrain missed the network entirely and the whole trip fell to
        // live navmesh stepping (the "circles" failure mode)
        uint32 const start = g.Nearest(sx, sy, sz, 180.0f);
        uint32 const goal = g.Nearest(dx, dy, dz, 180.0f);
        if (start == 0xFFFFFFFF)
        {
            why = RouteFail::NoEntry;
            return false;
        }
        if (goal == 0xFFFFFFFF)
        {
            why = RouteFail::NoExit;
            return false;
        }
        if (start == goal)
        {
            why = RouteFail::SameNode;
            return false;
        }
        // disconnected landmasses: fail instantly, never flood the queue
        if (g.comp[start] != g.comp[goal])
        {
            why = RouteFail::Island;
            return false;
        }

        float const waterFactor = s_waterCost.load();
        // The heuristic is the true 2D straight-line distance to the goal
        // (min cost per yard is 1.0 on flat ground, so this never
        // overestimates - admissible). Weighting it UP is counter-productive
        // here: measured, w=1.3 explored 10x more nodes than w=1.0 because
        // inflated climb costs make a weighted heuristic overshoot on the
        // flat stretches and thrash. So keep it exact.
        auto heuristic = [&](uint32 i)
        {
            PwNode const& n = g.nodes[i];
            float const hx = n.x - g.nodes[goal].x;
            float const hy = n.y - g.nodes[goal].y;
            return std::sqrt(hx * hx + hy * hy) * 0.999f;
        };

        std::unordered_map<uint32, float> gScore;
        std::unordered_map<uint32, uint32> parent;
        using QItem = std::pair<float, uint32>;
        std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> open;

        gScore[start] = 0.0f;
        open.push({ heuristic(start), start });

        // A* runs synchronously on the bot's own map-update thread, so a
        // pathological query must never sweep a whole 190k-node component in
        // one tick. Two guards: an expansion cap (memory/loop backstop) AND a
        // wall-clock budget - a genuine cross-continent route measured ~27k
        // pops (~2ms native), so 20ms is >>10x headroom yet bounds the tick
        // hit. On either limit we bail; the caller falls back to live
        // pathfinding, so this is a safe degradation, never a wrong answer.
        uint32 const deadline = getMSTime() + 20;
        uint32 expanded = 0;
        bool found = false;
        while (!open.empty())
        {
            auto [f, cur] = open.top();
            open.pop();
            if (cur == goal)
            {
                found = true;
                break;
            }
            if (++expanded > 200000)
                break; // pathological query - bail out, caller falls back
            if ((expanded & 4095) == 0 && getMSTime() > deadline)
                break; // over the per-query wall-clock budget - bail out

            float const curG = gScore[cur];
            if (f - heuristic(cur) > curG + 0.01f)
                continue; // stale queue entry

            for (uint32 e = g.adjOffset[cur]; e < g.adjOffset[cur + 1]; ++e)
            {
                auto const& [rawIdx, rawCost] = g.adjData[e];
                uint32 const next = rawIdx & ~EDGE_WATER;
                float const cost = (rawIdx & EDGE_WATER) ? rawCost * waterFactor : rawCost;
                float const cand = curG + cost;
                auto it = gScore.find(next);
                if (it != gScore.end() && it->second <= cand)
                    continue;
                gScore[next] = cand;
                parent[next] = cur;
                open.push({ cand + heuristic(next), next });
            }
        }

        if (!found)
        {
            why = RouteFail::Budget;
            return false;
        }

        std::vector<uint32> chain;
        uint32 cur = goal;
        while (true)
        {
            chain.push_back(cur);
            if (cur == start)
                break;
            cur = parent[cur];
        }

        out.clear();
        out.reserve(chain.size());
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        {
            PwNode const& n = g.nodes[*it];
            out.push_back({ n.x, n.y, n.z });
        }
        return true;
    }

    /* ------------------------------------------------------------------ */
    /*  Startup loader                                                     */
    /* ------------------------------------------------------------------ */
    // strict "map<digits>.wlp" - sscanf's %u accepts prefixes/overflow
    bool ParsePathwayFilename(std::string const& name, uint32& mapId)
    {
        if (name.size() < 8 || name.size() > 13) // map0.wlp .. map99999999.wlp is absurd; cap digits at 6
            return false;
        if (name.compare(0, 3, "map") != 0)
            return false;
        if (name.compare(name.size() - 4, 4, ".wlp") != 0)
            return false;
        std::string const digits = name.substr(3, name.size() - 7);
        if (digits.empty() || digits.size() > 6)
            return false;
        uint32 value = 0;
        for (char c : digits)
        {
            if (c < '0' || c > '9')
                return false;
            value = value * 10 + uint32(c - '0');
        }
        mapId = value;
        return true;
    }

    void LoadAllGraphs()
    {
        std::error_code ec;
        if (!std::filesystem::exists(PathwaysDir(), ec))
        {
            LOG_INFO("server.loading", "[pathways] no pathway data at {} - bots use live pathfinding only", PathwaysDir());
            return;
        }
        for (auto const& entry : std::filesystem::directory_iterator(PathwaysDir(), ec))
        {
            std::string const name = entry.path().filename().string();
            uint32 mapId = 0;
            if (!ParsePathwayFilename(name, mapId))
                continue;
            if (auto g = LoadGraphFile(mapId))
            {
                LOG_INFO("server.loading", "[pathways] map {}: {} nodes, {} edges loaded",
                         mapId, uint32(g->nodes.size()), uint32(g->adjData.size() / 2));
                std::lock_guard<std::mutex> lock(s_graphMutex);
                s_graphs[mapId] = std::move(g);
            }
        }
        ClearRouteCache();
    }
}

namespace
{
    /* ------------------------------------------------------------------ */
    /*  Async route jobs (guide escorts)                                   */
    /* ------------------------------------------------------------------ */
    // Long mountain routes on the climb-inflated graph legitimately need
    // 150k+ A* pops (measured: Razor Hill -> Desolace = 161k on the x22
    // build) - too heavy for a map-tick budget. Guide routes therefore run
    // as RESUMABLE jobs sliced on the world tick (map workers idle at the
    // barrier - the same window the graph builder uses), with NO deadline:
    // only an absolute expansion cap as a runaway backstop. Map threads
    // request and poll through a mutex; the world thread computes.
    struct RouteJob
    {
        uint32 mapId = 0;
        float sx = 0, sy = 0, sz = 0, dx = 0, dy = 0, dz = 0;
        std::shared_ptr<PathwayGraph const> graph; // pinned for the job
        // resumable A* state
        std::unordered_map<uint32, float> gScore;
        std::unordered_map<uint32, uint32> parent;
        std::priority_queue<std::pair<float, uint32>,
            std::vector<std::pair<float, uint32>>,
            std::greater<std::pair<float, uint32>>> open;
        uint32 start = 0, goal = 0;
        uint32 expanded = 0;
        bool started = false;
        // result
        int state = 0; // 0 running, 1 done-ok, -1 done-fail
        std::vector<std::array<float, 3>> path;
        char const* why = "pending";
        uint32 doneMs = 0;
    };
    std::mutex s_jobMutex;
    std::unordered_map<uint32, std::shared_ptr<RouteJob>> s_jobs;
    std::deque<uint32> s_jobQueue;
    uint32 s_nextJobId = 1;
    constexpr uint32 JOB_CAP_EXPANSIONS = 1200000; // runaway backstop only
    constexpr std::size_t JOB_MAX_QUEUED = 8;

    // one time-boxed slice of one job; world thread only
    void AdvanceRouteJobs()
    {
        std::shared_ptr<RouteJob> job;
        {
            std::lock_guard<std::mutex> lock(s_jobMutex);
            // GC finished jobs nobody collected (requester died/retargeted)
            uint32 const now = getMSTime();
            for (auto it = s_jobs.begin(); it != s_jobs.end();)
            {
                if (it->second->state != 0 && now - it->second->doneMs > 30000)
                    it = s_jobs.erase(it);
                else
                    ++it;
            }
            while (!s_jobQueue.empty())
            {
                auto it = s_jobs.find(s_jobQueue.front());
                if (it == s_jobs.end() || it->second->state != 0)
                {
                    s_jobQueue.pop_front();
                    continue;
                }
                job = it->second;
                break;
            }
        }
        if (!job)
            return;

        PathwayGraph const& g = *job->graph;
        if (!job->started)
        {
            job->started = true;
            uint32 const start = g.Nearest(job->sx, job->sy, job->sz, 180.0f);
            uint32 const goal = g.Nearest(job->dx, job->dy, job->dz, 180.0f);
            RouteFail why = RouteFail::None;
            if (start == 0xFFFFFFFF)
                why = RouteFail::NoEntry;
            else if (goal == 0xFFFFFFFF)
                why = RouteFail::NoExit;
            else if (start == goal)
                why = RouteFail::SameNode;
            else if (g.comp[start] != g.comp[goal])
                why = RouteFail::Island;
            if (why != RouteFail::None)
            {
                // structural failures are cacheable (short TTL). Cache and
                // job mutexes are never held together (lock-order hygiene).
                // WL FIX (2026-07-20): EXCEPT NoEntry. RouteKey quantizes x/y
                // to 128yd cells and ignores z entirely, but NoEntry comes from
                // a nearest-node probe with a 30yd z filter - so it depends on
                // the caller's HEIGHT, not on the cell. Caching it let a player
                // in a cave (or under a bridge / on a lower city tier) poison
                // every other player in that cell for 15s, including someone
                // standing on the road directly above.
                if (why != RouteFail::NoEntry)
                {
                    uint64 const key = RouteKey(job->mapId, job->sx, job->sy, job->dx, job->dy);
                    std::lock_guard<std::mutex> lock(s_routeCacheMutex);
                    s_routeCache[key] = { {}, getMSTime() };
                }
                std::lock_guard<std::mutex> lock(s_jobMutex);
                job->state = -1;
                job->why = RouteFailName(why);
                job->doneMs = getMSTime();
                return;
            }
            job->start = start;
            job->goal = goal;
            job->gScore[start] = 0.0f;
            float const hx = g.nodes[start].x - g.nodes[goal].x;
            float const hy = g.nodes[start].y - g.nodes[goal].y;
            job->open.push({ std::sqrt(hx * hx + hy * hy) * 0.999f, start });
        }

        float const waterFactor = s_waterCost.load();
        auto heuristic = [&](uint32 i)
        {
            float const hx = g.nodes[i].x - g.nodes[job->goal].x;
            float const hy = g.nodes[i].y - g.nodes[job->goal].y;
            return std::sqrt(hx * hx + hy * hy) * 0.999f;
        };

        uint32 const sliceEnd = getMSTime() + 4; // ~4ms per world tick
        bool found = false;
        bool exhausted = false;
        while (!job->open.empty())
        {
            auto [f, cur] = job->open.top();
            if (cur == job->goal)
            {
                found = true;
                break;
            }
            job->open.pop();
            if (++job->expanded > JOB_CAP_EXPANSIONS)
            {
                exhausted = true;
                break;
            }
            float const curG = job->gScore[cur];
            if (f - heuristic(cur) > curG + 0.01f)
            {
                if ((job->expanded & 1023) == 0 && getMSTime() >= sliceEnd)
                    return; // resume next tick
                continue;
            }
            for (uint32 e = g.adjOffset[cur]; e < g.adjOffset[cur + 1]; ++e)
            {
                auto const& [rawIdx, rawCost] = g.adjData[e];
                uint32 const next = rawIdx & ~EDGE_WATER;
                float const cost = (rawIdx & EDGE_WATER) ? rawCost * waterFactor : rawCost;
                float const cand = curG + cost;
                auto it = job->gScore.find(next);
                if (it != job->gScore.end() && it->second <= cand)
                    continue;
                job->gScore[next] = cand;
                job->parent[next] = cur;
                job->open.push({ cand + heuristic(next), next });
            }
            if ((job->expanded & 1023) == 0 && getMSTime() >= sliceEnd)
                return; // resume next tick
        }
        // loop exit invariants: found, exhausted (cap), or open drained -
        // every mid-slice pause RETURNS from inside the loop directly

        // reconstruct + free the A* scratch BEFORE taking the mutex: the
        // scratch holds hundreds of thousands of hash nodes on long routes
        // and only the world thread ever touches it - freeing it under the
        // contended job mutex stalled every poller (review catch)
        std::vector<std::array<float, 3>> path;
        if (found)
        {
            std::vector<uint32> chain;
            uint32 cur = job->goal;
            while (true)
            {
                chain.push_back(cur);
                if (cur == job->start)
                    break;
                cur = job->parent[cur];
            }
            path.reserve(chain.size());
            for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            {
                PwNode const& n = g.nodes[*it];
                path.push_back({ n.x, n.y, n.z });
            }
        }
        job->gScore = {};
        job->parent = {};
        job->open = {};

        // cache BEFORE publishing to the job: the moment state flips, a
        // poller may move job->path out - the cache must copy from the
        // local vector, never from the published job
        if (found)
        {
            uint64 const key = RouteKey(job->mapId, job->sx, job->sy, job->dx, job->dy);
            std::lock_guard<std::mutex> lock(s_routeCacheMutex);
            if (s_routeCache.size() > 2048)
                s_routeCache.clear();
            s_routeCache[key] = { path, getMSTime() };
        }

        std::lock_guard<std::mutex> lock(s_jobMutex);
        job->doneMs = getMSTime();
        if (!found)
        {
            job->state = -1;
            job->why = "search exhausted"; // cap or empty open set
            return;
        }
        job->path = std::move(path);
        job->state = 1;
        job->why = "ok";
    }
}

/* ---------------------------------------------------------------------- */
/*  Extern surface (consumed by mod-playerbots via declaration)            */
/* ---------------------------------------------------------------------- */
bool WlBotPathwaysEnabled()
{
    return s_enabled.load(std::memory_order_relaxed);
}

bool WlBotPathwaysAllBots()
{
    return s_allBots.load(std::memory_order_relaxed);
}

bool WlPathwaysAvailable(uint32 mapId)
{
    if (!s_enabled.load(std::memory_order_relaxed))
        return false;
    return GetGraph(mapId) != nullptr;
}

bool WlPathwaysRoute(uint32 mapId, float sx, float sy, float sz,
                     float dx, float dy, float dz,
                     std::vector<std::array<float, 3>>& out,
                     char const** whyOut)
{
    if (whyOut)
        *whyOut = "pathways disabled";
    if (!s_enabled.load(std::memory_order_relaxed))
        return false;
    std::shared_ptr<PathwayGraph const> g = GetGraph(mapId);
    if (!g)
    {
        if (whyOut)
            *whyOut = "no graph for this map";
        return false;
    }

    uint64 const key = RouteKey(mapId, sx, sy, dx, dy);
    uint32 const now = getMSTime();
    {
        std::lock_guard<std::mutex> lock(s_routeCacheMutex);
        auto it = s_routeCache.find(key);
        if (it != s_routeCache.end())
        {
            // negatives expire fast (15s): the world did not change, but the
            // BOT moves - the next try from a slightly different spot must
            // get a fresh answer, not last minute's failure
            uint32 const ttl = it->second.path.empty() ? 15000 : 60000;
            if (now - it->second.ts < ttl)
            {
                if (it->second.path.empty())
                {
                    if (whyOut)
                        *whyOut = "cached no-route";
                    return false;
                }
                out = it->second.path;
                if (whyOut)
                    *whyOut = "ok";
                return true;
            }
        }
    }

    RouteFail why = RouteFail::None;
    bool const ok = RouteOnGraph(*g, sx, sy, sz, dx, dy, dz, out, why);
    if (whyOut)
        *whyOut = RouteFailName(why);
    // budget bails are transient (a busy tick, an unusually long route) -
    // caching one as "no route" poisons every retry from this 128yd cell.
    // NoEntry is excluded for the same reason it is in the async worker above:
    // "no graph node near me" depends on the caller's HEIGHT, and RouteKey is
    // z-blind, so a bot in a cave (or under a bridge, or on a lower city tier)
    // would poison everyone in that cell for 15s - including someone standing
    // on the road directly overhead. This path is the high-volume writer (every
    // bot's long same-map travel when Pathways.AllBots is on), so leaving it out
    // of the fix left the guide reading poisoned entries it no longer writes.
    bool const cacheable = ok || (why != RouteFail::Budget && why != RouteFail::NoEntry);
    if (cacheable)
    {
        std::lock_guard<std::mutex> lock(s_routeCacheMutex);
        if (s_routeCache.size() > 2048) // simple bound - clear wholesale
            s_routeCache.clear();
        s_routeCache[key] = { ok ? out : std::vector<std::array<float, 3>>{}, now };
    }
    return ok;
}

uint32 WlPathwaysRouteAsync(uint32 mapId, float sx, float sy, float sz,
                            float dx, float dy, float dz)
{
    if (!s_enabled.load(std::memory_order_relaxed))
        return 0;
    std::shared_ptr<PathwayGraph const> g = GetGraph(mapId);
    if (!g)
        return 0;

    // a fresh cached answer (positive or structural negative) resolves the
    // job instantly - the poll returns it on the first call
    std::shared_ptr<RouteJob> job = std::make_shared<RouteJob>();
    job->mapId = mapId;
    job->sx = sx; job->sy = sy; job->sz = sz;
    job->dx = dx; job->dy = dy; job->dz = dz;
    job->graph = std::move(g);
    {
        uint64 const key = RouteKey(mapId, sx, sy, dx, dy);
        std::lock_guard<std::mutex> lock(s_routeCacheMutex);
        auto it = s_routeCache.find(key);
        if (it != s_routeCache.end())
        {
            uint32 const ttl = it->second.path.empty() ? 15000 : 60000;
            if (getMSTime() - it->second.ts < ttl)
            {
                job->state = it->second.path.empty() ? -1 : 1;
                job->path = it->second.path;
                job->why = it->second.path.empty() ? "cached no-route" : "ok";
                job->doneMs = getMSTime();
            }
        }
    }

    std::lock_guard<std::mutex> lock(s_jobMutex);
    // purge dead ids (cancelled/finished) so they can't hold the queue at
    // cap and starve new requests while a long job runs (review catch)
    std::deque<uint32> live;
    for (uint32 qid : s_jobQueue)
    {
        auto it = s_jobs.find(qid);
        if (it != s_jobs.end() && it->second->state == 0)
            live.push_back(qid);
    }
    s_jobQueue.swap(live);
    if (job->state == 0 && s_jobQueue.size() >= JOB_MAX_QUEUED)
        return 0; // queue full - caller falls back to live stepping
    uint32 id = s_nextJobId++;
    if (!s_nextJobId)
        s_nextJobId = 1;
    while (s_jobs.count(id)) // wrap-around collision with a live job
    {
        id = s_nextJobId++;
        if (!s_nextJobId)
            s_nextJobId = 1;
    }
    bool const queued = job->state == 0;
    s_jobs[id] = std::move(job);
    if (queued)
        s_jobQueue.push_back(id);
    return id;
}

int WlPathwaysRoutePoll(uint32 jobId, std::vector<std::array<float, 3>>& out,
                        char const** whyOut)
{
    std::lock_guard<std::mutex> lock(s_jobMutex);
    auto it = s_jobs.find(jobId);
    if (it == s_jobs.end())
    {
        if (whyOut)
            *whyOut = "job expired";
        return -1;
    }
    if (it->second->state == 0)
        return 0;
    int const state = it->second->state;
    if (whyOut)
        *whyOut = it->second->why;
    if (state == 1)
        out = std::move(it->second->path);
    s_jobs.erase(it);
    return state;
}

void WlPathwaysRouteCancel(uint32 jobId)
{
    std::lock_guard<std::mutex> lock(s_jobMutex);
    s_jobs.erase(jobId); // the queue skips missing/finished ids
}

/* ---------------------------------------------------------------------- */
/*  Scripts                                                                */
/* ---------------------------------------------------------------------- */
class WowLegendsPathwaysWorld : public WorldScript
{
public:
    WowLegendsPathwaysWorld() : WorldScript("WowLegendsPathwaysWorld") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadPathwaysConfig();
    }

    void OnStartup() override
    {
        if (s_enabled.load())
            LoadAllGraphs();
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        AdvanceBuildJob();
        AdvanceRouteJobs();
    }
};

class WowLegendsPathwaysCommand : public CommandScript
{
public:
    WowLegendsPathwaysCommand() : CommandScript("WowLegendsPathwaysCommand") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable sub =
        {
            { "build",  HandleBuild,  SEC_ADMINISTRATOR, Console::Yes },
            { "status", HandleStatus, SEC_ADMINISTRATOR, Console::Yes },
        };
        static ChatCommandTable base =
        {
            { "wlpaths", sub },
        };
        return base;
    }

    static bool HandleBuild(ChatHandler* handler, uint32 mapId)
    {
        if (s_job)
        {
            handler->PSendSysMessage("지도 {}의 경로를 이미 생성 중입니다. .wlpaths status로 확인하세요.", s_job->mapId);
            return true;
        }

        MapEntry const* entry = sMapStore.LookupEntry(mapId);
        if (!entry || entry->Instanceable())
        {
            handler->PSendSysMessage("지도 {}는 경로를 생성할 수 있는 월드 지도가 아닙니다.", mapId);
            return true;
        }

        Map* map = sMapMgr->CreateBaseMap(mapId);
        if (!map)
        {
            handler->PSendSysMessage("지도 {}를 열 수 없습니다.", mapId);
            return true;
        }

        s_job = std::make_unique<BuildJob>();
        s_job->mapId = mapId;
        s_job->maxIdx = uint32(MAP_SIZE / SPACING);
        s_job->tanComfort = std::tan(s_comfortDeg.load() * float(M_PI) / 180.0f);
        s_job->climbWeight = s_climbWeight.load();
        s_job->startMs = getMSTime();
        s_job->g = std::make_shared<PathwayGraph>();
        s_job->g->mapId = mapId;

        handler->PSendSysMessage("지도 {} 경로 생성 중 (권장 경사 {:.0f}도, 등반 배율 {:.0f}). 약 1~2분 뒤 .wlpaths status 또는 로그로 확인하세요.",
                                 mapId, s_comfortDeg.load(), s_climbWeight.load());
        LOG_INFO("server", "[pathways] build started for map {} (comfort {:.1f} deg, climb weight {:.1f})",
                 mapId, s_comfortDeg.load(), s_climbWeight.load());
        return true;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        handler->PSendSysMessage("경로 탐색 {} (전체 봇: {}), 권장 경사 {:.0f}도, 등반 배율 {:.0f}, 수중 배율 {:.1f}",
                                 s_enabled.load() ? "ON" : "OFF", s_allBots.load() ? "yes" : "no",
                                 s_comfortDeg.load(), s_climbWeight.load(), s_waterCost.load());
        if (s_job)
        {
            char const* phase = s_job->phase == 0 ? "sampling" : (s_job->phase == 1 ? "linking" : "finalizing");
            handler->PSendSysMessage("  생성 중: 지도 {}, {} - 행 {}/{}, 현재 지점 {}개",
                                     s_job->mapId, phase, s_job->iy, s_job->maxIdx,
                                     uint32(s_job->g->nodes.size()));
        }
        std::lock_guard<std::mutex> lock(s_graphMutex);
        if (s_graphs.empty())
        {
            handler->SendSysMessage("불러온 경로 그래프가 없습니다.");
            return true;
        }
        for (auto const& [mapId, g] : s_graphs)
            handler->PSendSysMessage("  지도 {}: 지점 {}개, 연결 {}개",
                                     mapId, uint32(g->nodes.size()), uint32(g->adjData.size() / 2));
        return true;
    }
};

void AddWowLegendsPathwaysScripts()
{
    new WowLegendsPathwaysWorld();
    new WowLegendsPathwaysCommand();
}
