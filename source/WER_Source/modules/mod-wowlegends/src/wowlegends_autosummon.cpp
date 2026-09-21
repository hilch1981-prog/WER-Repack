/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - Auto-summon party bots into instances
 *
 * mod-playerbots' dungeon-finder path does not reliably pull a real player's
 * group bots into the instance (the in-game `summon` command does, but you have
 * to type it). When a real player zones into a dungeon or raid, this fires that
 * summon automatically a few seconds later - after the group has settled -
 * teleporting every bot in the player's group that isn't already on the
 * player's map to them (reviving any that died). Solves the "bots stay outside
 * the dungeon" problem with zero player effort.
 *
 * World-thread only: the teleport runs in WorldScript::OnUpdate (post
 * map-update barrier, the same safe place as the World PvP sweep). The delay is
 * configurable and is what fixes the "only 1-2 bots come" race - the whole
 * group is in by then. Default ON; toggle WowLegends.AutoSummonBots.Enabled.
 * Only the group LEADER's entry triggers it (the player running the group).
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "WorldSession.h"
#include "Configuration/Config.h"
#include "GameTime.h"
#include "Log.h"
#include "Random.h"
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    std::atomic<bool> g_enabled{true};
    std::atomic<uint32> g_delayMs{4000};

    constexpr float TWO_PI = 6.2831853f;

    // player guid -> earliest summon time (GameTime ms). Written from
    // OnPlayerMapChanged (map worker), drained in OnUpdate (world thread).
    std::mutex g_mutex;
    std::unordered_map<ObjectGuid, uint64> g_pending;

    bool IsBotPlayer(Player* p)
    {
        return p && p->GetSession() && p->GetSession()->IsBot();
    }

    void SummonGroupBots(Player* player)
    {
        Group* group = player->GetGroup();
        Map* pmap = player->FindMap();
        if (!group || !pmap)
            return;

        uint32 summoned = 0;
        for (GroupReference* itr = group->GetFirstMember(); itr;
            itr = itr->next())
        {
            Player* bot = itr->GetSource();
            if (!bot || bot == player || !IsBotPlayer(bot))
                continue;
            if (bot->FindMap() == pmap)        // already inside with us
                continue;

            if (!bot->IsAlive())
            {
                bot->ResurrectPlayer(1.0f);
                bot->SpawnCorpseBones();   // clear the corpse (like the summon cmd)
            }

            float const ang = frand(0.0f, TWO_PI);
            bot->TeleportTo(player->GetMapId(),
                player->GetPositionX() + 2.0f * std::cos(ang),
                player->GetPositionY() + 2.0f * std::sin(ang),
                player->GetPositionZ(), player->GetOrientation());
            ++summoned;
        }
        if (summoned)
            LOG_DEBUG("server", "[autosummon] pulled {} bot(s) to {} (map {})",
                summoned, player->GetName(), player->GetMapId());
    }
}

class WowLegendsAutoSummonPlayer : public PlayerScript
{
public:
    WowLegendsAutoSummonPlayer() : PlayerScript("WowLegendsAutoSummonPlayer",
        { PLAYERHOOK_ON_MAP_CHANGED })
    {
    }

    void OnPlayerMapChanged(Player* player) override
    {
        if (!g_enabled.load() || !player || IsBotPlayer(player))
            return;
        Group* group = player->GetGroup();
        if (!group || group->GetLeaderGUID() != player->GetGUID())
            return;
        Map* map = player->FindMap();
        if (!map || (!map->IsDungeon() && !map->IsRaid()))
            return;

        uint64 const fire =
            uint64(GameTime::GetGameTimeMS().count()) + g_delayMs.load();
        std::lock_guard<std::mutex> lock(g_mutex);
        g_pending[player->GetGUID()] = fire;
    }
};

class WowLegendsAutoSummonWorld : public WorldScript
{
public:
    WowLegendsAutoSummonWorld() : WorldScript("WowLegendsAutoSummonWorld",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>(
            "WowLegends.AutoSummonBots.Enabled", true);
        g_delayMs = sConfigMgr->GetOption<uint32>(
            "WowLegends.AutoSummonBots.DelayMs", 4000);
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        if (!g_enabled.load())
            return;

        uint64 const now = uint64(GameTime::GetGameTimeMS().count());
        std::vector<ObjectGuid> due;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_pending.empty())
                return;
            for (auto it = g_pending.begin(); it != g_pending.end(); )
            {
                if (now >= it->second)
                {
                    due.push_back(it->first);
                    it = g_pending.erase(it);
                }
                else
                    ++it;
            }
        }

        for (ObjectGuid const& guid : due)
        {
            Player* p = ObjectAccessor::FindPlayer(guid);
            if (!p)
                continue;
            Map* map = p->FindMap();
            if (map && (map->IsDungeon() || map->IsRaid()))
                SummonGroupBots(p);
        }
    }
};

void AddWowLegendsAutoSummonScripts()
{
    new WowLegendsAutoSummonPlayer();
    new WowLegendsAutoSummonWorld();
}
