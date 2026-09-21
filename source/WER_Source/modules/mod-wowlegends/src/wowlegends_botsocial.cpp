/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - Bot social memory (acquaintance, recognition, grudges)
 *
 * Gives EVERY playerbot a persistent social memory of the real players it has
 * met: where and when you first crossed paths, how often you meet, whether you
 * grouped or dueled, who killed whom - and a warmth score that colors how the
 * bot treats you. When a bot spots a player it knows after time apart, it
 * RECOGNIZES them with an emote ladder: a wave (or a glare, if it holds a
 * grudge), and if the player waves back / targets the bot - or the tie is
 * already strong (grouped, dueled, many meetings) - the bot whispers a
 * greeting that references your shared history ("Last I saw you was in
 * Westfall..."). Greetings are canned lines with placeholders, so the feature
 * works on every install; the AI-chat layer upgrades them when available.
 *
 * Thread model: the sighting sweep, ladder firing, and DB flush run on the
 * WORLD thread (WorldScript::OnUpdate, post map-update barrier). Event hooks
 * (PvP kill, duel end, text emote) can fire on map workers, so ALL shared
 * state lives behind one mutex with tiny critical sections; Player pointers
 * are never stored across calls. DB writes use CharacterDatabase.Execute
 * (async); the only synchronous reads are per-login row loads.
 *
 * Exposed to other WL code:
 *   WlBotSocialNarrative(botLow, playerLow) - persona text for AI chat.
 *   WlBotSocialGrudge(botLow, playerLow)    - PvP grudge gate (playerbots).
 *
 * Config (all WowLegends.BotSocial.*): Enabled (default 1), SightRange,
 * MinAwayMinutes, GreetChance, MaxGreetsPerPlayerPerDay, StrongTieMeetings,
 * PvpGrudge.Enabled.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Group.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "WorldSession.h"
#include "World.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "CellImpl.h"
#include "GridNotifiersImpl.h"
#include "Chat.h"
#include "Configuration/Config.h"
#include "StringFormat.h"
#include "SharedDefines.h"
#include "Random.h"
#include "Log.h"
#include <atomic>
#include <algorithm>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// wowlegends_aichat.cpp: AI recognition greeting - the bot whispers first,
// grounded in the full persona + social narrative, and the greeting joins
// the pair's chat history. Returns false (use the canned line instead) when
// Living Chatter / AI greetings are off or the daily budget is spent.
bool WlAiRecognitionGreet(uint32 botLow, uint32 playerLow);

namespace
{
    // ---- cached config (OnAfterConfigLoad; read from several threads) ----
    std::atomic<bool>   g_enabled{true};
    std::atomic<bool>   g_pvpGrudge{true};
    std::atomic<uint32> g_minAwayMinutes{720};
    std::atomic<uint32> g_greetChance{60};
    std::atomic<uint32> g_maxGreetsPerDay{3};
    std::atomic<uint32> g_strongTieMeetings{5};
    float g_sightRange = 25.0f;            // world thread only

    constexpr uint32 SWEEP_MS = 5000;      // sighting sweep cadence
    constexpr uint32 FLUSH_MS = 60000;     // dirty-row DB flush cadence
    constexpr uint32 LADDER_MS = 60000;    // wave-back window after the emote
    constexpr uint32 SEEN_WRITE_GAP = 300; // s between last_seen persists

    // Acq.flags bits - how the pair knows each other.
    constexpr uint8 ACQ_GROUPED         = 0x01;
    constexpr uint8 ACQ_DUELED          = 0x02;
    constexpr uint8 ACQ_BOT_SLEW_PLAYER = 0x04;
    constexpr uint8 ACQ_PLAYER_SLEW_BOT = 0x08;

    struct Acq
    {
        uint32 firstMet = 0;               // unix seconds
        uint32 lastSeen = 0;
        uint32 lastSeenWritten = 0;        // last_seen value already persisted
        uint32 timesMet = 1;
        uint32 lastGreet = 0;
        uint32 returnGap = 0;              // s away before the last return
        int32  warmth = 0;                 // -100 (grudge) .. +100 (friend)
        uint8  flags = 0;
        std::string lastZone;
        bool   dirty = false;
    };

    // All social state behind ONE mutex: RAM rows, per-player greet budgets,
    // the pending emote-ladder entries, and the fired-greeting queue.
    std::mutex g_mutex;
    std::unordered_map<uint64, Acq> g_acq;             // botLow<<32|playerLow
    std::unordered_set<uint32> g_loaded;               // players with rows in
    std::unordered_map<uint32, std::pair<uint32, uint32>> g_greetsToday;

    // Everything a queued greeting needs is SNAPSHOTTED at recognition time -
    // the sweep updates the live Acq (zone, last_seen) immediately after, so
    // reading it later would name the CURRENT zone instead of where you last
    // parted (the marquee "Last I saw you was in {zone}" line).
    struct GreetSnapshot
    {
        uint32 botLow;
        uint32 playerLow;
        uint32 gap;                        // s apart before this return
        int32  warmth;
        std::string zone;                  // where you last parted
    };
    struct PendingLadder
    {
        GreetSnapshot snap;
        uint32 expireMs;
    };
    std::vector<PendingLadder> g_ladder;
    std::vector<GreetSnapshot> g_fired;

    // Recognition emotes are queued too: sending a grid-broadcast packet while
    // holding g_mutex would run every observer's script hooks under our lock.
    struct EmoteReq
    {
        uint32 botLow;
        uint32 playerLow;
        bool   grudge;
    };
    std::vector<EmoteReq> g_emotes;

    uint64 PairKey(uint32 botLow, uint32 playerLow)
    {
        return (uint64(botLow) << 32) | playerLow;
    }

    bool WlIsRealPlayer(Player* p)
    {
        return p && p->GetSession() && !p->GetSession()->IsBot();
    }

    bool IsBotPlayer(Player* p)
    {
        return p && p->GetSession() && p->GetSession()->IsBot();
    }

    uint32 NowSec()
    {
        return uint32(std::time(nullptr));
    }

    std::string ZoneNameOf(Player* p)
    {
        if (AreaTableEntry const* zone =
            sAreaTableStore.LookupEntry(p->GetZoneId()))
            return zone->area_name[0];
        return "the wilds";
    }

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

    // "less than a day" / "3 days" / "2 weeks" - fuzzy on purpose.
    std::string HumanizeAgo(uint32 seconds, bool korean = false)
    {
        uint32 const days = seconds / 86400;
        if (korean)
        {
            if (days < 1) return "하루도 안 되는 시간";
            if (days < 14) return Acore::StringFormat("{}일", days);
            return Acore::StringFormat("{}주", days / 7);
        }
        if (days < 1)
            return "less than a day";
        if (days == 1)
            return "a day";
        if (days < 14)
            return Acore::StringFormat("{} days", days);
        return Acore::StringFormat("{} weeks", days / 7);
    }

    bool StrongTie(Acq const& a)
    {
        if (a.flags & (ACQ_GROUPED | ACQ_DUELED | ACQ_PLAYER_SLEW_BOT))
            return true;
        if (a.timesMet >= g_strongTieMeetings.load())
            return true;
        return a.warmth >= 40 || a.warmth <= -40;
    }

    void BumpWarmth(Acq& a, int32 delta)
    {
        a.warmth = std::clamp<int32>(a.warmth + delta, -100, 100);
        a.dirty = true;
    }

    /* ---------------------------------------------------------------------- */
    /*  Canned greeting pools ({name} {zone} {ago} substituted at fire time)   */
    /* ---------------------------------------------------------------------- */
    char const* const GREET_FRIEND[] =
    {
        "Well met again, {name}! Last I saw you was in {zone}, {ago} ago.",
        "{name}! Still in one piece, I see. Been {ago} since {zone}.",
        "Good to see you, {name}. How have the roads treated you since {zone}?",
        "Ha! I was just thinking about {zone}. Good timing, {name}.",
        "The road brings you back, {name}! {zone} was {ago} ago already.",
    };
    char const* const GREET_NEUTRAL[] =
    {
        "You look familiar... {zone}, {ago} ago, was it? Small world, {name}.",
        "We have crossed paths before, {name}. {zone}, if memory serves.",
        "I remember you from {zone}, {name}. It has been {ago}.",
        "Well, if it is not {name}. {zone} feels a lifetime ago.",
    };
    char const* const GREET_GRUDGE[] =
    {
        "I remember you, {name}. {zone}. I have not forgotten.",
        "Well, well. {name}. Last time was {zone}, {ago} ago. Watch yourself.",
        "You again, {name}. {zone} still stings.",
    };

    char const* const GREET_FRIEND_KO[] =
    {
        "{name}님, 또 뵙네요! {ago} 전에 {zone}에서 봤죠?",
        "{name}님! {zone}에서 본 지 벌써 {ago} 됐네요.",
        "반가워요, {name}님. {zone}에서 만난 뒤로 퀘스트는 잘 풀렸어요?",
        "마침 {zone} 얘기 하고 있었는데! {name}님 오셨네요.",
        "{name}님 다시 만났네요! {zone}에서 본 게 벌써 {ago} 전이네요.",
    };
    char const* const GREET_NEUTRAL_KO[] =
    {
        "낯이 익네요. {ago} 전에 {zone}에서 만났죠, {name}님?",
        "{name}님, 전에 만난 적 있죠? 아마 {zone}였을 거예요.",
        "{name}님, {zone}에서 뵌 거 기억나요. {ago} 만이네요.",
        "어, {name}님이네요. {zone}에서 만났던 게 생각나요.",
    };
    char const* const GREET_GRUDGE_KO[] =
    {
        "{name}님 기억해요. {zone}에서 있었던 일도요.",
        "{name}님이네요. {ago} 전에 {zone}에서 만났죠. 이번엔 잘해 봅시다.",
        "{name}님, 또 만났네요. {zone}에서 진 건 아직 좀 아쉽네요.",
    };

    std::string LocalizedRememberedZone(std::string const& canonical, LocaleConstant locale)
    {
        // Old acquaintance rows stored English names, not an area ID. Resolve
        // them at display time as well, without erasing the persistent memory.
        if (canonical.empty())
            return locale == LOCALE_koKR ? "지난번 지역" : "the road";
        for (uint32 id = 0; id < sAreaTableStore.GetNumRows(); ++id)
            if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(id))
                if (canonical == area->area_name[LOCALE_enUS])
                {
                    char const* translated = area->area_name[locale];
                    if (translated && *translated) return translated;
                    break;
                }
        if (locale == LOCALE_koKR && std::any_of(canonical.begin(), canonical.end(),
            [](unsigned char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }))
            return "지난번 지역";
        return canonical;
    }

    std::string BuildGreeting(GreetSnapshot const& s,
        std::string const& playerName, LocaleConstant locale)
    {
        bool const korean = locale == LOCALE_koKR;
        char const* line;
        if (s.warmth <= -10)
            line = (korean ? GREET_GRUDGE_KO : GREET_GRUDGE)[urand(0, 2)];
        else if (s.warmth >= 10)
            line = (korean ? GREET_FRIEND_KO : GREET_FRIEND)[urand(0, 4)];
        else
            line = (korean ? GREET_NEUTRAL_KO : GREET_NEUTRAL)[urand(0, 3)];

        std::string out = line;
        auto sub = [&out](std::string const& tag, std::string const& val)
        {
            if (size_t pos = out.find(tag); pos != std::string::npos)
                out.replace(pos, tag.size(), val);
        };
        sub("{name}", playerName);
        sub("{zone}", LocalizedRememberedZone(s.zone, locale));
        sub("{ago}", HumanizeAgo(s.gap ? s.gap : 86400, korean));
        return out;
    }

    // Whisper straight to the player's session (both are online here).
    void DeliverWhisper(Player* bot, Player* player, std::string const& text)
    {
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER,
            Language(LANG_UNIVERSAL), bot->GetGUID(), player->GetGUID(), text,
            /*chatTag=*/0, bot->GetName(), player->GetName());
        player->GetSession()->SendPacket(&data);
    }

    // One VALUES(...) tuple for the REPLACE. Caller holds g_mutex.
    std::string RowValues(uint64 key, Acq const& a)
    {
        return Acore::StringFormat(
            "({}, {}, {}, {}, '{}', {}, {}, {}, {})",
            uint32(key >> 32), uint32(key & 0xFFFFFFFF), a.firstMet,
            a.lastSeenWritten ? a.lastSeenWritten : a.lastSeen,
            Escape(a.lastZone.substr(0, 64)), a.timesMet, a.warmth,
            uint32(a.flags), a.lastGreet);
    }

    // Async-persist a batch of tuples. Call WITHOUT g_mutex held.
    void ExecuteReplaceRows(std::vector<std::string> const& rows)
    {
        if (rows.empty())
            return;
        std::string values;
        for (size_t i = 0; i < rows.size(); ++i)
        {
            values += rows[i];
            if (i + 1 < rows.size())
                values += ", ";
        }
        CharacterDatabase.Execute(
            "REPLACE INTO wowlegends_bot_acquaintance (bot_guid, player_guid, "
            "first_met, last_seen, last_zone, times_met, warmth, flags, "
            "last_greet) VALUES " + values);
    }

    // Load ONE pair row into RAM if present (sync PK SELECT; used when an
    // event fires for a player whose rows were never bulk-loaded, so a fresh
    // RAM entry can never REPLACE-wipe their real history).
    void LoadPairRow(uint32 botLow, uint32 playerLow)
    {
        QueryResult r = CharacterDatabase.Query(
            "SELECT first_met, last_seen, last_zone, times_met, warmth, "
            "flags, last_greet FROM wowlegends_bot_acquaintance "
            "WHERE bot_guid = {} AND player_guid = {}", botLow, playerLow);
        if (!r)
            return;

        Field* f = r->Fetch();
        std::lock_guard<std::mutex> lock(g_mutex);
        auto [it, isNew] = g_acq.try_emplace(PairKey(botLow, playerLow));
        if (!isNew)
            return;                          // a live entry raced us in - keep it
        Acq& a = it->second;
        a.firstMet = f[0].Get<uint32>();
        a.lastSeen = f[1].Get<uint32>();
        a.lastSeenWritten = a.lastSeen;
        a.lastZone = f[2].Get<std::string>();
        a.timesMet = f[3].Get<uint32>();
        a.warmth = f[4].Get<int8>();
        a.flags = f[5].Get<uint8>();
        a.lastGreet = f[6].Get<uint32>();
    }

    // Bulk-load every persisted pair for one player (sync; login / backfill,
    // both rare). Marks the player loaded so the sweep may include them.
    void LoadPlayerRows(uint32 playerLow)
    {
        QueryResult r = CharacterDatabase.Query(
            "SELECT bot_guid, first_met, last_seen, last_zone, times_met, "
            "warmth, flags, last_greet FROM wowlegends_bot_acquaintance "
            "WHERE player_guid = {}", playerLow);

        std::lock_guard<std::mutex> lock(g_mutex);
        if (r)
        {
            do
            {
                Field* f = r->Fetch();
                uint64 const key = PairKey(f[0].Get<uint32>(), playerLow);
                if (g_acq.count(key))
                    continue;                 // session-fresh row wins
                Acq a;
                a.firstMet = f[1].Get<uint32>();
                a.lastSeen = f[2].Get<uint32>();
                a.lastSeenWritten = a.lastSeen;
                a.lastZone = f[3].Get<std::string>();
                a.timesMet = f[4].Get<uint32>();
                a.warmth = f[5].Get<int8>();
                a.flags = f[6].Get<uint8>();
                a.lastGreet = f[7].Get<uint32>();
                g_acq.emplace(key, std::move(a));
            } while (r->NextRow());
        }
        g_loaded.insert(playerLow);
    }

    // Set by OnAfterConfigLoad (console/RA thread possible); serviced on the
    // world thread in OnUpdate.
    std::atomic<bool> g_backfill{false};

    // Greet budget: true if this player may still receive a greeting today.
    // Caller holds g_mutex.
    bool TakeGreetBudget(uint32 playerLow, uint32 now)
    {
        uint32 const day = now / 86400;
        auto& [stamp, count] = g_greetsToday[playerLow];
        if (stamp != day)
        {
            stamp = day;
            count = 0;
        }
        if (count >= g_maxGreetsPerDay.load())
            return false;
        ++count;
        return true;
    }

    // The recognition moment. Caller holds g_mutex (world thread); the Acq
    // still holds the PARTING zone at this point. Queues the emote and either
    // the whisper (strong tie) or the wave-back ladder - nothing is sent while
    // the lock is held.
    void Recognize(Player* bot, Player* player, Acq& a, uint32 now,
        uint32 worldMs)
    {
        if (!roll_chance_i(int32(g_greetChance.load())))
            return;
        if (!TakeGreetBudget(player->GetGUID().GetCounter(), now))
            return;

        a.lastGreet = now;
        a.dirty = true;

        GreetSnapshot snap;
        snap.botLow = bot->GetGUID().GetCounter();
        snap.playerLow = player->GetGUID().GetCounter();
        snap.gap = a.returnGap;
        snap.warmth = a.warmth;
        snap.zone = a.lastZone;

        g_emotes.push_back({ snap.botLow, snap.playerLow, a.warmth <= -10 });

        if (StrongTie(a))
            g_fired.push_back(std::move(snap));         // friends skip the wait
        else
            g_ladder.push_back({ std::move(snap), worldMs + LADDER_MS });
    }
}

