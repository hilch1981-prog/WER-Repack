/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - Living Chatter (AI-flavored open-world bot talk)
 *
 * Makes the open world SOUND alive with real AI, on a strict budget:
 *  - AMBIENT lines: every so often, one bot near a real player says one
 *    AI-generated line out loud, grounded in what it actually is and where
 *    it actually stands (zone, level, class, combat state) plus a rotating
 *    topic seed - a level 12 in Elwynn gripes about wolves, not Naxxramas.
 *  - SCENES: two bots near a real player act a short dialogue. ONE AI call
 *    writes the whole exchange ("A:"/"B:" script); the lines are delivered
 *    alternately with natural pauses, so the player just sees two
 *    adventurers talking.
 *  - (Recognition GREETINGS also ride this budget - see wowlegends_aichat's
 *    WlAiRecognitionGreet, called from wowlegends_botsocial.)
 *
 * Cost design: WITNESSED-ONLY - nothing is ever generated unless a real
 * player is close enough to read it, so an empty realm spends nothing. All
 * requests go through wowlegends_aichat's queue with a Living Chatter kind
 * and are capped by WowLegends.LivingChatter.MaxRequestsPerDay (on the
 * hosted proxy every request costs one credit - the conf documents this).
 *
 * Thread model: everything here runs on the WORLD thread - the scheduler
 * (WorldScript::OnUpdate), WlLivingDeliver (called from aichat's OnUpdate),
 * and the GM test hooks (in-game command handlers). No locks needed; the
 * only cross-thread hop is inside aichat's own queue.
 *
 * Config (WowLegends.LivingChatter.*): Enabled (default 0 - it spends
 * credits), AmbientIntervalSec, SceneIntervalSec, WitnessRange,
 * MaxRequestsPerDay, AiGreetings.
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "WorldSession.h"
#include "DBCStores.h"
#include "GridNotifiers.h"
#include "CellImpl.h"
#include "GridNotifiersImpl.h"
#include "Configuration/Config.h"
#include "StringFormat.h"
#include "SharedDefines.h"
#include "Random.h"
#include "Log.h"
#include <atomic>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

// wowlegends_aichat.cpp: queue one Living Chatter generation (see AiKind).
bool WlAiLivingGenerate(uint32 kind, ObjectGuid botA, ObjectGuid botB,
    std::string const& botAName, std::string const& prompt);

// WOW Legends "Voice Cards" (wowlegends_voicecard.cpp): a bot's own
// deterministic personality + how readily it pipes up in ambient chatter.
extern std::string WlVoiceCardPersona(uint32 guidLow);
extern float WlVoiceCardTalkFactor(uint32 guidLow);
// WOW Legends "Speech Governor" (wowlegends_speechgov.cpp): area cadence gate.
// Ambient/scene are the lowest-priority filler, held first when an area is
// noisy - and asked BEFORE the LLM call, so a held line costs nothing.
extern bool WlSpeechAllow(Player* who, int prio, uint64 beat);

namespace
{
    // must match wowlegends_aichat.cpp's AiKind
    constexpr uint32 KIND_AMBIENT = 2;
    constexpr uint32 KIND_SCENE   = 3;

    constexpr uint32 TICK_MS = 5000;          // scheduler cadence
    constexpr uint32 BOT_AMBIENT_GAP = 600;   // s between ambient lines per bot
    constexpr uint32 BOT_SCENE_GAP = 900;     // s between scenes per bot
    constexpr float  PAIR_RANGE = 15.0f;      // bots this close can talk

    // ---- cached config (world thread only) ----
    std::atomic<bool> g_enabled{false};
    uint32 g_ambientSec = 120;
    uint32 g_sceneSec = 420;
    float  g_witnessRange = 30.0f;

    // ---- world-thread state ----
    // Timers use wrap-safe signed differences (uint32 world-ms wraps after
    // ~49.7 days of uptime; a plain >= comparison would stall for weeks).
    uint32 g_worldMs = 0;
    uint32 g_tickAccum = 0;
    uint32 g_nextAmbientMs = 0;
    uint32 g_nextSceneMs = 0;
    bool   g_timersArmed = false;
    std::unordered_map<uint32, time_t> g_botNext;   // botLow -> next allowed

    bool Due(uint32 nowMs, uint32 deadlineMs)
    {
        return int32(nowMs - deadlineMs) >= 0;
    }

    struct SceneLine
    {
        uint32 sceneId;
        uint32 botLow;
        uint32 otherLow;    // the scene partner (abort if it dies/leaves)
        uint32 fireMs;
        std::string text;
    };
    std::vector<SceneLine> g_sceneQueue;
    uint32 g_sceneIdSeq = 0;

    bool WlIsRealPlayer(Player* p)
    {
        return p && p->GetSession() && !p->GetSession()->IsBot();
    }

    bool IsBotPlayer(Player* p)
    {
        return p && p->GetSession() && p->GetSession()->IsBot();
    }

    char const* RaceName(uint8 race)
    {
        switch (race)
        {
            case 1:  return "human";
            case 2:  return "orc";
            case 3:  return "dwarf";
            case 4:  return "night elf";
            case 5:  return "undead";
            case 6:  return "tauren";
            case 7:  return "gnome";
            case 8:  return "troll";
            case 10: return "blood elf";
            case 11: return "draenei";
            default: return "adventurer";
        }
    }

    char const* ClassName(uint8 klass)
    {
        switch (klass)
        {
            case 1:  return "warrior";
            case 2:  return "paladin";
            case 3:  return "hunter";
            case 4:  return "rogue";
            case 5:  return "priest";
            case 6:  return "death knight";
            case 7:  return "shaman";
            case 8:  return "mage";
            case 9:  return "warlock";
            case 11: return "druid";
            default: return "wanderer";
        }
    }

    std::string ZoneNameOf(Player* p)
    {
        if (AreaTableEntry const* z = sAreaTableStore.LookupEntry(p->GetZoneId()))
            if (z->area_name[0] && *z->area_name[0])
                return z->area_name[0];
        return "the wilds";
    }

    // Rotating topic seeds so the lines don't all orbit the same subject.
    char const* const TOPICS[] =
    {
        "the local wildlife giving you trouble",
        "your repair bill after the last fight",
        "a rumor you heard at the last inn",
        "the road conditions around here",
        "what you plan to cook over the fire tonight",
        "the weather in this zone",
        "a near-death moment you barely escaped",
        "the price of goods at the auction house",
        "something odd you saw on the horizon",
        "your profession and how work is going",
        "the war between the factions",
        "a dungeon you want to brave someday",
        "how heavy your pack has gotten",
        "the quality of the fishing nearby",
        "an old friend you have not seen in a while",
        "what you will do when you finally retire from adventuring",
    };
    constexpr uint32 TOPIC_COUNT = uint32(sizeof(TOPICS) / sizeof(TOPICS[0]));

    // One-line grounded identity, shared by both prompt builders.
    std::string Identity(Player* bot)
    {
        std::string out = bot->GetName();
        out += ", a level " + std::to_string(bot->GetLevel()) + " ";
        out += RaceName(bot->getRace());
        out += " ";
        out += ClassName(bot->getClass());
        out += (bot->GetTeamId() == TEAM_HORDE) ? " of the Horde"
                                                : " of the Alliance";
        return out;
    }

    std::string AmbientPrompt(Player* bot)
    {
        std::string p = "You are " + Identity(bot) + ", out in "
            + ZoneNameOf(bot) + ".";
        p += WlVoiceCardPersona(bot->GetGUID().GetCounter());
        if (bot->IsInCombat())
            p += " You are in the middle of a fight.";
        else if (bot->IsAlive() && bot->GetHealthPct() < 40.0f)
            p += " You are patched up but still hurting.";
        p += " TASK: say ONE short line out loud, in character, about ";
        p += TOPICS[urand(0, TOPIC_COUNT - 1)];
        p += ". Under 25 words. Plain text only: no quotes, no asterisks, "
             "no emotes, and do NOT prefix your own name.";
        return p;
    }

    std::string ScenePrompt(Player* a, Player* b)
    {
        std::string p = "Two adventurers are talking within earshot of "
            "travelers in " + ZoneNameOf(a) + ".\n";
        p += "A is " + Identity(a) + "."
            + WlVoiceCardPersona(a->GetGUID().GetCounter()) + "\n";
        p += "B is " + Identity(b) + "."
            + WlVoiceCardPersona(b->GetGUID().GetCounter()) + "\n";
        p += "TASK: write EXACTLY 4 lines of natural in-character "
             "conversation between them about ";
        p += TOPICS[urand(0, TOPIC_COUNT - 1)];
        p += ", alternating speakers, in THIS format and nothing else:\n"
             "A: <what A says>\nB: <what B says>\nA: <what A says>\n"
             "B: <what B says>\n"
             "Each line under 22 words. Plain text: no emotes, no narration, "
             "no quotes.";
        return p;
    }

    // Every bot within `range` of a real, outdoor, non-GM player. The
    // witnessed-only rule lives here: no players, no candidates, no spend.
    void NearbyBots(Player* around, float range, std::vector<Player*>& out)
    {
        std::list<Player*> nearby;
        Acore::AnyPlayerInObjectRangeCheck check(around, range);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck>
            searcher(around, nearby, check);
        Cell::VisitObjects(around, searcher, range);
        for (Player* b : nearby)
            if (IsBotPlayer(b) && b->IsAlive())
                out.push_back(b);
    }

    bool EligibleWitness(Player* p)
    {
        return WlIsRealPlayer(p) && p->IsInWorld() && p->IsAlive()
            && !p->isAFK() && !p->IsGameMaster()
            && !p->GetMap()->Instanceable() && !p->InBattleground();
    }

    // Strip a leading speaker prefix ("A:", "B:", "Botname:") and any
    // wrapping quotes the model sneaked in; collapse to one clean line.
    std::string CleanLine(std::string s, std::string const& dropPrefix)
    {
        if (size_t nl = s.find('\n'); nl != std::string::npos)
            s.resize(nl);
        auto trim = [](std::string& v)
        {
            while (!v.empty() && (v.front() == ' ' || v.front() == '"'
                || v.front() == '\'' || v.front() == '*'))
                v.erase(v.begin());
            while (!v.empty() && (v.back() == ' ' || v.back() == '"'
                || v.back() == '\'' || v.back() == '*' || v.back() == '\r'))
                v.pop_back();
        };
        trim(s);
        if (!dropPrefix.empty() && s.size() >= dropPrefix.size()
            && s.compare(0, dropPrefix.size(), dropPrefix) == 0)
        {
            s.erase(0, dropPrefix.size());
            trim(s);
        }
        if (s.size() > 200)
        {
            // never split a multibyte UTF-8 sequence; prefer a word boundary
            size_t cut = 200;
            while (cut > 0 && (uint8(s[cut]) & 0xC0) == 0x80)
                --cut;
            if (size_t sp = s.find_last_of(' ', cut); sp != std::string::npos
                && sp > 120)
                cut = sp;
            s.resize(cut);
        }
        return s;
    }

    // Normalize a raw scene-script line before speaker matching: trim junk,
    // then drop the "1. " / "2) " numbering some models add.
    std::string NormalizeSceneLine(std::string s)
    {
        s = CleanLine(std::move(s), "");
        size_t d = 0;
        while (d < s.size() && s[d] >= '0' && s[d] <= '9')
            ++d;
        if (d > 0 && d < s.size() && (s[d] == '.' || s[d] == ')'))
        {
            ++d;
            while (d < s.size() && s[d] == ' ')
                ++d;
            s.erase(0, d);
        }
        return s;
    }

    bool BotReady(uint32 botLow, time_t now)
    {
        auto it = g_botNext.find(botLow);
        return it == g_botNext.end() || it->second <= now;
    }

    // Fire one ambient line around `around` (a real player). `force` is the
    // GM test path: skips per-bot cooldowns, not the budget.
    bool TryAmbient(Player* around, bool force)
    {
        std::vector<Player*> bots;
        NearbyBots(around, g_witnessRange, bots);
        if (bots.empty())
            return false;

        time_t const now = std::time(nullptr);
        std::vector<Player*> ready;
        for (Player* b : bots)
            if (force || BotReady(b->GetGUID().GetCounter(), now))
                ready.push_back(b);
        if (ready.empty())
            return false;

        Player* bot = ready[urand(0, uint32(ready.size()) - 1)];
        uint32 const botLow = bot->GetGUID().GetCounter();
        // lowest-priority filler: if this spot is already chatty, stay quiet
        // (and skip the LLM call entirely). force = GM test, bypasses it.
        if (!force && !WlSpeechAllow(bot, 0 /*ambient*/, 0))
            return false;
        if (!WlAiLivingGenerate(KIND_AMBIENT, bot->GetGUID(),
            ObjectGuid::Empty, bot->GetName(), AmbientPrompt(bot)))
            return false;
        // talkativeness: quiet bots wait longer before piping up again,
        // chatty bots sooner - the personality shows even without the LLM
        g_botNext[botLow] = now + time_t(BOT_AMBIENT_GAP * WlVoiceCardTalkFactor(botLow));
        return true;
    }

    // Fire one two-bot scene around `around`.
    bool TryScene(Player* around, bool force)
    {
        std::vector<Player*> bots;
        NearbyBots(around, g_witnessRange, bots);
        if (bots.size() < 2)
            return false;

        time_t const now = std::time(nullptr);
        Player* a = nullptr;
        Player* b = nullptr;
        for (size_t i = 0; i < bots.size() && !b; ++i)
        {
            if (!force && (!BotReady(bots[i]->GetGUID().GetCounter(), now)
                || bots[i]->IsInCombat()))
                continue;
            for (size_t j = i + 1; j < bots.size(); ++j)
            {
                if (!force && (!BotReady(bots[j]->GetGUID().GetCounter(), now)
                    || bots[j]->IsInCombat()))
                    continue;
                // same faction only - enemies don't share campfire gossip
                if (bots[i]->GetTeamId() != bots[j]->GetTeamId())
                    continue;
                if (bots[i]->IsWithinDist(bots[j], PAIR_RANGE))
                {
                    a = bots[i];
                    b = bots[j];
                    break;
                }
            }
        }
        if (!a || !b)
            return false;

        // a two-bot scene is the heaviest filler (4 lines + 2 LLM turns): hold
        // it if the area is already lively, before spending anything
        if (!force && !WlSpeechAllow(a, 0 /*ambient*/, 0))
            return false;

        if (!WlAiLivingGenerate(KIND_SCENE, a->GetGUID(), b->GetGUID(),
            a->GetName(), ScenePrompt(a, b)))
            return false;
        uint32 const aLow = a->GetGUID().GetCounter();
        uint32 const bLow = b->GetGUID().GetCounter();
        g_botNext[aLow] = now + time_t(BOT_SCENE_GAP * WlVoiceCardTalkFactor(aLow));
        g_botNext[bLow] = now + time_t(BOT_SCENE_GAP * WlVoiceCardTalkFactor(bLow));
        return true;
    }

    // Pick ONE random eligible witness (real player) to center the moment on.
    Player* PickWitness()
    {
        std::vector<Player*> eligible;
        for (auto const& pair : ObjectAccessor::GetPlayers())
            if (EligibleWitness(pair.second))
                eligible.push_back(pair.second);
        if (eligible.empty())
            return nullptr;
        return eligible[urand(0, uint32(eligible.size()) - 1)];
    }

    uint32 Jitter(uint32 baseSec)
    {
        return uint32(float(baseSec * 1000) * frand(0.75f, 1.25f));
    }
}

/* -------------------------------------------------------------------------- */
/*  Called from wowlegends_aichat's OnUpdate (world thread) with a finished    */
/*  generation. Ambient: say it. Scene: parse the script + schedule the show.  */
/* -------------------------------------------------------------------------- */
void WlLivingDeliver(uint32 kind, ObjectGuid botA, ObjectGuid botB,
    std::string const& text)
{
    if (kind == KIND_AMBIENT)
    {
        Player* bot = ObjectAccessor::FindPlayer(botA);
        if (!bot || !bot->IsAlive())
            return;
        // take the first usable line of a possibly multi-line reply
        std::string rest = text;
        while (!rest.empty())
        {
            std::string raw;
            if (size_t nl = rest.find('\n'); nl != std::string::npos)
            {
                raw = rest.substr(0, nl);
                rest.erase(0, nl + 1);
            }
            else
            {
                raw = rest;
                rest.clear();
            }
            std::string line = NormalizeSceneLine(std::move(raw));
            line = CleanLine(std::move(line),
                bot->GetName() + std::string(":"));
            line = CleanLine(std::move(line), "A:");
            if (line.size() >= 2)
            {
                bot->Say(line, LANG_UNIVERSAL);
                return;
            }
        }
        LOG_INFO("server", "[livingchatter] ambient line unusable: '{}'",
            text.substr(0, 80));
        return;
    }

    // scene: split the script into speaker lines and stagger them.
    Player* a = ObjectAccessor::FindPlayer(botA);
    Player* b = ObjectAccessor::FindPlayer(botB);
    if (!a || !b)
        return;

    uint32 const sceneId = ++g_sceneIdSeq;
    uint32 fire = g_worldMs + 1500;
    uint32 scheduled = 0;
    std::string rest = text;
    while (!rest.empty() && scheduled < 6)
    {
        std::string raw;
        if (size_t nl = rest.find('\n'); nl != std::string::npos)
        {
            raw = rest.substr(0, nl);
            rest.erase(0, nl + 1);
        }
        else
        {
            raw = rest;
            rest.clear();
        }
        std::string line = NormalizeSceneLine(std::move(raw));

        uint32 botLow = 0;
        uint32 otherLow = 0;
        std::string cleaned;
        auto match = [&line](std::string const& prefix)
        {
            return line.size() >= prefix.size()
                && line.compare(0, prefix.size(), prefix) == 0;
        };
        if (match("A:") || match(a->GetName() + std::string(":")))
        {
            botLow = botA.GetCounter();
            otherLow = botB.GetCounter();
            cleaned = CleanLine(std::move(line), match("A:")
                ? std::string("A:") : a->GetName() + std::string(":"));
        }
        else if (match("B:") || match(b->GetName() + std::string(":")))
        {
            botLow = botB.GetCounter();
            otherLow = botA.GetCounter();
            cleaned = CleanLine(std::move(line), match("B:")
                ? std::string("B:") : b->GetName() + std::string(":"));
        }
        if (!botLow || cleaned.size() < 2)
            continue;                        // narration/garbage line: skip

        g_sceneQueue.push_back({ sceneId, botLow, otherLow, fire, cleaned });
        fire += urand(3500, 6500);           // a natural back-and-forth pace
        ++scheduled;
    }
    if (!scheduled)
        LOG_INFO("server", "[livingchatter] scene script unparseable: '{}'",
            text.substr(0, 80));
}

/* -------------------------------------------------------------------------- */
/*  GM test hooks (.aichat test ...) - bypass Enabled + cooldowns so the       */
/*  feature can be trialed on a realm before switching it on. Budget applies.  */
/* -------------------------------------------------------------------------- */
bool WlLivingForceAmbient(Player* around)
{
    return around && TryAmbient(around, /*force=*/true);
}

bool WlLivingForceScene(Player* around)
{
    return around && TryScene(around, /*force=*/true);
}

/* -------------------------------------------------------------------------- */
/*  WorldScript: config + the scheduler + the scene-line pump                  */
/* -------------------------------------------------------------------------- */
class WowLegendsLivingChatterWorld : public WorldScript
{
public:
    WowLegendsLivingChatterWorld() : WorldScript("WowLegendsLivingChatterWorld",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>(
            "WowLegends.LivingChatter.Enabled", false);
        g_ambientSec = std::max<uint32>(30, sConfigMgr->GetOption<uint32>(
            "WowLegends.LivingChatter.AmbientIntervalSec", 120));
        g_sceneSec = std::max<uint32>(60, sConfigMgr->GetOption<uint32>(
            "WowLegends.LivingChatter.SceneIntervalSec", 420));
        g_witnessRange = sConfigMgr->GetOption<float>(
            "WowLegends.LivingChatter.WitnessRange", 30.0f);
    }

    void OnUpdate(uint32 diff) override
    {
        g_worldMs += diff;

        // the scene pump runs even when the scheduler is off, so GM-forced
        // test scenes still play out. A scene ABORTS (remaining lines
        // dropped) when either actor dies or the pair drifts out of earshot.
        if (!g_sceneQueue.empty())
        {
            uint32 abortScene = 0;
            for (auto it = g_sceneQueue.begin(); it != g_sceneQueue.end();)
            {
                if (abortScene && it->sceneId == abortScene)
                {
                    it = g_sceneQueue.erase(it);
                    continue;
                }
                if (!Due(g_worldMs, it->fireMs))
                {
                    ++it;
                    continue;
                }
                Player* bot = ObjectAccessor::FindPlayer(
                    ObjectGuid(HighGuid::Player, it->botLow));
                Player* other = ObjectAccessor::FindPlayer(
                    ObjectGuid(HighGuid::Player, it->otherLow));
                if (!bot || !other || !bot->IsAlive() || !other->IsAlive()
                    || !bot->IsWithinDist(other, 40.0f))
                    abortScene = it->sceneId;
                else
                    bot->Say(it->text, LANG_UNIVERSAL);
                it = g_sceneQueue.erase(it);
            }
        }

        if (!g_enabled.load())
        {
            g_timersArmed = false;
            return;
        }
        g_tickAccum += diff;
        if (g_tickAccum < TICK_MS)
            return;
        g_tickAccum = 0;

        if (!g_timersArmed)
        {
            g_timersArmed = true;
            g_nextAmbientMs = g_worldMs + Jitter(g_ambientSec);
            g_nextSceneMs = g_worldMs + Jitter(g_sceneSec);
        }

        if (Due(g_worldMs, g_nextAmbientMs))
        {
            g_nextAmbientMs = g_worldMs + Jitter(g_ambientSec);
            if (Player* witness = PickWitness())
                TryAmbient(witness, /*force=*/false);
        }
        if (Due(g_worldMs, g_nextSceneMs))
        {
            g_nextSceneMs = g_worldMs + Jitter(g_sceneSec);
            if (Player* witness = PickWitness())
                TryScene(witness, /*force=*/false);
        }
    }
};

void AddWowLegendsSocialChatterScripts();
void AddWowLegendsBotAuctionScripts();

void AddWowLegendsLivingChatterScripts()
{
    new WowLegendsLivingChatterWorld();
    AddWowLegendsSocialChatterScripts();
    AddWowLegendsBotAuctionScripts();
}
