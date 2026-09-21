/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - World Events: Faction Battlefront
 *
 * A contested capture point ("battlefront") erupts in a curated open-world zone
 * - either by the random scheduler or a GM command - and both factions fight
 * over it. A war banner marks the spot, a few neutral guards defend it, and an
 * on-screen capture bar (the Hellfire/Outland tower slider) shows the tug of
 * war. Control is driven by who stands near the banner; the AI playerbots
 * (players too) join in, so it becomes a live PvP flashpoint, but real players
 * are weighted far heavier than bots (default 3:1) so they decide it. The
 * winning side's real participants get gold + honor (+ an optional buff); the
 * losing side gets a small honor consolation.
 *
 * Everything runs on the world thread (WorldScript::OnUpdate, post map-update
 * barrier - the same safe place as the World PvP sweep) and from the GM command
 * (also world thread), so the live event state needs no locking. One
 * battlefront at a time, a hard timeout, and a bounded set of summoned objects
 * (one banner + a handful of guards) keep it GUID-cap safe. Default OFF.
 *
 * GM: .wlevent start [zone|index] | stop | status | zones
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Player.h"
#include "Creature.h"
#include "GameObject.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Transport.h"
#include "WorldSession.h"
#include "Configuration/Config.h"
#include "StringFormat.h"
#include "Log.h"
#include "Random.h"
#include "SharedDefines.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    // ---- cached config (read once in OnAfterConfigLoad; world thread) ----
    std::atomic<bool> g_enabled{false};
    uint32 g_intervalMinSec = 45 * 60;
    uint32 g_intervalMaxSec = 90 * 60;
    float  g_radius = 50.0f;
    uint32 g_realWeight = 3;
    uint32 g_botWeight = 1;
    uint32 g_holdToWinSec = 8;
    uint32 g_timeoutSec = 20 * 60;
    int32  g_captureStep = 4;          // control change per tick, uncontested
    uint32 g_bannerEntry = 182259;     // Battle Standard (beacon GO)
    uint32 g_guardEntry = 23562;       // Unstoppable Abomination (faction 14)
    uint32 g_guardCount = 2;
    float  g_guardRing = 10.0f;        // guard spawn ring radius (yards)
    uint32 g_spawnBotsPerFaction = 10; // bots teleported per faction at start
    uint32 g_rewardGold = 50000;       // copper (5g)
    uint32 g_rewardHonor = 250;
    uint32 g_consolationHonor = 50;
    uint32 g_rewardBuffSpell = 22888;  // Rallying Cry; 0 = no buff

    constexpr float TWO_PI = 6.2831853f;

    // ---- curated battlefront locations (valid game_tele ground) ----
    struct ZoneSpot
    {
        char const* name;
        uint32 map;
        float x, y, z, o;
    };

    ZoneSpot const g_zones[] =
    {
        // Eastern Kingdoms (map 0)
        { "Westfall",             0, -10235.2f,  1222.5f,  43.6f, 6.24f },
        { "Redridge Mountains",   0,  -9266.6f, -2188.8f,  64.1f, 2.10f },
        { "Duskwood",             0, -10573.0f, -1182.5f,  28.0f, 0.31f },
        { "Arathi Highlands",     0,  -1508.5f, -2732.1f,  32.5f, 3.36f },
        { "Hillsbrad Foothills",  0,   -853.2f,  -533.5f,  10.0f, 0.24f },
        { "The Hinterlands",      0,    119.4f, -3190.4f, 117.3f, 2.34f },
        { "Stranglethorn Vale",   0, -12388.9f,   172.6f,   2.8f, 1.92f },
        { "Eastern Plaguelands",  0,   2301.0f, -4613.4f,  73.6f, 0.37f },
        // Kalimdor (map 1)
        { "The Barrens",          1,   -452.8f, -2650.8f,  95.5f, 0.24f },
        { "Ashenvale",            1,   1928.3f, -2165.9f,  93.8f, 0.21f },
        { "Stonetalon Mountains", 1,   1570.9f,  1031.5f, 138.0f, 3.33f },
        { "Desolace",             1,   -606.4f,  2211.8f,  93.0f, 0.81f },
        { "Thousand Needles",     1,  -4969.0f, -1726.9f, -62.1f, 3.79f },
        { "Feralas",              1,  -4841.2f,  1309.4f,  81.4f, 1.49f },
        { "Tanaris",              1,  -7177.1f, -3785.3f,   8.4f, 6.10f },
        { "Dustwallow Marsh",     1,  -4043.6f, -2991.3f,  36.4f, 3.37f },
    };
    constexpr uint32 ZONE_COUNT = uint32(sizeof(g_zones) / sizeof(g_zones[0]));

    constexpr uint32 TICK_MS = 3000;        // capture evaluation cadence
    constexpr uint32 PROGRESS_MS = 15000;   // per-participant % chat cadence
    constexpr int32  CONTROL_MAX = 100;

    // ---- live event state (touched ONLY on the world thread) ----
    struct ActiveEvent
    {
        bool active = false;
        uint32 zoneIdx = 0;
        ObjectGuid banner;
        std::vector<ObjectGuid> guards;
        int32 control = 0;                 // -MAX (Horde) .. +MAX (Alliance)
        uint32 elapsedMs = 0;
        uint32 holdMs = 0;                 // time the leader has pinned its cap
        uint32 sinceAnnounceMs = 0;
        std::set<ObjectGuid> contribA;
        std::set<ObjectGuid> contribH;
    };
    ActiveEvent g_evt;

    uint32 g_tickAccum = 0;
    uint32 g_nextSpawnMs = 0;
    bool   g_spawnArmed = false;

    bool IsBotPlayer(Player* p)
    {
        return p && p->GetSession() && p->GetSession()->IsBot();
    }

    void Broadcast(std::string const& msg)
    {
        ChatHandler(nullptr).SendGlobalSysMessage(msg.c_str());
    }

    int32 AlliancePercent()
    {
        return (g_evt.control + CONTROL_MAX) * 100 / (2 * CONTROL_MAX);
    }

    // Snap a ring point to the terrain so guards/bots placed off-centre on a
    // slope don't spawn underground or in the air (TeleportTo/Create do NOT
    // ground-snap). Falls back to the banner's z if no ground is found.
    float GroundZ(Map* map, float x, float y, float fallbackZ)
    {
        float const z =
            map->GetHeight(PHASEMASK_NORMAL, x, y, fallbackZ + 5.0f);
        return z <= -100000.0f ? fallbackZ : z;
    }

    ObjectGuid SpawnBanner(ZoneSpot const& spot)
    {
        Map* map = sMapMgr->CreateBaseMap(spot.map);
        if (!map)
            return ObjectGuid::Empty;

        GameObject* go = sObjectMgr->IsGameObjectStaticTransport(g_bannerEntry)
            ? new StaticTransport() : new GameObject();
        G3D::Quat const rot =
            G3D::Quat::fromAxisAngleRotation(G3D::Vector3::unitZ(), spot.o);
        if (!go->Create(map->GenerateLowGuid<HighGuid::GameObject>(),
            g_bannerEntry, map, PHASEMASK_NORMAL, spot.x, spot.y, spot.z,
            spot.o, rot, 100, GO_STATE_READY))
        {
            delete go;
            LOG_ERROR("server", "[worldevents] banner {} failed to spawn in {}",
                g_bannerEntry, spot.name);
            return ObjectGuid::Empty;
        }

        // setActive before AddToMap so the grid is force-loaded (the beacon may
        // spawn in an otherwise-empty cell of a loaded continent).
        go->setActive(true);
        map->AddToMap(go);
        return go->GetGUID();
    }

    void DespawnBanner()
    {
        if (!g_evt.banner)
            return;
        ZoneSpot const& spot = g_zones[g_evt.zoneIdx];
        if (Map* map = sMapMgr->FindBaseMap(spot.map))
            if (GameObject* go = map->GetGameObject(g_evt.banner))
            {
                go->SetRespawnTime(0);
                go->Delete();
            }
        g_evt.banner.Clear();
    }

    void SpawnGuards(ZoneSpot const& spot)
    {
        if (!g_guardCount || !g_guardEntry)
            return;
        Map* map = sMapMgr->FindBaseMap(spot.map);
        if (!map)
            return;

        for (uint32 i = 0; i < g_guardCount; ++i)
        {
            float const ang = float(i) * TWO_PI / float(g_guardCount);
            float const gx = spot.x + g_guardRing * std::cos(ang);
            float const gy = spot.y + g_guardRing * std::sin(ang);
            float const gz = GroundZ(map, gx, gy, spot.z);

            Creature* c = new Creature();
            if (!c->Create(map->GenerateLowGuid<HighGuid::Unit>(), map,
                PHASEMASK_NORMAL, g_guardEntry, 0, gx, gy, gz, ang))
            {
                delete c;
                continue;
            }
            c->SetHomePosition(gx, gy, gz, ang);
            map->AddToMap(c);
            g_evt.guards.push_back(c->GetGUID());
        }
    }

    void DespawnGuards()
    {
        if (g_evt.guards.empty())
            return;
        // AddObjectToRemoveList (NOT DespawnOrUnsummon): these guards are
        // manually created (spawnId == 0), so DespawnOrUnsummon would only
        // hide them and arm a ~5 min respawn - leaking permanent NPCs. This
        // queues real deletion with no respawn.
        if (Map* map = sMapMgr->FindBaseMap(g_zones[g_evt.zoneIdx].map))
            for (ObjectGuid const& guid : g_evt.guards)
                if (Creature* c = map->GetCreature(guid))
                    c->AddObjectToRemoveList();
        g_evt.guards.clear();
    }

    // Teleport up to SpawnBotsPerFaction random bots from the pool to a ring
    // around the banner so the point has bodies to fight over. They are NOT
    // tracked or cleaned up - they are the realm's own random bots, just moved;
    // they brawl, then drift back to their routines on their own.
    void TeleportBotsToEvent(std::vector<ObjectGuid>& pool,
        ZoneSpot const& spot)
    {
        Map* map = sMapMgr->FindBaseMap(spot.map);
        uint32 want = std::min<uint32>(g_spawnBotsPerFaction,
            uint32(pool.size()));
        while (want-- && !pool.empty())
        {
            uint32 const pick = urand(0, uint32(pool.size()) - 1);
            ObjectGuid const guid = pool[pick];
            pool[pick] = pool.back();
            pool.pop_back();

            Player* bot = ObjectAccessor::FindPlayer(guid);
            if (!bot)
                continue;
            float const ang = frand(0.0f, TWO_PI);
            float const r = 14.0f + frand(0.0f, 6.0f);
            float const bx = spot.x + r * std::cos(ang);
            float const by = spot.y + r * std::sin(ang);
            float const bz = map ? GroundZ(map, bx, by, spot.z) : spot.z;
            bot->TeleportTo(spot.map, bx, by, bz, ang);
        }
    }

    void SpawnEventBots(ZoneSpot const& spot)
    {
        if (!g_spawnBotsPerFaction)
            return;
        std::vector<ObjectGuid> ally;
        std::vector<ObjectGuid> horde;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* p = pair.second;
            if (!IsBotPlayer(p) || !p->IsInWorld() || !p->IsAlive())
                continue;
            if (p->IsGameMaster() || p->GetMapId() != spot.map)
                continue;
            if (p->GetMap()->Instanceable() || p->InBattleground())
                continue;
            if (p->GetTeamId() == TEAM_ALLIANCE)
                ally.push_back(p->GetGUID());
            else if (p->GetTeamId() == TEAM_HORDE)
                horde.push_back(p->GetGUID());
        }
        TeleportBotsToEvent(ally, spot);
        TeleportBotsToEvent(horde, spot);
    }

    // Tear down every summoned object and reset for the next event.
    void Teardown()
    {
        DespawnGuards();
        DespawnBanner();
        g_evt = ActiveEvent();
        g_spawnArmed = false;          // re-arm the random cadence afresh
    }

    // Pay only real players actually AT the point when it is decided - this
    // closes the drive-by / AFK farm and pays by LIVE faction (a mid-event
    // faction change is handled correctly).
    void RewardPresentPlayers(TeamId winner)
    {
        ZoneSpot const& spot = g_zones[g_evt.zoneIdx];
        float const radiusSq = g_radius * g_radius;
        uint32 const goldG = g_rewardGold / 10000;
        uint32 const goldS = (g_rewardGold % 10000) / 100;

        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* p = pair.second;
            if (!p || !p->IsInWorld() || p->GetMapId() != spot.map)
                continue;
            if (p->IsGameMaster() || IsBotPlayer(p))   // real players only
                continue;
            float const dx = p->GetPositionX() - spot.x;
            float const dy = p->GetPositionY() - spot.y;
            if (dx * dx + dy * dy > radiusSq)
                continue;

            TeamId const team = p->GetTeamId();
            if (team == winner)
            {
                if (g_rewardGold)
                    p->ModifyMoney(int32(g_rewardGold));
                if (g_rewardHonor)
                    p->ModifyHonorPoints(int32(g_rewardHonor));
                if (g_rewardBuffSpell)
                    p->CastSpell(p, g_rewardBuffSpell, true);
                ChatHandler(p->GetSession()).PSendSysMessage(
                    "전선 승리! 보상: +{}골드 {}실버, +명예 {}{}.",
                    goldG, goldS, g_rewardHonor,
                    g_rewardBuffSpell ? ", a war buff" : "");
            }
            else if (team == TEAM_ALLIANCE || team == TEAM_HORDE)
            {
                if (g_consolationHonor)
                    p->ModifyHonorPoints(int32(g_consolationHonor));
                ChatHandler(p->GetSession()).PSendSysMessage(
                    "전선 패배 - 참가 보상: +명예 {}.",
                    g_consolationHonor);
            }
        }
    }

    void EndEvent(TeamId winner)   // ALLIANCE / HORDE win; NEUTRAL = no victor
    {
        if (!g_evt.active)
            return;
        ZoneSpot const& spot = g_zones[g_evt.zoneIdx];

        if (winner == TEAM_ALLIANCE)
            Broadcast(Acore::StringFormat("|cff1e90ff[World Event]|r The "
                "Alliance has seized the battlefront in {}!", spot.name));
        else if (winner == TEAM_HORDE)
            Broadcast(Acore::StringFormat("|cffff2020[World Event]|r The "
                "Horde has seized the battlefront in {}!", spot.name));
        else
            Broadcast(Acore::StringFormat("|cffffd700[World Event]|r The "
                "battlefront in {} faded with no victor.", spot.name));

        if (winner == TEAM_ALLIANCE || winner == TEAM_HORDE)
            RewardPresentPlayers(winner);

        LOG_INFO("server", "[worldevents] battlefront in {} ended (winner {})",
            spot.name, uint32(winner));
        Teardown();
    }

    void StartEvent(uint32 zoneIdx)
    {
        if (g_evt.active || zoneIdx >= ZONE_COUNT)
            return;
        ZoneSpot const& spot = g_zones[zoneIdx];

        g_evt = ActiveEvent();
        g_evt.active = true;
        g_evt.zoneIdx = zoneIdx;
        g_evt.banner = SpawnBanner(spot);   // flavour; ok if empty
        SpawnGuards(spot);
        SpawnEventBots(spot);

        Broadcast(Acore::StringFormat("|cffffd700[World Event]|r A battlefront "
            "has erupted in {}! Stand on the banner and hold it for your "
            "faction!", spot.name));
        LOG_INFO("server", "[worldevents] battlefront started in {} (map {})",
            spot.name, spot.map);
    }

    void TickEvent(uint32 diff)
    {
        if (!g_evt.active)
            return;

        g_evt.elapsedMs += diff;
        g_evt.sinceAnnounceMs += diff;

        // TimeoutMinutes = 0 means no timeout: run until a faction wins.
        if (g_timeoutSec > 0 && g_evt.elapsedMs >= g_timeoutSec * 1000)
        {
            EndEvent(TEAM_NEUTRAL);
            return;
        }

        ZoneSpot const& spot = g_zones[g_evt.zoneIdx];

        // Tally weighted presence within the radius; credit real players and
        // collect them so we can drive their capture bar after control updates.
        // realA/realH gate the WIN so a bot-only swarm can push the bar but
        // never trigger a victory with no one to reward.
        uint32 wA = 0;
        uint32 wH = 0;
        bool realA = false;
        bool realH = false;
        std::vector<Player*> realInRadius;
        float const radiusSq = g_radius * g_radius;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* p = pair.second;
            if (!p || !p->IsInWorld() || p->GetMapId() != spot.map)
                continue;
            if (!p->IsAlive())
                continue;
            float const dx = p->GetPositionX() - spot.x;
            float const dy = p->GetPositionY() - spot.y;
            if (dx * dx + dy * dy > radiusSq)
                continue;

            bool const bot = IsBotPlayer(p);
            if (!bot)
                realInRadius.push_back(p);   // real players (incl GM) get UI
            if (p->IsGameMaster())
                continue;                    // GMs don't influence the tally

            uint32 const w = bot ? g_botWeight : g_realWeight;
            if (p->GetTeamId() == TEAM_ALLIANCE)
            {
                wA += w;
                if (!bot)
                {
                    realA = true;
                    g_evt.contribA.insert(p->GetGUID());
                }
            }
            else if (p->GetTeamId() == TEAM_HORDE)
            {
                wH += w;
                if (!bot)
                {
                    realH = true;
                    g_evt.contribH.insert(p->GetGUID());
                }
            }
        }

        // Move control toward whoever holds the point.
        if (wA > 0 && wH == 0)
            g_evt.control =
                std::min<int32>(CONTROL_MAX, g_evt.control + g_captureStep);
        else if (wH > 0 && wA == 0)
            g_evt.control =
                std::max<int32>(-CONTROL_MAX, g_evt.control - g_captureStep);
        else if (wA > 0 && wH > 0)
        {
            // Contested: the stronger side creeps in slowly; equal = frozen.
            int32 const creep = std::max<int32>(1, g_captureStep / 2);
            if (wA > wH)
                g_evt.control =
                    std::min<int32>(CONTROL_MAX, g_evt.control + creep);
            else if (wH > wA)
                g_evt.control =
                    std::max<int32>(-CONTROL_MAX, g_evt.control - creep);
        }
        else
        {
            // Empty: decay toward neutral.
            if (g_evt.control > 0)
                g_evt.control = std::max<int32>(0, g_evt.control - 1);
            else if (g_evt.control < 0)
                g_evt.control = std::min<int32>(0, g_evt.control + 1);
        }

        // Hold-to-win: sit at the cap with a REAL player of that side present.
        if (g_evt.control >= CONTROL_MAX && realA)
        {
            g_evt.holdMs += diff;
            if (g_evt.holdMs >= g_holdToWinSec * 1000)
            {
                EndEvent(TEAM_ALLIANCE);
                return;
            }
        }
        else if (g_evt.control <= -CONTROL_MAX && realH)
        {
            g_evt.holdMs += diff;
            if (g_evt.holdMs >= g_holdToWinSec * 1000)
            {
                EndEvent(TEAM_HORDE);
                return;
            }
        }
        else
        {
            g_evt.holdMs = 0;
        }

        // Progress readout for players at the point. A true on-screen capture
        // bar is NOT possible server-side in these zones (the 3.3.5 client only
        // renders the slider in its built-in Outland PvP zones), so this is a
        // throttled chat % that works everywhere. A real bar would need a
        // client-side addon reading this same data.
        if (g_evt.sinceAnnounceMs >= PROGRESS_MS)
        {
            g_evt.sinceAnnounceMs = 0;
            int32 const a = AlliancePercent();
            for (Player* p : realInRadius)
                ChatHandler(p->GetSession()).PSendSysMessage(
                    "|cffffd700전선|r {}: 얼라이언스 {}% / 호드 {}%",
                    spot.name, a, 100 - a);
        }
    }

    void ArmNextSpawn()
    {
        uint32 lo = g_intervalMinSec;
        uint32 hi = g_intervalMaxSec;
        if (hi < lo)
            std::swap(lo, hi);
        g_nextSpawnMs = urand(lo, hi) * 1000;
        g_spawnArmed = true;
    }
}