/* -------------------------------------------------------------------------- */
/*  Exposed to wowlegends_aichat.cpp: persona narrative for a known player     */
/* -------------------------------------------------------------------------- */
std::string WlBotSocialNarrative(uint32 botLow, uint32 playerLow)
{
    if (!g_enabled.load())
        return "";

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_acq.find(PairKey(botLow, playerLow));
    if (it == g_acq.end())
        return "";
    Acq const& a = it->second;

    std::string out = Acore::StringFormat(
        "\nYou KNOW this player: you have met {} time(s); you last saw them "
        "in {}, {} ago.", a.timesMet,
        a.lastZone.empty() ? "the road" : a.lastZone,
        HumanizeAgo(NowSec() - std::min(a.lastSeen, NowSec())));

    if (a.flags & ACQ_GROUPED)
        out += " You have adventured in a group together.";
    if (a.flags & ACQ_DUELED)
        out += " You once dueled each other.";
    if (a.flags & ACQ_PLAYER_SLEW_BOT)
        out += " They KILLED you in battle once - you remember it well.";
    if (a.flags & ACQ_BOT_SLEW_PLAYER)
        out += " You bested them in combat once.";

    if (a.warmth >= 40)
        out += " You consider them a true friend.";
    else if (a.warmth >= 10)
        out += " You are fond of them.";
    else if (a.warmth <= -40)
        out += " You bear them a deep grudge.";
    else if (a.warmth <= -10)
        out += " You distrust them.";
    return out;
}

