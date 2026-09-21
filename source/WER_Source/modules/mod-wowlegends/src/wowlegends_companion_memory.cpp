/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - Companion long-term memory (shared adventures)
 *
 * Gives a player's personal Companion (see wowlegends_companion.cpp) a memory of
 * the milestones you live through TOGETHER - leveling up, felling a boss, falling
 * in battle - and lets it retell them in AI chat. A memory is only recorded when
 * the companion is actually grouped with its owner as the event fires (a truly
 * SHARED moment). Summaries are plain author-built English (never LLM output),
 * persisted in characters.wowlegends_companion_memory, and injected into the
 * companion's AI-chat persona only when its OWNER is the one chatting with it.
 *
 * 100% mod-wowlegends - zero mod-playerbots edits. Toggles:
 *   WowLegends.Companion.Memory.Enabled   (default 1)
 *   WowLegends.Companion.Memory.MaxEvents (default 8, prompt-token budget)
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Creature.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "WorldSession.h"
#include "DatabaseEnv.h"
#include "Configuration/Config.h"
#include "Log.h"
#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
    std::atomic<bool> g_companionMemEnabled{true};
    std::atomic<uint32> g_companionMemMax{8};

    enum CompanionMemoryKind : uint8
    {
        COMPANION_MEM_LEVEL = 1,
        COMPANION_MEM_BOSS  = 2,
        COMPANION_MEM_DEATH = 3,
    };

    bool WlIsRealPlayer(Player* p)
    {
        return p && p->GetSession() && !p->GetSession()->IsBot();
    }

    // SQL-escape a string for embedding in a quoted literal.
    std::string Escape(std::string const& s)
    {
        std::string out;
        out.reserve(s.size() + 4);
        for (char c : s)
        {
            if (c == '\'' || c == '\\')
                out.push_back('\\');
            out.push_back(c);
        }
        return out;
    }

    // bot_guid -> owner_guid cache (0 = confirmed NOT a companion). Accessed on
    // the world tick (chat hooks + AI EnqueueAi); guarded for safety. Kept fresh
    // by WlCompanionMemoryInvalidate() on companion create / forget.
    std::mutex g_ownerCacheMutex;
    std::unordered_map<uint32, uint32> g_ownerCache;

    uint32 CompanionOwnerOf(uint32 botGuid)
    {
        {
            std::lock_guard<std::mutex> lock(g_ownerCacheMutex);
            auto it = g_ownerCache.find(botGuid);
            if (it != g_ownerCache.end())
                return it->second;
        }
        uint32 owner = 0;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT owner_guid FROM wowlegends_companion WHERE bot_guid = {}", botGuid))
            owner = r->Fetch()[0].Get<uint32>();
        {
            std::lock_guard<std::mutex> lock(g_ownerCacheMutex);
            g_ownerCache[botGuid] = owner;
        }
        return owner;
    }

    uint32 CompanionBotOf(uint32 ownerGuid)
    {
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT bot_guid FROM wowlegends_companion WHERE owner_guid = {}", ownerGuid))
            return r->Fetch()[0].Get<uint32>();
        return 0;
    }

    // Is the bot (by guid) currently in the owner's group?
    bool BotGroupedWith(Player* owner, uint32 botGuid)
    {
        if (!owner)
            return false;
        Group* g = owner->GetGroup();
        return g && g->IsMember(ObjectGuid(HighGuid::Player, botGuid));
    }

    void RecordCompanionMemory(uint32 botGuid, uint8 kind, std::string const& summary)
    {
        std::string esc = Escape(summary);
        if (esc.size() > 255)
            esc.resize(255);

        CharacterDatabase.Execute(
            "INSERT INTO wowlegends_companion_memory (bot_guid, kind, summary) "
            "VALUES ({}, {}, '{}')",
            botGuid, (uint32)kind, esc);

        // Keep only the newest N memories per companion (id is auto-increment).
        uint32 keep = g_companionMemMax.load();
        if (keep < 1)
            keep = 1;
        CharacterDatabase.Execute(
            "DELETE FROM wowlegends_companion_memory WHERE bot_guid = {} AND id NOT IN "
            "(SELECT id FROM (SELECT id FROM wowlegends_companion_memory "
            "WHERE bot_guid = {} ORDER BY id DESC LIMIT {}) AS keep_rows)",
            botGuid, botGuid, keep);
    }
}

// Exposed to wowlegends_companion.cpp (create / forget) via a plain
// `extern void WlCompanionMemoryInvalidate(uint32);` so the bot->owner cache
// never serves a stale answer after a companion is claimed or released.
void WlCompanionMemoryInvalidate(uint32 botGuid)
{
    std::lock_guard<std::mutex> lock(g_ownerCacheMutex);
    g_ownerCache.erase(botGuid);
}