class WowLegendsWorldEventsWorld : public WorldScript
{
public:
    WowLegendsWorldEventsWorld() : WorldScript("WowLegendsWorldEventsWorld",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        bool const was = g_enabled.load();
        g_enabled = sConfigMgr->GetOption<bool>(
            "WowLegends.WorldEvents.Enabled", false);

        // Clamp the minutes to [1, 1440] BEFORE *60 so the *1000 in
        // ArmNextSpawn can never overflow uint32.
        uint32 minMin = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.IntervalMinMinutes", 45);
        uint32 maxMin = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.IntervalMaxMinutes", 90);
        uint32 toMin = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.Battlefront.TimeoutMinutes", 20);
        g_intervalMinSec = std::clamp<uint32>(minMin, 1, 1440) * 60;
        g_intervalMaxSec = std::clamp<uint32>(maxMin, 1, 1440) * 60;
        g_timeoutSec = std::clamp<uint32>(toMin, 0, 1440) * 60;   // 0 = no cap
        g_radius = sConfigMgr->GetOption<float>(
            "WowLegends.WorldEvents.Battlefront.Radius", 50.0f);
        g_realWeight = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.Battlefront.RealPlayerWeight", 3);
        g_botWeight = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.Battlefront.BotWeight", 1);
        g_holdToWinSec = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.Battlefront.HoldToWinSeconds", 8);
        g_captureStep = sConfigMgr->GetOption<int32>(
            "WowLegends.WorldEvents.Battlefront.CaptureStep", 4);
        g_bannerEntry = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.Battlefront.BannerEntry", 182259);
        g_guardEntry = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.Battlefront.GuardEntry", 23562);
        g_guardCount = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.Battlefront.GuardCount", 2);
        g_guardRing = sConfigMgr->GetOption<float>(
            "WowLegends.WorldEvents.Battlefront.GuardRingRadius", 10.0f);
        g_spawnBotsPerFaction = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.Battlefront.SpawnBotsPerFaction", 10);
        g_rewardGold = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.Reward.Gold", 50000);
        g_rewardHonor = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.Reward.Honor", 250);
        g_consolationHonor = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.Reward.ConsolationHonor", 50);
        g_rewardBuffSpell = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldEvents.Reward.BuffSpell", 22888);

        g_captureStep = std::clamp<int32>(g_captureStep, 1, CONTROL_MAX);
        g_guardCount = std::min<uint32>(g_guardCount, 20);   // sane cap
        g_spawnBotsPerFaction = std::min<uint32>(g_spawnBotsPerFaction, 40);
        // Cap gold so int32(copper) can never wrap negative (~100k gold).
        if (g_rewardGold > 1000000000)
            g_rewardGold = 1000000000;

        // Disabled mid-event: tear the active battlefront down cleanly.
        if (was && !g_enabled && g_evt.active)
            Teardown();
        else
            g_spawnArmed = false;
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_enabled.load())
            return;