/* -------------------------------------------------------------------------- */
/*  Exposed to mod-playerbots (EnemyPlayerValue): grudge-hunt gate.            */
/*  Called from map worker threads - keep it a single cheap lookup.            */
/* -------------------------------------------------------------------------- */
bool WlBotSocialGrudge(uint32 botLow, uint32 playerLow)
{
    if (!g_enabled.load() || !g_pvpGrudge.load())
        return false;

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_acq.find(PairKey(botLow, playerLow));
    if (it == g_acq.end())
        return false;
    return (it->second.flags & ACQ_PLAYER_SLEW_BOT) != 0
        && it->second.warmth <= -20;
}

/* -------------------------------------------------------------------------- */
/*  WorldScript: DDL + config + the sighting/ladder/flush pump                 */
/* -------------------------------------------------------------------------- */
class WowLegendsBotSocialWorld : public WorldScript
{
public:
    WowLegendsBotSocialWorld() : WorldScript("WowLegendsBotSocialWorld",
        { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_AFTER_CONFIG_LOAD,
          WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN }) { }

    void OnShutdown() override
    {
        Flush();   // don't lose the last <60s of social state on a restart
    }

    void OnStartup() override
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS wowlegends_bot_acquaintance ("
            "bot_guid INT UNSIGNED NOT NULL, "
            "player_guid INT UNSIGNED NOT NULL, "
            "first_met INT UNSIGNED NOT NULL, "
            "last_seen INT UNSIGNED NOT NULL, "
            "last_zone VARCHAR(64) NOT NULL DEFAULT '', "
            "times_met INT UNSIGNED NOT NULL DEFAULT 1, "
            "warmth TINYINT NOT NULL DEFAULT 0, "
            "flags TINYINT UNSIGNED NOT NULL DEFAULT 0, "
            "last_greet INT UNSIGNED NOT NULL DEFAULT 0, "
            "PRIMARY KEY (bot_guid, player_guid), "
            "KEY idx_player (player_guid)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");

