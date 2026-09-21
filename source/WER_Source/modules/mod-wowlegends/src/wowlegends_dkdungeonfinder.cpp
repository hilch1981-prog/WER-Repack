/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - let Death Knight BOTS use the Dungeon Finder
 *
 * The bug (reported by StretchtGorilla, 2026-07-30): put a Death Knight bot in
 * your party and the whole group can no longer queue for anything.
 *
 * The cause is a hard rule in the core, and it is CORRECT retail behaviour:
 * LFGMgr.cpp locks every Death Knight out of the Dungeon Finder until they have
 * finished the Ebon Hold starting chain -
 *
 *     else if (player->IsClass(CLASS_DEATH_KNIGHT) && !player->IsGameMaster() &&
 *              !(player->IsQuestRewarded(13188) || player->IsQuestRewarded(13189)))
 *         lockData = LFG_LOCKSTATUS_QUEST_NOT_COMPLETED;
 *
 * A real Death Knight earns that quest on the way out of Acherus. A BOT is
 * created straight at level 55+ by the playerbot factory and never runs the
 * chain, so it can never satisfy the check. Measured on our own realm before
 * this fix: 2,000 Death Knight bots, all level 55+, ZERO holding either quest.
 * And because the Dungeon Finder locks a group on the UNION of its members'
 * locks, one such bot blocks every human in the party too - which is what the
 * report was actually about.
 *
 * The fix: on login, if a BOT is a Death Knight and holds neither quest, mark
 * the faction-appropriate one rewarded. Deliberately narrow:
 *
 *   - BOTS ONLY. A real Death Knight still has to walk out of Acherus like
 *     everyone else; handing a human a quest they did not do would be a
 *     content change, not a bug fix.
 *   - SetRewardedQuest() is the whole operation, and it is exactly two lines in
 *     core: insert into m_RewardedQuests, flag it for save. No rewards, no
 *     spells, no items, no achievement credit, no phase change, no XP. Nothing
 *     to break.
 *   - Faction-correct: Alliance gets 13188 "Where Kings Walk", Horde gets
 *     13189 "Warchief's Blessing". The LFG check accepts either, so this is
 *     cosmetic honesty rather than function - a Horde bot holding the Alliance
 *     quest would just read as wrong to anyone inspecting it.
 *   - Idempotent, and it self-heals EXISTING bots. It runs at login, so bots
 *     created before this shipped are fixed the next time they log in - no SQL
 *     migration needed for owners upgrading.
 *
 * Ships ENABLED: this restores intended behaviour (a bot party can use the
 * Dungeon Finder) rather than changing the game, and a server whose bots cannot
 * queue is the broken state, not the safe one. Toggle
 * WowLegends.DkDungeonFinderFix.Enabled to turn it off.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Group.h"
#include "WorldSession.h"
#include "Configuration/Config.h"
#include "DungeonFinding/LFGMgr.h"
#include "Log.h"
#include "SharedDefines.h"
#include <atomic>

namespace
{
    std::atomic<bool> g_enabled{true};

    // The two faction finales of the Death Knight starting chain. LFGMgr accepts
    // EITHER, so which one a bot holds only matters for tidiness.
    constexpr uint32 QUEST_WHERE_KINGS_WALK = 13188;      // Alliance
    constexpr uint32 QUEST_WARCHIEFS_BLESSING = 13189;    // Horde

    bool IsBotPlayer(Player* p)
    {
        return p && p->GetSession() && p->GetSession()->IsBot();
    }
}

class WowLegendsDkDungeonFinderPlayer : public PlayerScript
{
public:
    WowLegendsDkDungeonFinderPlayer()
        : PlayerScript("WowLegendsDkDungeonFinderPlayer", { PLAYERHOOK_ON_LOGIN })
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_enabled.load() || !IsBotPlayer(player))
            return;

        if (!player->IsClass(CLASS_DEATH_KNIGHT))
            return;

        // Already satisfied - either from a previous login of ours, or because
        // the bot genuinely holds one. Nothing to do.
        if (player->IsQuestRewarded(QUEST_WHERE_KINGS_WALK) ||
            player->IsQuestRewarded(QUEST_WARCHIEFS_BLESSING))
            return;

        uint32 const questId = player->GetTeamId(true) == TEAM_ALLIANCE
            ? QUEST_WHERE_KINGS_WALK
            : QUEST_WARCHIEFS_BLESSING;

        player->SetRewardedQuest(questId);

        // ⚠️ MUST refresh the LFG lock cache, and this is not optional.
        // The Dungeon Finder does NOT re-evaluate locks when you queue - it
        // reads a per-player map cached by LFGMgr::InitializeLockedDungeons
        // (LFGMgr.cpp:585 SetLockedDungeons, read at :1597). Core's own
        // LFGPlayerScript::OnPlayerLogin (LFGScripts.cpp:72-93) builds that cache
        // on the SAME hook we are in, and script hook order between us and core
        // is not guaranteed. If core ran first it has already cached "Death
        // Knight, quest missing => locked" and our grant would do nothing until
        // the bot next logs in - the fix would look broken on the very first
        // test. Recomputing here makes the outcome order-independent: harmless
        // if core has not run yet, corrective if it has.
        sLFGMgr->InitializeLockedDungeons(player, player->GetGroup());

        LOG_DEBUG("server", "[dkfix] {} (DK bot) marked quest {} rewarded and LFG "
            "locks recomputed so the party can use the Dungeon Finder",
            player->GetName(), questId);
    }
};

class WowLegendsDkDungeonFinderWorld : public WorldScript
{
public:
    WowLegendsDkDungeonFinderWorld()
        : WorldScript("WowLegendsDkDungeonFinderWorld", { WORLDHOOK_ON_AFTER_CONFIG_LOAD })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>(
            "WowLegends.DkDungeonFinderFix.Enabled", true);
    }
};

void AddWowLegendsDkDungeonFinderScripts()
{
    new WowLegendsDkDungeonFinderPlayer();
    new WowLegendsDkDungeonFinderWorld();
}