        g_tickAccum += diff;
        if (g_tickAccum >= TICK_MS)
        {
            TickEvent(g_tickAccum);
            g_tickAccum = 0;
        }

        // Random auto-spawn while idle.
        if (!g_evt.active)
        {
            if (!g_spawnArmed)
                ArmNextSpawn();
            else if (g_nextSpawnMs <= diff)
                StartEvent(urand(0, ZONE_COUNT - 1));
            else
                g_nextSpawnMs -= diff;
        }
    }
};

class WowLegendsWorldEventCommand : public CommandScript
{
public:
    WowLegendsWorldEventCommand() : CommandScript("WowLegendsWorldEventCommand")
    {
    }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable sub =
        {
            { "start",  HandleStart,  SEC_GAMEMASTER, Console::Yes },
            { "stop",   HandleStop,   SEC_GAMEMASTER, Console::Yes },
            { "status", HandleStatus, SEC_GAMEMASTER, Console::Yes },
            { "zones",  HandleZones,  SEC_GAMEMASTER, Console::Yes },
        };
        static ChatCommandTable base =
        {
            { "wlevent", sub },
        };
        return base;
    }

    static bool HandleStart(ChatHandler* handler, Optional<std::string> zoneArg)
    {
        if (!g_enabled.load())
        {
            handler->SendSysMessage(
                "월드 이벤트가 꺼져 있습니다 (WowLegends.WorldEvents.Enabled = 0).");
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (g_evt.active)
        {
            handler->PSendSysMessage("이미 {}에서 전선이 진행 중입니다. 먼저 .wlevent stop을 사용하세요.", g_zones[g_evt.zoneIdx].name);
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 idx = ZONE_COUNT;   // sentinel = none chosen yet
        if (zoneArg && !zoneArg->empty())
        {
            std::string const q = *zoneArg;
            bool const numeric = std::all_of(q.begin(), q.end(),
                [](char c) { return std::isdigit((unsigned char)c) != 0; });
            if (numeric)
            {
                uint32 const n = uint32(std::atoi(q.c_str()));
                if (n >= 1 && n <= ZONE_COUNT)
                    idx = n - 1;
            }
            else
            {
                std::string ql = q;
                std::transform(ql.begin(), ql.end(), ql.begin(),
                    [](char c)
                    { return char(std::tolower((unsigned char)c)); });
                for (uint32 i = 0; i < ZONE_COUNT; ++i)
                {
                    std::string zn = g_zones[i].name;
                    std::transform(zn.begin(), zn.end(), zn.begin(),
                        [](char c)
                        { return char(std::tolower((unsigned char)c)); });
                    if (zn.find(ql) != std::string::npos)
                    {
                        idx = i;
                        break;
                    }
                }
            }
            if (idx >= ZONE_COUNT)
            {
                handler->PSendSysMessage("'{}' 지역을 찾을 수 없습니다. .wlevent zones로 목록을 확인하세요.", *zoneArg);
                handler->SetSentErrorMessage(true);
                return false;
            }
        }
        else
        {
            idx = urand(0, ZONE_COUNT - 1);
        }

        StartEvent(idx);
        handler->PSendSysMessage(
            "{}에서 전선을 시작했습니다.", g_zones[idx].name);
        return true;
    }

    static bool HandleStop(ChatHandler* handler)
    {
        if (!g_evt.active)
        {
            handler->SendSysMessage("진행 중인 전선이 없습니다.");
            return true;
        }
        std::string const name = g_zones[g_evt.zoneIdx].name;
        EndEvent(TEAM_NEUTRAL);
        handler->PSendSysMessage("{}의 전선을 중지했습니다.", name);
        return true;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        if (!g_evt.active)
        {
            handler->SendSysMessage("진행 중인 전선이 없습니다.");
            return true;
        }
        ZoneSpot const& spot = g_zones[g_evt.zoneIdx];
        int32 const a = AlliancePercent();
        handler->PSendSysMessage("{} 전선: 얼라이언스 {}% / 호드 {}%, 경과 {}초, 기여자 얼라이언스:{} 호드:{}.", spot.name, a, 100 - a,
            g_evt.elapsedMs / 1000, uint32(g_evt.contribA.size()),
            uint32(g_evt.contribH.size()));
        handler->PSendSysMessage(".go xyz {} {} {} {}  (깃발 위치)",
            spot.x, spot.y, spot.z, spot.map);
        return true;
    }

    static bool HandleZones(ChatHandler* handler)
    {
        handler->SendSysMessage("월드 이벤트 전선 지역:");
        for (uint32 i = 0; i < ZONE_COUNT; ++i)
            handler->PSendSysMessage("  {}. {}", i + 1, g_zones[i].name);
        return true;
    }
};

void AddWowLegendsWorldEventScripts()
{
    new WowLegendsWorldEventsWorld();
    new WowLegendsWorldEventCommand();
}