        // Bots get re-rolled (`.playerbots rndbot init`); drop dead pairs.
        CharacterDatabase.DirectExecute(
            "DELETE FROM wowlegends_bot_acquaintance WHERE bot_guid NOT IN "
            "(SELECT guid FROM characters) OR player_guid NOT IN "
            "(SELECT guid FROM characters)");
        LOG_INFO("server", "[botsocial] wowlegends_bot_acquaintance ready");
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>(
            "WowLegends.BotSocial.Enabled", true);
        g_pvpGrudge = sConfigMgr->GetOption<bool>(
            "WowLegends.BotSocial.PvpGrudge.Enabled", true);
        g_sightRange = sConfigMgr->GetOption<float>(
            "WowLegends.BotSocial.SightRange", 25.0f);
        g_minAwayMinutes = std::max<uint32>(1, sConfigMgr->GetOption<uint32>(
            "WowLegends.BotSocial.MinAwayMinutes", 720));
        g_greetChance = std::min<uint32>(sConfigMgr->GetOption<uint32>(
            "WowLegends.BotSocial.GreetChance", 60), 100);
        g_maxGreetsPerDay = sConfigMgr->GetOption<uint32>(
            "WowLegends.BotSocial.MaxGreetsPerPlayerPerDay", 3);
        g_strongTieMeetings = std::max<uint32>(1, sConfigMgr->GetOption<uint32>(
            "WowLegends.BotSocial.StrongTieMeetings", 5));