// Exposed to wowlegends_aichat.cpp (EnqueueAi). Returns persona text to append
// when `speakerGuid` is the owner of companion `botGuid`; "" otherwise (also ""
// when disabled, not a companion, or no memories yet). The cheap is-companion
// check is cached so it costs nothing for the 99% of bot chats that aren't a
// companion talking to its owner.
std::string WlCompanionMemoryNarrative(uint32 botGuid, uint32 speakerGuid)
{
    if (!g_companionMemEnabled.load())
        return "";

    uint32 owner = CompanionOwnerOf(botGuid);
    if (!owner || owner != speakerGuid)
        return "";

    uint32 keep = g_companionMemMax.load();
    if (keep < 1)
        keep = 1;
    QueryResult r = CharacterDatabase.Query(
        "SELECT summary FROM wowlegends_companion_memory WHERE bot_guid = {} "
        "ORDER BY id DESC LIMIT {}",
        botGuid, keep);
    if (!r)
        return "";

    std::string out =
        "\nAdventures you have shared with this very player (most recent first; "
        "if they ask, recall them warmly and proudly, in character):";
    do
    {
        out += "\n- ";
        out += r->Fetch()[0].Get<std::string>();
    } while (r->NextRow());
    return out;
}

/* -------------------------------------------------------------------------- */
/*  WorldScript: memory table DDL + orphan sweep + cached toggles              */
/* -------------------------------------------------------------------------- */
class WowLegendsCompanionMemoryWorld : public WorldScript
{
public:
    WowLegendsCompanionMemoryWorld() : WorldScript("WowLegendsCompanionMemoryWorld",
        { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnStartup() override
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS wowlegends_companion_memory ("
            "id INT UNSIGNED NOT NULL AUTO_INCREMENT, "
            "bot_guid INT UNSIGNED NOT NULL, "
            "event_time TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
            "kind TINYINT UNSIGNED NOT NULL, "
            "summary VARCHAR(255) NOT NULL, "
            "PRIMARY KEY (id), "
            "KEY idx_bot (bot_guid)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

        // Drop memories whose companion binding is gone (forgotten / re-rolled).
        CharacterDatabase.DirectExecute(
            "DELETE FROM wowlegends_companion_memory WHERE bot_guid NOT IN "
            "(SELECT bot_guid FROM wowlegends_companion)");
        LOG_INFO("server", "[companion] wowlegends_companion_memory table ready");
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_companionMemEnabled = sConfigMgr->GetOption<bool>(
            "WowLegends.Companion.Memory.Enabled", true);
        g_companionMemMax = sConfigMgr->GetOption<uint32>(
            "WowLegends.Companion.Memory.MaxEvents", 8);
    }
};

/* -------------------------------------------------------------------------- */
/*  PlayerScript: record shared milestones (level-up / boss kill / death)      */
/* -------------------------------------------------------------------------- */
class WowLegendsCompanionMemoryPlayer : public PlayerScript
{
public:
    WowLegendsCompanionMemoryPlayer() : PlayerScript("WowLegendsCompanionMemoryPlayer",
        { PLAYERHOOK_ON_LEVEL_CHANGED, PLAYERHOOK_ON_CREATURE_KILL,
          PLAYERHOOK_ON_PLAYER_JUST_DIED }) { }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!g_companionMemEnabled.load() || !WlIsRealPlayer(player))
            return;
        uint32 botGuid = CompanionBotOf(player->GetGUID().GetCounter());
        if (!botGuid || !BotGroupedWith(player, botGuid))
            return;
        RecordCompanionMemory(botGuid, COMPANION_MEM_LEVEL,
            "you reached level " + std::to_string(player->GetLevel()) +
            " with me at your side");
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!g_companionMemEnabled.load() || !WlIsRealPlayer(player))
            return;
        uint32 botGuid = CompanionBotOf(player->GetGUID().GetCounter());
        if (!botGuid || !BotGroupedWith(player, botGuid))
            return;
        RecordCompanionMemory(botGuid, COMPANION_MEM_DEATH,
            "you fell in battle while I fought beside you");
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        // Hot path: cheap gates BEFORE any binding / group / DB work.
        if (!g_companionMemEnabled.load() || !killer || !killed)
            return;
        if (!killed->isWorldBoss() && !killed->IsDungeonBoss())
            return;

        // The credited killer may be the real owner OR their companion bot, so
        // accept the kill from either as long as both are grouped.
        uint32 botGuid = 0;
        Player* owner = nullptr;
        if (WlIsRealPlayer(killer))
        {
            owner = killer;
            botGuid = CompanionBotOf(killer->GetGUID().GetCounter());
        }
        else
        {
            botGuid = killer->GetGUID().GetCounter();
            if (uint32 ownerGuid = CompanionOwnerOf(botGuid))
                owner = ObjectAccessor::FindPlayer(ObjectGuid(HighGuid::Player, ownerGuid));
        }

        if (!botGuid || !owner || !BotGroupedWith(owner, botGuid))
            return;

        RecordCompanionMemory(botGuid, COMPANION_MEM_BOSS,
            "we slew " + killed->GetName() + " together");
    }
};

void AddWowLegendsCompanionMemoryScripts()
{
    new WowLegendsCompanionMemoryWorld();
    new WowLegendsCompanionMemoryPlayer();
}