        // Enabled at runtime with players already online: their rows were
        // never login-loaded, so have the world thread backfill them.
        if (g_enabled.load())
            g_backfill = true;
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_enabled.load())
            return;

        m_worldMs += diff;
        m_sweepAccum += diff;
        m_flushAccum += diff;

        if (g_backfill.exchange(false))
            BackfillOnlinePlayers();

        DrainQueues();

        if (m_sweepAccum >= SWEEP_MS)
        {
            m_sweepAccum = 0;
            Sweep();
        }
        if (m_flushAccum >= FLUSH_MS)
        {
            m_flushAccum = 0;
            Flush();
        }
    }

private:
    uint32 m_worldMs = 0;
    uint32 m_sweepAccum = 0;
    uint32 m_flushAccum = 0;

    // Send the recognition emotes and greeting whispers queued under the
    // lock. World thread, lock RELEASED while packets go out.
    void DrainQueues()
    {
        std::vector<EmoteReq> emotes;
        std::vector<GreetSnapshot> fired;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_emotes.empty() && g_fired.empty())
                return;
            emotes.swap(g_emotes);
            fired.swap(g_fired);
        }

        for (EmoteReq const& e : emotes)
        {
            Player* bot = ObjectAccessor::FindPlayer(
                ObjectGuid(HighGuid::Player, e.botLow));
            Player* player = ObjectAccessor::FindPlayer(
                ObjectGuid(HighGuid::Player, e.playerLow));
            if (!bot || !player)
                continue;
            if (e.grudge)
            {
                bot->HandleEmoteCommand(EMOTE_ONESHOT_RUDE);
                bot->TextEmote(Acore::StringFormat(
                    sWorld->GetDefaultDbcLocale() == LOCALE_koKR
                        ? "{}님을 바라봅니다. 지난 만남을 기억하는 것 같습니다."
                        : "glares at {}. Some things are not forgotten.",
                    player->GetName()), player);
            }
            else
            {
                bot->HandleEmoteCommand(EMOTE_ONESHOT_WAVE);
                bot->TextEmote(Acore::StringFormat(
                    sWorld->GetDefaultDbcLocale() == LOCALE_koKR
                        ? "전에 만난 {}님에게 손을 흔듭니다."
                        : "waves at {}, a familiar face on the road.",
                    player->GetName()), player);
            }
        }

        for (GreetSnapshot const& s : fired)
        {
            Player* bot = ObjectAccessor::FindPlayer(
                ObjectGuid(HighGuid::Player, s.botLow));
            Player* player = ObjectAccessor::FindPlayer(
                ObjectGuid(HighGuid::Player, s.playerLow));
            if (!bot || !player || !player->GetSession())
                continue;
            // AI first (whispers with the shared history woven in); canned
            // placeholder line when the AI path is off or out of budget.
            if (WlAiRecognitionGreet(s.botLow, s.playerLow))
                continue;
            DeliverWhisper(bot, player, BuildGreeting(s, player->GetName(), player->GetSession()->GetSessionDbLocaleIndex()));
        }
    }

    void Sweep()
    {
        uint32 const now = NowSec();
        uint32 const awaySec = g_minAwayMinutes.load() * 60;

        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* p = pair.second;
            if (!WlIsRealPlayer(p) || !p->IsInWorld() || p->IsGameMaster())
                continue;
            if (p->GetMap()->Instanceable() || p->InBattleground())
                continue;
            if (p->HasStealthAura() || !p->IsAlive())
                continue;

            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (!g_loaded.count(p->GetGUID().GetCounter()))
                    continue;                     // rows still loading (login)
            }

            std::list<Player*> nearby;
            Acore::AnyPlayerInObjectRangeCheck check(p, g_sightRange);
            Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck>
                searcher(p, nearby, check);
            Cell::VisitObjects(p, searcher, g_sightRange);

            std::string const zone = ZoneNameOf(p);
            uint32 const playerLow = p->GetGUID().GetCounter();
            Group* grp = p->GetGroup();

            for (Player* b : nearby)
            {
                if (!IsBotPlayer(b) || !b->IsAlive() || b == p)
                    continue;

                uint32 const botLow = b->GetGUID().GetCounter();
                bool const grouped = grp && grp->IsMember(b->GetGUID());

                std::lock_guard<std::mutex> lock(g_mutex);
                auto [it, isNew] =
                    g_acq.try_emplace(PairKey(botLow, playerLow));
                Acq& a = it->second;

                if (isNew)
                {
                    a.firstMet = now;
                    a.lastSeen = now;
                    a.lastSeenWritten = now;
                    a.lastZone = zone;
                    a.dirty = true;
                }
                else
                {
                    // Clamp against clock rollback so a backwards jump can't
                    // produce "7000 weeks ago" or a bogus instant return.
                    uint32 const seen = std::min(a.lastSeen, now);
                    bool const returning = a.lastSeen && now - seen >= awaySec;
                    if (returning)
                    {
                        a.returnGap = now - seen;
                        ++a.timesMet;
                        BumpWarmth(a, 1);
                        if (now - std::min(a.lastGreet, now) >= awaySec)
                            Recognize(b, p, a, now, m_worldMs);
                    }
                    if (p->GetTarget() == b->GetGUID())
                        FireLadderLocked(PairKey(botLow, playerLow));
                    a.lastSeen = now;
                    a.lastZone = zone;
                    if (now - a.lastSeenWritten >= SEEN_WRITE_GAP)
                    {
                        a.lastSeenWritten = now;
                        a.dirty = true;
                    }
                }

                if (grouped && !(a.flags & ACQ_GROUPED))
                {
                    a.flags |= ACQ_GROUPED;
                    BumpWarmth(a, 10);
                }
            }
        }

        // Expire stale ladder entries + prune yesterday's greet budgets
        // (kept across relogs on purpose; only the day change clears them).
        std::lock_guard<std::mutex> lock(g_mutex);
        std::erase_if(g_ladder, [this](PendingLadder const& l)
        {
            return m_worldMs >= l.expireMs;
        });
        uint32 const today = now / 86400;
        std::erase_if(g_greetsToday, [today](auto const& e)
        {
            return e.second.first != today;
        });
    }

    // Caller holds g_mutex.
    static void FireLadderLocked(uint64 key)
    {
        for (auto it = g_ladder.begin(); it != g_ladder.end(); ++it)
            if (PairKey(it->snap.botLow, it->snap.playerLow) == key)
            {
                g_fired.push_back(std::move(it->snap));
                g_ladder.erase(it);
                return;
            }
    }

    void Flush()
    {
        std::vector<std::string> rows;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto& [key, a] : g_acq)
            {
                if (!a.dirty)
                    continue;
                a.dirty = false;
                rows.push_back(RowValues(key, a));
            }
        }
        ExecuteReplaceRows(rows);
    }

    // Feature enabled while players were already online (a live `.reload
    // config`): bulk-load their rows on the world thread so the sweep's
    // g_loaded gate doesn't skip them forever.
    static void BackfillOnlinePlayers()
    {
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* p = pair.second;
            if (!WlIsRealPlayer(p) || !p->IsInWorld())
                continue;
            uint32 const low = p->GetGUID().GetCounter();
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_loaded.count(low))
                    continue;
            }
            LoadPlayerRows(low);
        }
    }
};

/* -------------------------------------------------------------------------- */
/*  PlayerScript: login row load + duel / PvP-kill / wave-back hooks           */
/* -------------------------------------------------------------------------- */
class WowLegendsBotSocialPlayer : public PlayerScript
{
public:
    WowLegendsBotSocialPlayer() : PlayerScript("WowLegendsBotSocialPlayer",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_PVP_KILL,
          PLAYERHOOK_ON_DUEL_END, PLAYERHOOK_ON_TEXT_EMOTE }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_enabled.load() || !WlIsRealPlayer(player))
            return;
        LoadPlayerRows(player->GetGUID().GetCounter());
    }

    // Evict the player's social state on logout (persisting dirty rows
    // first) so RAM stays bounded by the ONLINE population; rows reload from
    // the DB at next login. Runs even when the feature was toggled off
    // mid-session, so stale state can't linger.
    void OnPlayerLogout(Player* player) override
    {
        if (!WlIsRealPlayer(player))
            return;

        uint32 const playerLow = player->GetGUID().GetCounter();
        std::vector<std::string> rows;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto it = g_acq.begin(); it != g_acq.end();)
            {
                if (uint32(it->first & 0xFFFFFFFF) == playerLow)
                {
                    if (it->second.dirty)
                        rows.push_back(RowValues(it->first, it->second));
                    it = g_acq.erase(it);
                }
                else
                    ++it;
            }
            g_loaded.erase(playerLow);
            // g_greetsToday is deliberately KEPT: erasing it would let a
            // relog reset the daily greeting budget (it is day-stamped and
            // self-resetting; stale days are pruned in the sweep).
            std::erase_if(g_ladder, [playerLow](PendingLadder const& l)
            {
                return l.snap.playerLow == playerLow;
            });
            std::erase_if(g_fired, [playerLow](GreetSnapshot const& s)
            {
                return s.playerLow == playerLow;
            });
            std::erase_if(g_emotes, [playerLow](EmoteReq const& e)
            {
                return e.playerLow == playerLow;
            });
        }
        ExecuteReplaceRows(rows);
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (!g_enabled.load() || !killer || !killed)
            return;

        // Exactly one side must be a bot for this to be a bot<->player event.
        if (IsBotPlayer(killer) && WlIsRealPlayer(killed))
            Record(killer, killed, ACQ_BOT_SLEW_PLAYER, -5, killed);
        else if (WlIsRealPlayer(killer) && IsBotPlayer(killed))
            Record(killed, killer, ACQ_PLAYER_SLEW_BOT, -25, killer);
    }

    void OnPlayerDuelEnd(Player* winner, Player* loser,
        DuelCompleteType /*type*/) override
    {
        if (!g_enabled.load() || !winner || !loser)
            return;

        // A sporting duel warms the pair up regardless of the outcome.
        if (IsBotPlayer(winner) && WlIsRealPlayer(loser))
            Record(winner, loser, ACQ_DUELED, 8, loser);
        else if (WlIsRealPlayer(winner) && IsBotPlayer(loser))
            Record(loser, winner, ACQ_DUELED, 8, winner);
    }

    void OnPlayerTextEmote(Player* player, uint32 textEmote,
        uint32 /*emoteNum*/, ObjectGuid guid) override
    {
        if (!g_enabled.load() || !WlIsRealPlayer(player))
            return;
        switch (textEmote)
        {
            case TEXT_EMOTE_WAVE:
            case TEXT_EMOTE_HELLO:
            case TEXT_EMOTE_GREET:
            case TEXT_EMOTE_BOW:
            case TEXT_EMOTE_SALUTE:
                break;
            default:
                return;
        }

        // A wave back at a bot whose ladder is pending fires the greeting.
        // Untargeted waves count for ANY of this player's pending bots. The
        // target guid must actually be a PLAYER guid - counters collide
        // across the creature/player namespaces, so a wave at a mob whose
        // creature-low equals the bot's player-low must not match.
        uint32 const playerLow = player->GetGUID().GetCounter();
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_ladder.begin(); it != g_ladder.end();)
        {
            if (it->snap.playerLow == playerLow
                && (!guid || (guid.IsPlayer()
                    && guid.GetCounter() == it->snap.botLow)))
            {
                g_fired.push_back(std::move(it->snap));
                it = g_ladder.erase(it);
            }
            else
                ++it;
        }
    }

private:
    // Upsert the pair (bot, player) with an event flag + warmth delta. The
    // zone is read from `where` (valid only for this call).
    static void Record(Player* bot, Player* player, uint8 flag, int32 delta,
        Player* where)
    {
        uint32 const now = NowSec();
        uint32 const botLow = bot->GetGUID().GetCounter();
        uint32 const playerLow = player->GetGUID().GetCounter();

        // If this player's rows were never bulk-loaded, pull THIS pair's row
        // first - otherwise a fresh RAM entry would later REPLACE-wipe the
        // pair's real history in the DB.
        bool needLoad;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            needLoad = !g_loaded.count(playerLow)
                && !g_acq.count(PairKey(botLow, playerLow));
        }
        if (needLoad)
            LoadPairRow(botLow, playerLow);

        std::lock_guard<std::mutex> lock(g_mutex);
        auto [it, isNew] = g_acq.try_emplace(PairKey(botLow, playerLow));
        Acq& a = it->second;
        if (isNew)
        {
            a.firstMet = now;
            a.lastSeen = now;
            a.lastSeenWritten = now;
        }
        if (where)
            a.lastZone = ZoneNameOf(where);
        a.flags |= flag;
        BumpWarmth(a, delta);
    }
};

void AddWowLegendsBotSocialScripts()
{
    new WowLegendsBotSocialWorld();
    new WowLegendsBotSocialPlayer();
}
