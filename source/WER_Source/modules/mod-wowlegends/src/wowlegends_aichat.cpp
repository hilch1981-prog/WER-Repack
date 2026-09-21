/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - Bot AI chat (proof of concept).
 *
 * Directed-only: triggers ONLY when a REAL player whispers a bot. The LLM call
 * is done on a worker thread (never blocks the world tick), and the reply is
 * delivered back on the main thread (WorldScript::OnUpdate) so we only ever
 * touch game objects from the map thread.
 *
 * Backend: Ollama HTTP (/api/generate). Provider abstraction + API-key/CPU
 * backends + party/guild triggers + budget come later; this proves the chain.
 */

#include "ScriptMgr.h"
#include "WowLegendsAiHttpStream.h"
#include "WowLegendsKoreanSpeech.h"
#include "Log.h"
#include "Player.h"
#include "Group.h"
#include "GroupReference.h"
#include "Guild.h"
#include "Chat.h"
#include "ChannelMgr.h"
#include "WorldSession.h"
#include "ObjectAccessor.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "SharedDefines.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "Cell.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Random.h"
#include "ObjectMgr.h"
#include "Trainer.h"
#include "Map.h"
#include "../../mod-enhanced-worldchat/src/WorldChat.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <exception>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/system/error_code.hpp>
#include <openssl/err.h>     // ERR_get_error for SNI error mapping

// SO_RCVTIMEO / SO_SNDTIMEO socket deadlines (see ApplySocketTimeout)
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/time.h>
#endif

// WOW Legends talk-AND-command: dispatching a bot ORDER uses mod-playerbots'
// own cross-thread-safe entry (PlayerbotAI::HandleCommand only enqueues into
// deferredChatCommands; the bot executes on its OWN tick - the #2474-safe
// path by construction). Same static `modules` lib, headers resolve directly.
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"

// WOW Legends: companion long-term memory retell, defined in
// wowlegends_companion_memory.cpp. Returns persona text (or "") when the
// speaker is the owner of companion `botGuid`.
extern std::string WlCompanionMemoryNarrative(uint32 botGuid, uint32 speakerGuid);
// WOW Legends: bot social memory (acquaintance / grudges), defined in
// wowlegends_botsocial.cpp. Returns persona text (or "") describing how this
// bot knows the speaker: meetings, last zone, shared history, warmth.
extern std::string WlBotSocialNarrative(uint32 botLow, uint32 playerLow);
// WOW Legends "The Sage" (wowlegends_sage.cpp): server-true facts for
// question-shaped messages, from READ-ONLY startup stores. World thread.
extern std::string WlBuildSageFacts(Player* speaker, std::string const& msg);
extern void WlSageWarmup();
// WOW Legends "Voice Cards" (wowlegends_voicecard.cpp): a deterministic
// per-bot personality (from GUID) layered on top of race/faction.
extern std::string WlVoiceCardPersona(uint32 guidLow);
// WOW Legends "Speech Governor" (wowlegends_speechgov.cpp): area cadence gate
// for bot CHATTER. prio 0 ambient / 2 greeting; directed replies bypass it.
extern bool WlSpeechAllow(Player* who, int prio, uint64 beat);
#include <cstring>           // std::strtoul, std::strncmp

// EXTERN (wowlegends_livingchatter.cpp): deliver a finished ambient line /
// bot-to-bot scene script. World thread (called from this module's OnUpdate).
void WlLivingDeliver(uint32 kind, ObjectGuid botA, ObjectGuid botB,
    std::string const& text);
void WlSocialDeliver(uint32 requestId, ObjectGuid botA, ObjectGuid botB, std::string const& text);
void WlSocialPlayerSpoke(Player* player, uint32 chatType);

// EXTERN (wowlegends_botflags.cpp): "The Guide" toggle - gates the guide
// verb at parse AND dispatch so a disabled feature never acks a promise.
extern bool WlBotGuideEnabled();

namespace
{
    // Request kinds. DIRECTED is player-driven chat and is never budgeted;
    // everything else is Living Chatter and counts against the daily budget.
    enum AiKind : uint32
    {
        AI_KIND_DIRECTED = 0,
        AI_KIND_GREETING = 1,   // bot-initiated recognition whisper
        AI_KIND_AMBIENT  = 2,   // one bot says a flavor line out loud
        AI_KIND_SCENE    = 3,   // two bots act a short dialogue
        AI_KIND_ORDER    = 4,   // talk-AND-command: whisper matched to a bot
        AI_KIND_SOCIAL   = 5,   // bounded, witnessed guild/group two-bot exchange
                                // order; the world-thread drain dispatches it
    };

    struct AiRequest { ObjectGuid bot; ObjectGuid player; std::string botName; std::string persona; std::string message; uint32 chatType; std::string model; uint32 kind; bool orderable = false; bool partyWide = false; uint32 channelId = 0; std::string channelName; };
    struct AiResult  { ObjectGuid bot; ObjectGuid player; std::string botName; std::string reply; uint32 chatType; uint32 kind; std::string order; bool partyWide = false; std::string orderTarget; uint32 channelId = 0; std::string channelName; };

    std::mutex g_inMtx, g_outMtx;
    std::deque<AiRequest> g_in;
    std::deque<AiResult>  g_out;
    std::condition_variable g_cv;
    std::atomic<bool> g_run{ false };
    std::vector<std::thread> g_workers;              // joined at shutdown
    std::unordered_map<uint64, time_t> g_cooldown;   // AngerKey(bot,player) -> next allowed (main thread only)
    std::unordered_map<uint64, time_t> g_orderCooldown; // talk-AND-command 2s throttle per (bot,player); main thread only
    std::unordered_map<uint32, time_t> g_markCooldown;  // owner low -> named-attack selection hold (~2s); main thread only
    std::unordered_map<uint32, std::pair<std::string, time_t>> g_partyConfirm; // owner low -> (verb, expires): party-wide home/grind say-it-again gate
    std::unordered_set<uint32> g_passiveParked;      // bot low: WE parked it passive via a flee order; passive
                                                     // silently vetoes combat actions, so a later combat order
                                                     // to a parked bot gets a '-passive' wake first (main thread only)
    std::unordered_map<uint32, time_t> g_botFloor;   // botGuid low -> next outbound; 2s floor so one
                                                     // bot can't machine-gun many players at once

    /* ---------------------------------------------------------------------- */
    /*  Telemetry (.aichat stats). Atomics: bumped from workers + world thread */
    /* ---------------------------------------------------------------------- */
    struct AiStats
    {
        std::atomic<uint64> requested{0};     // enqueued into the worker queue
        std::atomic<uint64> queueDrops{0};    // dropped: queue full
        std::atomic<uint64> failures{0};      // provider returned nothing
        std::atomic<uint64> replies{0};       // worker produced a reply
        std::atomic<uint64> delivered{0};     // actually handed to a session
        std::atomic<uint64> dedupeDrops{0};   // dropped as near-duplicates
        std::atomic<uint64> promptTokens{0};  // REAL usage as reported by the
        std::atomic<uint64> replyTokens{0};   // provider (0 when not reported)
        std::atomic<uint64> orders{0};        // talk-AND-command orders dispatched
        std::atomic<uint64> orderParseMiss{0};// orderable replies that broke the JSON contract
    };
    AiStats g_stats;                          // since worldserver start
    AiStats g_statsToday;                     // rolls over at UTC midnight
    std::atomic<uint32> g_statsDay{0};        // unix-day stamp of "today"
    std::atomic<uint32> g_workerCount{0};     // live worker threads

    // Living Chatter accounting (kind != DIRECTED). Requests are gated by a
    // DAILY budget - on the hosted proxy every request costs one credit, so
    // the budget knob is in REQUESTS, not tokens. Tokens tracked for stats.
    std::atomic<uint64> g_livingReqToday{0};
    std::atomic<uint64> g_livingTokensToday{0};
    std::atomic<uint64> g_livingLines{0};     // ambient lines delivered
    std::atomic<uint64> g_livingScenes{0};    // scenes delivered
    std::atomic<uint64> g_livingGreets{0};    // AI greetings delivered
    thread_local uint64 t_callTokens = 0;     // this worker's current call

    // Bump one counter in BOTH buckets.
    void Stat(std::atomic<uint64> AiStats::* f, uint64 n = 1)
    {
        (g_stats.*f) += n;
        (g_statsToday.*f) += n;
    }

    void ResetStats(AiStats& s)
    {
        s.requested = 0;
        s.queueDrops = 0;
        s.failures = 0;
        s.replies = 0;
        s.delivered = 0;
        s.dedupeDrops = 0;
        s.promptTokens = 0;
        s.replyTokens = 0;
        s.orders = 0;
        s.orderParseMiss = 0;
    }

    std::string ToLower(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // Everything the WORKER threads read per request lives in this cache -
    // the core's ConfigMgr reads take NO lock, so a worker touching
    // sConfigMgr races `.reload config` repopulating the map on the world
    // thread. Refreshed in OnAfterConfigLoad (startup + every reload).
    struct WorkerCfg
    {
        std::string host = "127.0.0.1";
        std::string port = "11434";
        std::string system = "You are a character in World of Warcraft. "
                             "Reply with one short in-character sentence.";
        std::string provider = "ollama";
        std::string apiUrl;
        std::string apiKey;
        uint32 maxTokens = 120;
        float  temp = 0.7f;
        uint32 timeoutMs = 20000;
        bool   verifyTls = false;
        bool   noTrailQ = true;
        bool   koreanOnly = false;
    };
    std::mutex g_cfgMtx;
    WorkerCfg g_cfg;

    WorkerCfg CfgCopy()
    {
        std::lock_guard<std::mutex> lk(g_cfgMtx);
        return g_cfg;
    }

    void RefreshWorkerCfg()   // world thread only
    {
        WorkerCfg c;
        c.host = sConfigMgr->GetOption<std::string>("WowLegends.AiChat.OllamaHost", c.host);
        c.port = sConfigMgr->GetOption<std::string>("WowLegends.AiChat.OllamaPort", c.port);
        c.system = sConfigMgr->GetOption<std::string>("WowLegends.AiChat.System", c.system);
        c.provider = ToLower(sConfigMgr->GetOption<std::string>("WowLegends.AiChat.Provider", c.provider));
        c.apiUrl = sConfigMgr->GetOption<std::string>("WowLegends.AiChat.ApiUrl", "");
        c.apiKey = sConfigMgr->GetOption<std::string>("WowLegends.AiChat.ApiKey", "");
        c.maxTokens = sConfigMgr->GetOption<uint32>("WowLegends.AiChat.MaxTokens", c.maxTokens);
        c.temp = sConfigMgr->GetOption<float>("WowLegends.AiChat.Temperature", c.temp);
        c.timeoutMs = sConfigMgr->GetOption<uint32>("WowLegends.AiChat.TimeoutMs", c.timeoutMs);
        c.verifyTls = sConfigMgr->GetOption<bool>("WowLegends.AiChat.VerifyTLS", c.verifyTls);
        c.noTrailQ = sConfigMgr->GetOption<bool>("WowLegends.AiChat.NoTrailingQuestions", c.noTrailQ);
        c.koreanOnly = sConfigMgr->GetOption<bool>("WowLegends.AiChat.KoreanOnly", false);
        std::lock_guard<std::mutex> lk(g_cfgMtx);
        g_cfg = std::move(c);
    }

    bool        CfgEnabled()  { return sConfigMgr->GetOption<bool>("WowLegends.AiChat.Enabled", false); }
    std::string CfgHost()     { return CfgCopy().host; }
    std::string CfgPort()     { return CfgCopy().port; }
    // Empty = let the endpoint pick (the hosted WL proxy enforces its own model
    // server-side). Direct providers (ollama / your own cloud key) need a name.
    std::string CfgModel()    { return sConfigMgr->GetOption<std::string>("WowLegends.AiChat.Model", ""); }
    // fast/cheap model for high-volume AMBIENT chatter (proximity, party chime-ins); falls back to Model
    // ModelFast: missing OR empty -> fall back to Model (one-model setup is the recommended
    // default; a separate ambient model is an advanced Ollama-only optimization).
    std::string CfgModelFast()
    {
        std::string v = sConfigMgr->GetOption<std::string>("WowLegends.AiChat.ModelFast", "");
        return v.empty() ? CfgModel() : v;
    }
    uint32      CfgCooldown() { return sConfigMgr->GetOption<uint32>("WowLegends.AiChat.CooldownSeconds", 5); }
    std::string CfgSystem()   { return CfgCopy().system; }
    bool   CfgRudeActions() { return sConfigMgr->GetOption<bool>("WowLegends.AiChat.RudeActions", true); }
    uint32 CfgLeaveAnger()  { return sConfigMgr->GetOption<uint32>("WowLegends.AiChat.LeavePartyAnger", 4); }
    uint32 CfgRoarAnger()   { return sConfigMgr->GetOption<uint32>("WowLegends.AiChat.RoarAnger", 6); }
    uint32 CfgMemoryTurns() { return sConfigMgr->GetOption<uint32>("WowLegends.AiChat.MemoryTurns", 12); }
    // drop a reply that near-repeats one of the bot's own last two lines (anti-loop)
    bool   CfgDedupe()      { return sConfigMgr->GetOption<bool>("WowLegends.AiChat.Dedupe", true); }
    // worker THREADS doing provider round-trips: more = several bots can answer
    // at once instead of queueing behind one slow call. Read at STARTUP only.
    uint32 CfgWorkers()
    {
        uint32 n = sConfigMgr->GetOption<uint32>("WowLegends.AiChat.Workers", 2);
        if (n < 1) n = 1;
        if (n > 4) n = 4;
        return n;
    }
    // Living Chatter: daily cap on budgeted (non-directed) AI requests, and
    // whether recognition greetings may use the AI instead of canned lines.
    uint32 CfgLivingBudget()  { return sConfigMgr->GetOption<uint32>("WowLegends.LivingChatter.MaxRequestsPerDay", 600); }
    bool   CfgLivingEnabled() { return sConfigMgr->GetOption<bool>("WowLegends.LivingChatter.Enabled", false); }
    bool   CfgAiGreetings()   { return sConfigMgr->GetOption<bool>("WowLegends.LivingChatter.AiGreetings", true); }

    // WL: a message starting with the playerbot command prefix (AiPlayerbot.CommandPrefix, e.g. "!")
    // is a bot ORDER handled by mod-playerbots (e.g. "!stay", "!follow"), NOT conversation.
    // AI chat must ignore these so orders obey cleanly instead of the bot "talking back".
    // Reads the prefix from the shared config so it always matches mod-playerbots (no hardcoding).
    bool IsBotOrder(std::string const& m)
    {
        // mod-playerbots' own cached value - value AND default shared with
        // HandleCommandInternal's check, so drift is impossible.
        std::string const& p = sPlayerbotAIConfig.commandPrefix;
        return !p.empty() && m.rfind(p, 0) == 0;
    }

    /* --- talk-AND-command (slice 1) --------------------------------------- */
    // Whisper a GROUPED bot plain English and it obeys. World-thread reads
    // only (chat hook + OnUpdate drain), like the other Cfg helpers.
    bool CfgAiCommandEnabled()
    { return sConfigMgr->GetOption<bool>("WowLegends.AiCommand.Enabled", false); }

    // T&C v2: party-wide orders ("everyone follow me") + attack-by-name.
    bool CfgPartyOrders()
    { return sConfigMgr->GetOption<bool>("WowLegends.AiCommand.PartyOrders.Enabled", true); }

    uint32 CfgMaxTargetsInPrompt()
    {
        uint32 v = sConfigMgr->GetOption<uint32>("WowLegends.AiCommand.MaxTargetsInPrompt", 12);
        return v < 1 ? 1 : (v > 24 ? 24 : v);
    }

    // A tiny exact-phrase matcher stands in for the LLM intent parser (same
    // pipe, zero cost, zero latency - the LLM lands in the next slice).
    // Returns the bare mod-playerbots verb to dispatch; "" = not an order,
    // the message stays normal AI chat.
    // lowercase, strip punctuation, collapse whitespace - shared by the
    // matcher, the group-address stripper and attack-by-name matching
    std::string NormalizeOrderText(std::string const& msg)
    {
        std::string norm;
        norm.reserve(msg.size());
        bool space = true;
        for (unsigned char c : msg)
        {
            if (std::ispunct(c))
                continue;
            if (std::isspace(c))
            {
                if (!space)
                    norm += ' ';
                space = true;
                continue;
            }
            norm += char(std::tolower(c));
            space = false;
        }
        if (!norm.empty() && norm.back() == ' ')
            norm.pop_back();
        return norm;
    }

    std::string MatchOrderIntentNorm(std::string const& norm)
    {
        // unambiguous imperatives ONLY - conversational interjections like
        // bare "wait" / "lets go" stay AI chat until the LLM can disambiguate
        if (norm == "follow" || norm == "follow me" || norm == "come with me")
            return "follow";
        if (norm == "stay" || norm == "stay here" || norm == "stay put"
            || norm == "wait here" || norm == "hold here"
            || norm == "hold position")
            return "stay";
        if (norm == "attack" || norm == "attack my target"
            || norm == "attack it" || norm == "kill it")
            return "attack";
        if (norm == "flee")
            return "flee";
        if (norm == "drink" || norm == "eat")
            return "drink";
        if (norm == "come here" || norm == "come to me")
            return "summon";
        if (norm == "go home" || norm == "hearth" || norm == "hearthstone"
            || norm == "use hearthstone" || norm == "use heartstone")
            return "home";
        return "";
    }

    std::string MatchOrderIntent(std::string const& msg)
    {
        return MatchOrderIntentNorm(NormalizeOrderText(msg));
    }

    // T&C v2: "everyone follow me" -> "follow me". Strips leading
    // group-address words from an already-NORMALIZED message. Returns the
    // address STRENGTH: 2 = unambiguous ("everyone", "bots", "yall" ... -
    // free-form phrasings may go to the LLM), 1 = weak ("all", "guys",
    // "team" - routine banter starts with these, so ONLY an exact verb or
    // named-attack match may consume the message), 0 = not group-addressed.
    int StripGroupAddress(std::string& norm)
    {
        static char const* const strongPrefixes[] =
        {
            "everyone ", "everybody ", "all of you ", "all bots ", "bots ",
            "you all ", "you guys ", "yall ",
        };
        static char const* const weakPrefixes[] =
        {
            "guys ", "lads ", "team ", "all ",
        };
        int strength = 0;
        for (bool again = true; again; )
        {
            again = false;
            for (char const* p : strongPrefixes)
            {
                std::size_t const n = std::char_traits<char>::length(p);
                if (norm.size() > n && norm.compare(0, n, p) == 0)
                {
                    norm.erase(0, n);
                    strength = 2;
                    again = true;
                }
            }
            for (char const* p : weakPrefixes)
            {
                std::size_t const n = std::char_traits<char>::length(p);
                if (norm.size() > n && norm.compare(0, n, p) == 0)
                {
                    norm.erase(0, n);
                    strength = strength < 1 ? 1 : strength;
                    again = true;
                }
            }
        }
        return strength;
    }

    // T&C v2 attack-by-name: "attack the young nightsaber" -> "young
    // nightsaber" (normalized). "" = not an attack-by-name (the plain-verb
    // forms "attack it"/"attack my target" stay with the exact matcher).
    std::string ParseAttackTargetName(std::string const& norm)
    {
        std::string rest;
        if (norm.rfind("attack ", 0) == 0)
            rest = norm.substr(7);
        else if (norm.rfind("kill ", 0) == 0)
            rest = norm.substr(5);
        else
            return "";

        for (char const* art : { "the ", "that ", "this ", "an ", "a " })
        {
            std::size_t const n = std::char_traits<char>::length(art);
            if (rest.rfind(art, 0) == 0)
            {
                rest.erase(0, n);
                break;
            }
        }
        // trailing courtesy tokens: "attack the boar please/now/first"
        for (bool again = true; again; )
        {
            again = false;
            for (char const* tail : { " please", " now", " first" })
            {
                std::size_t const n = std::char_traits<char>::length(tail);
                if (rest.size() > n
                    && rest.compare(rest.size() - n, n, tail) == 0)
                {
                    rest.erase(rest.size() - n);
                    again = true;
                }
            }
        }
        if (rest == "it" || rest == "them" || rest == "my target"
            || rest.size() < 3)
            return "";
        return rest;
    }

    // T&C "The Guide": "take me to the crossroads" -> "crossroads"
    // (normalized). "" = not a guide phrasing.
    std::string ParseGuideTargetName(std::string const& norm)
    {
        std::string rest;
        for (char const* lead : { "take me to ", "lead me to ", "guide me to ",
                                  "bring me to ", "show me the way to ",
                                  "lead the way to ", "walk me to " })
        {
            std::size_t const n = std::char_traits<char>::length(lead);
            if (norm.rfind(lead, 0) == 0)
            {
                rest = norm.substr(n);
                break;
            }
        }
        if (rest.empty())
            return "";

        for (char const* art : { "the ", "that ", "my ", "an ", "a " })
        {
            std::size_t const n = std::char_traits<char>::length(art);
            if (rest.rfind(art, 0) == 0)
            {
                rest.erase(0, n);
                break;
            }
        }
        for (bool again = true; again; )
        {
            again = false;
            for (char const* tail : { " please", " now" })
            {
                std::size_t const n = std::char_traits<char>::length(tail);
                if (rest.size() > n
                    && rest.compare(rest.size() - n, n, tail) == 0)
                {
                    rest.erase(rest.size() - n);
                    again = true;
                }
            }
        }
        if (rest.size() < 3)
            return "";
        return rest;
    }

    // talk-AND-command verb table: the LLM/phrase verb -> the mod-playerbots
    // chat trigger it dispatches + the canned fast-path ack. Every dispatch
    // token is verified against ChatCommandHandlerStrategy.cpp (a wrong
    // token silently no-ops). "heal" is special-cased at dispatch: it needs
    // the owner's name as a parameter ("focus heal +<name>").
    struct OrderVerb
    {
        char const* verb;
        char const* dispatch;
        char const* ack;
    };
    constexpr OrderVerb ORDER_VERBS[] =
    {
        { "follow",  "follow",      "On it - right behind you." },
        { "stay",    "stay",        "Holding here." },
        { "attack",  "attack",      "On it - attacking your mark." },
        { "tank",    "tank attack", "I'll hold its attention - hit it." },
        { "pull",    "pull",        "Pulling it - get ready." },
        { "flee",    "flee",        "Falling back to you!" },
        { "grind",   "grind",       "Clearing this place out." },
        { "summon",  "summon",      "On my way to you." },
        { "revive",  "revive",      "Heading for a spirit healer." },
        { "release", "release",     "Releasing." },
        { "home",    "hearthstone", "Hearthing home." },   // "$home" only SETS
                                                           // the hearth (needs
                                                           // an innkeeper)
        { "drink",   "drink",       "Taking a moment to eat and drink." },
        { "reset",   "reset botAI", "Shaking it off - back to my senses." },
        { "heal",    "",            "Keeping you topped up." },
        { "guide",   "",            "Follow me - I know the way." },
    };

    OrderVerb const* FindOrderVerb(std::string const& verb)
    {
        for (OrderVerb const& v : ORDER_VERBS)
            if (verb == v.verb)
                return &v;
        return nullptr;
    }

    // The whispered ack, so the loop never feels dead (fast path: canned;
    // the AI path speaks its own in-character line instead).
    std::string OrderAck(std::string const& verb)
    {
        OrderVerb const* v = FindOrderVerb(verb);
        return v ? v->ack : "On it.";
    }

    // T&C v2 attack-by-name. Both helpers are WORLD-THREAD ONLY (chat hook /
    // OnUpdate drain top-level frames): they visit the grid around the OWNER.

    // Names of attackable units near the owner, deduped, capped - injected
    // into the order prompt so the model can copy a real "target" value.
    std::string BuildTargetTable(Player* owner)
    {
        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(owner, owner, 40.0f);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(owner, units, check);
        Cell::VisitObjects(owner, searcher, 40.0f);

        std::string out;
        std::unordered_set<std::string> seen;
        uint32 count = 0;
        for (Unit* u : units)
        {
            if (!u->IsAlive() || !u->IsInWorld() || !owner->IsValidAttackTarget(u))
                continue;
            std::string const& name = u->GetName();
            if (name.empty() || !seen.insert(name).second)
                continue;
            if (!out.empty())
                out += ", ";
            out += name;
            if (++count >= CfgMaxTargetsInPrompt())
                break;
        }
        return out;
    }

    // Re-resolve a spoken enemy name against the LIVE grid at dispatch time.
    // Exact normalized match beats substring; ties go to the nearest unit.
    Unit* FindNearbyEnemyByName(Player* owner, std::string const& spokenName)
    {
        std::string const query = NormalizeOrderText(spokenName);
        if (query.empty())
            return nullptr;

        std::list<Unit*> units;
        Acore::AnyUnfriendlyUnitInObjectRangeCheck check(owner, owner, 60.0f);
        Acore::UnitListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(owner, units, check);
        Cell::VisitObjects(owner, searcher, 60.0f);

        Unit* best = nullptr;
        int bestRank = 0;
        float bestDist = 0.0f;
        for (Unit* u : units)
        {
            if (!u->IsAlive() || !u->IsInWorld() || !owner->IsValidAttackTarget(u))
                continue;
            std::string const name = NormalizeOrderText(u->GetName());
            int rank = 0;
            if (name == query)
                rank = 2;
            else if (name.find(query) != std::string::npos)
                rank = 1;
            if (!rank)
                continue;
            float const d = owner->GetDistance(u);
            if (!best || rank > bestRank || (rank == bestRank && d < bestDist))
            {
                best = u;
                bestRank = rank;
                bestDist = d;
            }
        }
        return best;
    }

    // Hook-time gate for the deterministic attack-by-name fast path: only a
    // name that RESOLVES against the live grid right now commits to the fast
    // path ("kill time" stays chat); a trailing plural retries singular.
    // Adjusts `name` to the form that resolved. WORLD THREAD ONLY.
    bool AttackNameResolvesNearby(Player* owner, std::string& name)
    {
        if (name.empty())
            return false;
        if (FindNearbyEnemyByName(owner, name))
            return true;
        if (name.size() > 3 && name.back() == 's')
        {
            std::string singular = name.substr(0, name.size() - 1);
            if (FindNearbyEnemyByName(owner, singular))
            {
                name = singular;
                return true;
            }
        }
        return false;
    }

    // Per-race / per-faction flavour appended to the persona (ONE race + ONE faction per call).
    // Keys: WowLegends.AiChat.Race.<Name> and WowLegends.AiChat.Faction.Alliance/.Horde.
    std::string CfgRace(char const* key, char const* def)
    { return sConfigMgr->GetOption<std::string>(std::string("WowLegends.AiChat.Race.") + key, def); }
    std::string CfgFactionAlliance()
    { return sConfigMgr->GetOption<std::string>("WowLegends.AiChat.Faction.Alliance",
        "You stand for the Alliance: bound by honor, duty, and the defense of the innocent. Treat the Horde as savage rivals to be opposed, though the Scourge is the greater threat."); }
    std::string CfgFactionHorde()
    { return sConfigMgr->GetOption<std::string>("WowLegends.AiChat.Faction.Horde",
        "You stand for the Horde: bound by strength, survival, and fierce loyalty to your people. Treat the Alliance as soft rivals to be challenged, though you'll unite against the Scourge when you must."); }

    // Map a 3.3.5a race id -> (config-key, baked-in default flavour line). "" key = no injection.
    // Only the BOT'S OWN race line is added per call, so this is token-cheap (one line, not a blob).
    void RaceFlavor(uint8 race, char const*& key, char const*& def)
    {
        switch (race)
        {
            case 1:  key = "Human";    def = "You are a human of the Alliance: earnest, dutiful, and resolute, speaking plainly with steady resolve and faith in the kingdoms of men. You rally to honor, hold the line against the Scourge, and treat any cause as worth fighting for."; break;
            case 2:  key = "Orc";      def = "You are an orc of the Horde: blunt, warlike, and fiercely honor-bound, speaking in short, gruff bursts with pride in strength and the Horde. You respect a worthy foe, scorn cowardice, and would sooner charge than bargain."; break;
            case 3:  key = "Dwarf";    def = "You are a dwarf of Khaz Modan: hearty, gruff, and quick to laugh, fond of ale, gunpowder, and a good brawl, peppering your speech with 'lad' and 'aye'. You face danger with a grin and never back down from a fight."; break;
            case 4:  key = "NightElf"; def = "You are a night elf, ancient and aloof, bound to nature and the goddess Elune, speaking with formal, measured solemnity earned over ten thousand years. You regard mortals' haste with patient disdain and guard the wilds with quiet vigilance."; break;
            case 5:  key = "Undead";   def = "You are a Forsaken of the Undercity: cold, sardonic, and bitter, dripping with gallows humor and contempt for the living you once were. You serve the Dark Lady, trust no one fully, and find grim amusement in death."; break;
            case 6:  key = "Tauren";   def = "You are a tauren of Mulgore: calm, wise, and deeply spiritual, reverent of the Earth Mother and slow to anger, speaking with gentle, deliberate dignity. You seek balance and harmony, but your fury, once roused, is like the thunder."; break;
            case 7:  key = "Gnome";    def = "You are a gnome of Gnomeregan: chipper, hyper-intelligent, and irrepressibly curious, prone to tangents about gadgets, gears, and grand inventions, talking fast and a touch verbosely. You meet every problem with bright optimism and a clever contraption."; break;
            case 8:  key = "Troll";    def = "You are a Darkspear troll, laid-back and mystical, speakin' wit' a relaxed 'mon' cadence and a fondness for the loa and old superstitions. You stay cool under pressure, trust da spirits, and got a sly sense of humor."; break;
            case 10: key = "BloodElf"; def = "You are a blood elf of Silvermoon: haughty, refined, and elegant, carrying yourself with cultured superiority and a barely-veiled hunger for arcane magic. You value beauty and power, and regard lesser beings with cool, condescending grace."; break;
            case 11: key = "Draenei";  def = "You are a draenei, serene and devout, formal in speech and unwavering in faith in the Light and the Naaru, often invoking their blessing. You bear ages of exile with dignified patience and offer wisdom with calm, otherworldly grace."; break;
            default: key = "";         def = ""; break;
        }
    }
    bool   CfgPartyChat()   { return sConfigMgr->GetOption<bool>("WowLegends.AiChat.PartyChat", true); }
    bool   CfgGuildChat()   { return sConfigMgr->GetOption<bool>("WowLegends.AiChat.GuildChat", true); }
    uint32 CfgAmbientChance(){ return std::min<uint32>(100, sConfigMgr->GetOption<uint32>("WowLegends.AiChat.PartyAmbientChance", 80)); }
    bool   CfgProximityChat()  { return sConfigMgr->GetOption<bool>("WowLegends.AiChat.ProximityChat", true); }
    float  CfgProximityRange() { return sConfigMgr->GetOption<float>("WowLegends.AiChat.ProximityRange", 20.0f); }
    uint32 CfgProximityChance(){ return std::min<uint32>(100, sConfigMgr->GetOption<uint32>("WowLegends.AiChat.ProximityChance", 80)); }
    uint32 CfgGuildChance()    { return std::min<uint32>(100, sConfigMgr->GetOption<uint32>("WowLegends.AiChat.GuildPlayerReplyChance", 80)); }
    bool   CfgPublicChannelChat() { return sConfigMgr->GetOption<bool>("WowLegends.AiChat.PublicChannelChat", true); }
    uint32 CfgPublicPlayerChance(){ return std::min<uint32>(100, sConfigMgr->GetOption<uint32>("WowLegends.AiChat.PublicPlayerReplyChance", 80)); }
    uint32 CfgPublicBotChance()   { return std::min<uint32>(100, sConfigMgr->GetOption<uint32>("WowLegends.AiChat.PublicBotReplyChance", 1)); }

    // Provider-abstraction config - WORKER-SAFE (served from the cache).
    std::string CfgProvider()  { return CfgCopy().provider; }
    std::string CfgApiUrl()    { return CfgCopy().apiUrl; }
    std::string CfgApiKey()    { return CfgCopy().apiKey; }
    uint32      CfgMaxTokens() { return CfgCopy().maxTokens; }
    float       CfgTemp()      { return CfgCopy().temp; }
    uint32      CfgTimeoutMs() { return CfgCopy().timeoutMs; }
    bool        CfgVerifyTLS() { return CfgCopy().verifyTls; }
    bool        CfgNoTrailQ()  { return CfgCopy().noTrailQ; }

    // Per (bot,player) anger meter + rolling chat history. MAIN THREAD ONLY.
    std::unordered_map<uint64, int> g_anger;
    std::unordered_map<uint64, std::deque<std::string>> g_history;   // recent "Who: line" turns
    uint64 AngerKey(uint32 bot, uint32 pl) { return (static_cast<uint64>(bot) << 32) | pl; }

    // keep the rolling window to (turns * 2) lines, at least one exchange.
    // The configured MemoryTurns is honored fully (no extra cap) so large values
    // like 100 work; a sane safety ceiling avoids a runaway blob in the persona/DB.
    void TrimHistory(std::deque<std::string>& dq)
    {
        uint32 turns = CfgMemoryTurns();
        if (turns < 1)   turns = 1;
        if (turns > 200) turns = 200;     // safety ceiling: 200 turns = 400 lines, MEDIUMTEXT-safe
        uint32 maxLines = turns * 2;
        while (dq.size() > maxLines) dq.pop_front();
    }

    /* ---------------------------------------------------------------------- */
    /*  Persistent chat memory (characters DB) — MODULE-CONTAINED, no core     */
    /*  edits. The (bot,player) rolling history + anger survive restarts/days. */
    /*  All access is MAIN THREAD ONLY (chat hooks + WorldScript::OnUpdate);   */
    /*  the worker thread never touches g_history/g_anger/g_loaded.            */
    /* ---------------------------------------------------------------------- */

    // Base64 (standard alphabet). Encode the serialized history before storing it
    // via a plain string Execute() so quotes / newlines / UTF-8 can't corrupt the
    // SQL or the blob — we have no prepared statement (that would need a core edit).
    std::string Base64Encode(std::string const& in)
    {
        static char const* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve(((in.size() + 2) / 3) * 4);
        std::size_t i = 0;
        while (i + 3 <= in.size())
        {
            unsigned n = (static_cast<unsigned char>(in[i]) << 16)
                       | (static_cast<unsigned char>(in[i + 1]) << 8)
                       |  static_cast<unsigned char>(in[i + 2]);
            out += tbl[(n >> 18) & 0x3F]; out += tbl[(n >> 12) & 0x3F];
            out += tbl[(n >> 6)  & 0x3F]; out += tbl[n & 0x3F];
            i += 3;
        }
        if (i + 1 == in.size())
        {
            unsigned n = static_cast<unsigned char>(in[i]) << 16;
            out += tbl[(n >> 18) & 0x3F]; out += tbl[(n >> 12) & 0x3F];
            out += "==";
        }
        else if (i + 2 == in.size())
        {
            unsigned n = (static_cast<unsigned char>(in[i]) << 16)
                       | (static_cast<unsigned char>(in[i + 1]) << 8);
            out += tbl[(n >> 18) & 0x3F]; out += tbl[(n >> 12) & 0x3F];
            out += tbl[(n >> 6)  & 0x3F]; out += '=';
        }
        return out;
    }

    std::string Base64Decode(std::string const& in)
    {
        auto val = [](unsigned char c) -> int
        {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '+') return 62;
            if (c == '/') return 63;
            return -1;   // '=' padding or any stray char
        };
        std::string out;
        out.reserve((in.size() / 4) * 3);
        int buf = 0, bits = 0;
        for (char ch : in)
        {
            int v = val(static_cast<unsigned char>(ch));
            if (v < 0) continue;            // skip '=' / whitespace / junk
            buf = (buf << 6) | v;
            bits += 6;
            if (bits >= 8)
            {
                bits -= 8;
                out += static_cast<char>((buf >> bits) & 0xFF);
            }
        }
        return out;
    }

    // Join a history deque into one blob ('\n' row separator; lines are kept single-line
    // by sanitizing on push, see EnqueueAi / OnUpdate).
    std::string SerializeHistory(std::deque<std::string> const& dq)
    {
        std::string out;
        for (std::string const& line : dq)
        {
            if (!out.empty()) out += '\n';
            out += line;
        }
        return out;
    }

    // Split a stored blob back into history lines (drops empties).
    void DeserializeHistory(std::string const& blob, std::deque<std::string>& dq)
    {
        std::size_t start = 0;
        while (start <= blob.size())
        {
            std::size_t nl = blob.find('\n', start);
            std::string line = (nl == std::string::npos) ? blob.substr(start) : blob.substr(start, nl - start);
            if (!line.empty()) dq.push_back(line);
            if (nl == std::string::npos) break;
            start = nl + 1;
        }
    }

    // (bot,player) pairs already hydrated from the DB this session — load-once guard.
    std::unordered_set<uint64> g_loaded;

    // Load persisted history + anger for (botLow, playerLow) into the in-memory maps,
    // exactly once per pair per session. SYNCHRONOUS PK lookup (sub-ms, mirrors the
    // hardcore module's main-thread reads). MAIN THREAD ONLY.
    void LoadMemory(uint32 botLow, uint32 playerLow)
    {
        uint64 ak = AngerKey(botLow, playerLow);
        if (!g_loaded.insert(ak).second)
            return;   // already hydrated this session

        // botLow / playerLow are integers formatted into the SQL -> no injection risk.
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT history, anger FROM playerbot_ai_chat_memory WHERE bot_guid={} AND player_guid={}",
                botLow, playerLow))
        {
            Field* f = r->Fetch();
            std::string blob = Base64Decode(f[0].Get<std::string>());
            int anger        = f[1].Get<int32>();

            auto& dq = g_history[ak];
            dq.clear();
            DeserializeHistory(blob, dq);
            TrimHistory(dq);          // honour the (possibly raised) cap on load
            g_anger[ak] = anger;
            LOG_INFO("server", "[aichat] memory loaded bot={} player={} turns={} anger={}",
                     botLow, playerLow, (int)dq.size(), anger);
        }
    }

    // Persist current history + anger for (botLow, playerLow). ASYNC string Execute
    // (non-blocking on the world tick). The history is base64-encoded so the blob is
    // pure [A-Za-z0-9+/=] and cannot break the SQL; anger is a plain int. MAIN THREAD ONLY.
    void SaveMemory(uint32 botLow, uint32 playerLow)
    {
        uint64 ak = AngerKey(botLow, playerLow);
        std::string blob;
        auto hit = g_history.find(ak);
        if (hit != g_history.end())
            blob = SerializeHistory(hit->second);
        int anger = 0;
        auto ait = g_anger.find(ak);
        if (ait != g_anger.end())
            anger = ait->second;

        std::string enc = Base64Encode(blob);   // safe charset, no quotes/newlines
        CharacterDatabase.Execute(
            "REPLACE INTO playerbot_ai_chat_memory (bot_guid, player_guid, history, anger) "
            "VALUES ({}, {}, '{}', {})",
            botLow, playerLow, enc, anger);
    }

    bool LooksLikeInsult(std::string s)
    {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        static char const* words[] = {
            "fuck", "fuk", "fuc", "shit", "sh1t", "stfu", "idiot", "stupid", "noob", "loser",
            "moron", "bastard", "suck", "trash", "screw you", "piss off", "shut up", "hate you",
            "traitor", "scum", "dumb", "jerk", "coward", "pathetic", "worthless", "clown", "useless"
        };
        for (char const* w : words)
            if (s.find(w) != std::string::npos)
                return true;
        return false;
    }

    char const* RaceName(uint8 r)
    {
        switch (r)
        {
            case 1:  return "Human";    case 2:  return "Orc";       case 3:  return "Dwarf";
            case 4:  return "Night Elf"; case 5: return "Undead";    case 6:  return "Tauren";
            case 7:  return "Gnome";    case 8:  return "Troll";     case 10: return "Blood Elf";
            case 11: return "Draenei";  default: return "adventurer";
        }
    }
    char const* ClassName(uint8 c)
    {
        switch (c)
        {
            case 1:  return "warrior"; case 2:  return "paladin"; case 3:  return "hunter";
            case 4:  return "rogue";   case 5:  return "priest";  case 6:  return "death knight";
            case 7:  return "shaman";  case 8:  return "mage";    case 9:  return "warlock";
            case 11: return "druid";   default: return "adventurer";
        }
    }

    // 3.3.5a TalentTab.dbc id -> spec name (the DBC name column is unused/not loaded).
    char const* TabName(uint32 tab)
    {
        switch (tab)
        {
            case 161: return "Arms";        case 164: return "Fury";          case 163: return "Protection";   // warrior
            case 382: return "Holy";        case 383: return "Protection";    case 381: return "Retribution";  // paladin
            case 361: return "Beast Mastery"; case 363: return "Marksmanship"; case 362: return "Survival";    // hunter
            case 182: return "Assassination"; case 181: return "Combat";       case 183: return "Subtlety";    // rogue
            case 201: return "Discipline";  case 202: return "Holy";           case 203: return "Shadow";       // priest
            case 398: return "Blood";       case 399: return "Frost";          case 400: return "Unholy";       // death knight
            case 261: return "Elemental";   case 263: return "Enhancement";    case 262: return "Restoration";  // shaman
            case 81:  return "Arcane";      case 41:  return "Fire";           case 61:  return "Frost";        // mage
            case 302: return "Affliction";  case 303: return "Demonology";     case 301: return "Destruction";  // warlock
            case 283: return "Balance";     case 281: return "Feral";          case 282: return "Restoration";  // druid
            default:  return "";
        }
    }

    // Dominant talent tree for the player's ACTIVE spec (most points spent).
    std::string SpecName(Player* p)
    {
        uint8 spec = p->GetActiveSpec();
        std::unordered_map<uint32, int> perTab;
        for (auto const& kv : p->GetTalentMap())
        {
            PlayerTalent* t = kv.second;
            if (!t || t->State == PLAYERSPELL_REMOVED || !t->IsInSpec(spec))
                continue;
            if (TalentEntry const* te = sTalentStore.LookupEntry(t->talentID))
                ++perTab[te->TalentTab];
        }
        uint32 best = 0; int bestN = 0;
        for (auto const& kv : perTab)
            if (kv.second > bestN) { bestN = kv.second; best = kv.first; }
        return bestN ? TabName(best) : "";
    }

    // Name of the bot's equipped main-hand weapon ("" if unarmed).
    std::string MainHandName(Player* p)
    {
        if (Item* mh = p->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND))
            if (ItemTemplate const* it = mh->GetTemplate())
            {
                if (CfgCopy().koreanOnly)
                {
                    if (ItemLocale const* locale = sObjectMgr->GetItemLocale(it->ItemId))
                        if (locale->Name.size() > LOCALE_koKR && !locale->Name[LOCALE_koKR].empty())
                            return locale->Name[LOCALE_koKR];
                    return "이름 미확인 무기";
                }
                return it->Name1;
            }
        return "";
    }

    std::string JsonEscape(std::string const& s)
    {
        std::string o; o.reserve(s.size() + 8);
        for (char c : s)
        {
            switch (c)
            {
                case '"':  o += "\\\""; break;
                case '\\': o += "\\\\"; break;
                case '\n': o += "\\n";  break;
                case '\r': break;
                case '\t': o += ' ';    break;
                default: if (static_cast<unsigned char>(c) < 0x20) o += ' '; else o += c;
            }
        }
        return o;
    }

    // Strip glyphs the 3.3.5a client can't render (emoji etc.) so they don't show as "?".
    // Keeps ASCII + 2-byte (Latin accents: ä ñ é) + 3-byte (CJK/Cyrillic); drops 4-byte
    // sequences (emoji U+10000+) and the FE0E/FE0F variation selectors.
    std::string StripUnsupported(std::string const& in)
    {
        std::string out; out.reserve(in.size());
        for (std::size_t i = 0; i < in.size(); )
        {
            unsigned char c = static_cast<unsigned char>(in[i]);
            if (c < 0x80)                 { out += static_cast<char>(c); i += 1; }
            else if ((c & 0xE0) == 0xC0)  { out.append(in, i, 2); i += 2; }   // 2-byte: keep
            else if ((c & 0xF0) == 0xE0)                                       // 3-byte
            {
                bool varSel = (c == 0xEF && i + 2 < in.size()
                            && static_cast<unsigned char>(in[i + 1]) == 0xB8
                            && (static_cast<unsigned char>(in[i + 2]) == 0x8F
                             || static_cast<unsigned char>(in[i + 2]) == 0x8E));
                if (!varSel) out.append(in, i, 3);
                i += 3;
            }
            else if ((c & 0xF8) == 0xF0)  { i += 4; }                          // 4-byte emoji: drop
            else                          { i += 1; }                          // stray byte: drop
        }
        std::size_t b = out.find_last_not_of(" \t\r\n");
        if (b != std::string::npos) out.erase(b + 1);
        return out;
    }

    /* ---------------------------------------------------------------------- */
    /*  HTTP/HTTPS transport (one path for Ollama-local + cloud providers)     */
    /*  WORKER THREAD ONLY — blocking; never touches game objects.             */
    /* ---------------------------------------------------------------------- */

    // Parse "https://host[:port]/path" or "http://host[:port]/path".
    // Returns false if it doesn't parse. Fills scheme/host/port/path.
    bool ParseUrl(std::string const& url, bool& https, std::string& host, std::string& port, std::string& path)
    {
        std::size_t p;
        if (url.rfind("https://", 0) == 0)      { https = true;  p = 8; port = "443"; }
        else if (url.rfind("http://", 0) == 0)  { https = false; p = 7; port = "80";  }
        else return false;

        std::size_t slash = url.find('/', p);
        std::string authority = (slash == std::string::npos) ? url.substr(p) : url.substr(p, slash - p);
        path = (slash == std::string::npos) ? "/" : url.substr(slash);

        std::size_t colon = authority.find(':');
        if (colon == std::string::npos) host = authority;
        else { host = authority.substr(0, colon); port = authority.substr(colon + 1); }
        return !host.empty();
    }

    // Shared production/test implementation. Empty string remains the existing error contract.
    using WowLegendsAiHttp::ReadHttpBody;

    // Build the raw HTTP/1.1 request string.
    std::string BuildHttpRequest(std::string const& host, std::string const& port, std::string const& path,
                                 std::vector<std::string> const& headers, std::string const& body)
    {
        std::string req  = "POST " + path + " HTTP/1.1\r\n";
        req += "Host: " + host + (port == "80" || port == "443" ? "" : (":" + port)) + "\r\n";
        req += "User-Agent: WowLegends-AiChat\r\n";
        req += "Accept: application/json\r\n";
        req += "Content-Type: application/json\r\n";
        for (std::string const& h : headers) req += h + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "Connection: close\r\n\r\n";
        req += body;
        return req;
    }

    // Apply the configured request deadline (CfgTimeoutMs) to a connected socket.
    // Blocking reads/writes then error out instead of hanging the single worker
    // thread forever on a stuck provider (the error takes the existing "" path).
    void ApplySocketTimeout(boost::asio::ip::tcp::socket& sock)
    {
        uint32 ms = CfgTimeoutMs();
        if (!ms)
            return;
#ifdef _WIN32
        DWORD tv = ms;                              // milliseconds
#else
        struct timeval tv;
        tv.tv_sec  = ms / 1000;
        tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
#endif
        setsockopt(sock.native_handle(), SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<char const*>(&tv), static_cast<int>(sizeof(tv)));
        setsockopt(sock.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
                   reinterpret_cast<char const*>(&tv), static_cast<int>(sizeof(tv)));
    }

    // One transport for HTTP (Ollama-local) and HTTPS (cloud). WORKER THREAD ONLY. "" on any failure.
    std::string HttpPost(bool https, std::string const& host, std::string const& port, std::string const& path,
                         std::vector<std::string> const& headers, std::string const& body)
    {
        namespace asio = boost::asio;
        using asio::ip::tcp;
        try
        {
            asio::io_context io;
            tcp::resolver resolver(io);
            auto endpoints = resolver.resolve(host, port);
            std::string req = BuildHttpRequest(host, port, path, headers, body);

            if (!https)
            {
                tcp::socket sock(io);
                asio::connect(sock, endpoints);
                ApplySocketTimeout(sock);
                asio::write(sock, asio::buffer(req));
                return ReadHttpBody(sock);
            }

            asio::ssl::context ctx(asio::ssl::context::tls_client);
            ctx.set_options(asio::ssl::context::default_workarounds |
                            asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3);
            if (CfgVerifyTLS())
            {
                ctx.set_default_verify_paths();
                ctx.set_verify_mode(asio::ssl::verify_peer);
            }
            else
                ctx.set_verify_mode(asio::ssl::verify_none);

            asio::ssl::stream<tcp::socket> stream(io, ctx);
            if (CfgVerifyTLS())
                stream.set_verify_callback(asio::ssl::host_name_verification(host));

            // SNI — required by virtually all cloud TLS endpoints. Takes a non-const char*.
            if (!SSL_set_tlsext_host_name(stream.native_handle(), host.c_str()))
            {
                boost::system::error_code ec{ (int)::ERR_get_error(), asio::error::get_ssl_category() };
                throw boost::system::system_error(ec);
            }
            asio::connect(stream.next_layer(), endpoints);
            ApplySocketTimeout(stream.next_layer());   // the underlying TCP socket
            stream.handshake(asio::ssl::stream_base::client);
            asio::write(stream, asio::buffer(req));
            return ReadHttpBody(stream);
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("server", "[aichat] HttpPost {}://{}:{}{} failed: {}", https ? "https" : "http", host, port, path, e.what());
            return "";
        }
    }

    /* ---------------------------------------------------------------------- */
    /*  Minimal JSON helpers (string-based, no JSON lib) + thinking stripper   */
    /* ---------------------------------------------------------------------- */

    // Extract a JSON string value for `key` starting the search at/after `from`.
    // Decodes \" \\ \/ \n \r \t \uXXXX (incl. surrogate pairs). Returns "" if not found.
    std::string JsonExtractString(std::string const& body, std::string const& key, std::size_t from = 0)
    {
        std::string needle = "\"" + key + "\"";
        std::size_t k = body.find(needle, from);
        if (k == std::string::npos) return "";
        std::size_t colon = body.find(':', k + needle.size());
        if (colon == std::string::npos) return "";
        std::size_t q = body.find('"', colon);
        if (q == std::string::npos) return "";
        std::string out; bool esc = false;
        for (std::size_t i = q + 1; i < body.size(); ++i)
        {
            char c = body[i];
            if (esc)
            {
                switch (c)
                {
                    case 'n': out += '\n'; break;  case 't': out += ' '; break;
                    case 'r': break;               case 'b': case 'f': out += ' '; break;
                    case 'u':                      // \uXXXX -> UTF-8 (BMP + surrogate pairs)
                    {
                        if (i + 5 > body.size())   // needs 4 hex digits at i+1..i+4
                            break;                 // truncated escape: drop it
                        unsigned cp = (unsigned)std::strtoul(body.substr(i + 1, 4).c_str(), nullptr, 16);
                        i += 4;
                        // UTF-16 surrogate pair: a high half D800-DBFF must merge with the
                        // following \uDC00-DFFF low half into ONE real code point; emitting
                        // each half alone would produce two broken 3-byte sequences.
                        if (cp >= 0xD800 && cp <= 0xDBFF)
                        {
                            unsigned lo = 0;
                            if (i + 6 < body.size() && body[i + 1] == '\\' && body[i + 2] == 'u')
                                lo = (unsigned)std::strtoul(body.substr(i + 3, 4).c_str(), nullptr, 16);
                            if (lo < 0xDC00 || lo > 0xDFFF)
                                break;             // lone high surrogate: not encodable, drop
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            i += 6;                // consume the "\uXXXX" low half too
                        }
                        else if (cp >= 0xDC00 && cp <= 0xDFFF)
                            break;                 // lone low surrogate: not encodable, drop
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
                        else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                        else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: out += c; break;      // \" \\ \/ and anything else: literal
                }
                esc = false;
            }
            else if (c == '\\') esc = true;
            else if (c == '"') break;
            else out += c;
        }
        return out;
    }

    // Find the position of a key (used to scope Anthropic/OpenAI block searches).
    std::size_t JsonFindKey(std::string const& body, std::string const& key, std::size_t from = 0)
    {
        return body.find("\"" + key + "\"", from);
    }

    std::string TrimWs(std::string const& s)
    {
        std::size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return "";
        std::size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    // Remove inline <think>...</think>. If stripping empties the string, return the original (fallback).
    std::string StripThinkSafe(std::string s)
    {
        std::string original = s;
        for (;;)
        {
            std::size_t a = s.find("<think>");
            if (a == std::string::npos) break;
            std::size_t b = s.find("</think>", a);
            if (b == std::string::npos) { s.erase(a); break; }   // open tag, no close: drop tail
            s.erase(a, (b + 8) - a);
        }
        // also tolerate a stray leading "</think>" with no opener
        std::size_t close = s.find("</think>");
        if (close != std::string::npos && s.find("<think>") == std::string::npos)
            s.erase(0, close + 8);

        std::string trimmed = TrimWs(s);
        return trimmed.empty() ? TrimWs(original) : trimmed;   // NEVER drop the reply to empty
    }

    // Truncate an over-long reply at a natural boundary instead of a hard byte
    // slice (which can bisect a multibyte UTF-8 char into "?" and chop mid-word):
    // prefer the last sentence end at/before the cap; if that lands too early
    // (< ~80 bytes) fall back to the last space; never split inside a multibyte
    // sequence. maxBytes stays the hard ceiling.
    // Cut a TRAILING question sentence off a chat line, when a statement survives.
    //
    // Small local models are assistant-tuned and end nearly every line with a
    // question or an offer of help ("...What can I do for you?"), which reads as
    // a chatbot, not an orc. Reported from the field (Boethiah, 2026-07-14) on
    // qwen2.5:3b - "not a single statement made".
    //
    // MEASURED against that exact model before writing this (14 whispers/variant):
    //   * shipped prompt ............................ 6/14 ended in a question
    //   * + "never end with a question" in System ... 6/14  (no effect at all)
    //   * + the same rule at the prompt TAIL ........ 6/14  (no effect)
    //   * + "end with a period, not a question" ..... 0/14  BUT the model then
    //         literally wrote the word "Period." at the end of every line.
    //   * cutting it here, in code .................. 0/14, and what remains
    //         reads naturally ("Well met." / "It's decent enough where I stand.")
    // So: prompt rules do not fix this on small models - code does, and it works
    // on ANY model the player self-hosts.
    //
    // A reply that is a SINGLE question survives untouched, so bots still ask
    // something back now and then; only the tacked-on trailing question goes.
    std::string DropTrailingQuestion(std::string const& in)
    {
        if (!CfgNoTrailQ() || in.empty())
            return in;

        // split into sentences, keeping each terminator with its sentence
        std::vector<std::string> parts;
        std::string cur;
        for (char const c : in)
        {
            cur += c;
            if (c == '.' || c == '!' || c == '?')
            {
                parts.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty())
            parts.push_back(cur);   // tail fragment with no terminator

        auto endsInQuestion = [](std::string const& s)
        {
            for (auto it = s.rbegin(); it != s.rend(); ++it)
            {
                if (std::isspace(static_cast<unsigned char>(*it)))
                    continue;
                return *it == '?';
            }
            return false;
        };

        while (parts.size() > 1 && endsInQuestion(parts.back()))
            parts.pop_back();

        std::string out;
        for (std::string const& p : parts)
            out += p;
        while (!out.empty() && std::isspace(static_cast<unsigned char>(out.back())))
            out.pop_back();

        return out.empty() ? in : out;   // never hand back an empty line
    }

    std::string TrimReplySmart(std::string const& in, std::size_t maxBytes)
    {
        if (in.size() <= maxBytes)
            return in;

        // back the cut off to a UTF-8 sequence START (continuation = 10xxxxxx)
        std::size_t cut = maxBytes;
        while (cut > 0 && (static_cast<unsigned char>(in[cut]) & 0xC0) == 0x80)
            --cut;
        std::string head = in.substr(0, cut);

        std::size_t dot = head.find_last_of(".!?");
        if (dot != std::string::npos && dot >= 80)
            return TrimWs(head.substr(0, dot + 1));   // whole sentences only

        std::size_t sp = head.find_last_of(' ');
        if (sp != std::string::npos && sp > 0)
            return TrimWs(head.substr(0, sp));        // whole words at least

        return head;   // one giant token: hard cut (already UTF-8 safe)
    }

    /* ---------------------------------------------------------------------- */
    /*  Per-provider request building + thinking-safe response parsing         */
    /*  WORKER THREAD ONLY.                                                     */
    /* ---------------------------------------------------------------------- */

    // Pull a small non-negative integer that follows "key": in a JSON body.
    // Good enough for provider usage counters; 0 when absent.
    uint64 JsonExtractUInt(std::string const& body, char const* key)
    {
        std::string const needle = std::string("\"") + key + "\"";
        std::size_t p = body.find(needle);
        if (p == std::string::npos)
            return 0;
        p = body.find(':', p + needle.size());
        if (p == std::string::npos)
            return 0;
        ++p;
        while (p < body.size() && (body[p] == ' ' || body[p] == '\t'))
            ++p;
        uint64 v = 0;
        bool any = false;
        while (p < body.size() && body[p] >= '0' && body[p] <= '9')
        {
            v = v * 10 + uint64(body[p] - '0');
            ++p;
            any = true;
        }
        return any ? v : 0;
    }

    // Record one call's provider-reported usage: global stats buckets + the
    // worker-local counter used for Living Chatter budget attribution.
    void CountCallTokens(uint64 in, uint64 out)
    {
        Stat(&AiStats::promptTokens, in);
        Stat(&AiStats::replyTokens, out);
        t_callTokens += in + out;
    }

    // The directed chat persona/system pair fights the Living Chatter task
    // formats ("reply with one short sentence" vs "write EXACTLY 4 lines"),
    // so each kind frames its call differently. WORKER THREAD ONLY.
    // talk-AND-command: appended to the system prompt when the speaker is
    // the bot's MASTER (rq.orderable). The model answers in character AND
    // flags an executable order in one strict-JSON object - one call does
    // both jobs, so a command costs exactly what a chat reply costs.
    constexpr char const* ORDER_SYSTEM_ADDENDUM =
        "\nIMPORTANT OUTPUT FORMAT: The speaker leads your group. Reply ONLY "
        "with one single-line JSON object, nothing before or after it: "
        "{\"order\":\"<verb>\",\"target\":\"<enemy name or empty>\",\"say\":"
        "\"<what you speak, in character>\"} "
        "When they clearly COMMAND an action, set \"order\" to exactly one "
        "of: \"follow\" (come along with them), \"stay\" (stop, wait, hold "
        "position), \"attack\" (attack their current target), \"tank\" "
        "(tank their current target), \"pull\" (pull their current target - "
        "only if YOU are a tank; otherwise decline in character), "
        "\"flee\" (retreat to them), \"grind\" (kill everything around), "
        "\"summon\" (teleport yourself to them), \"revive\" (you died: go "
        "to a spirit healer), \"release\" (release your spirit), \"home\" "
        "(use your hearthstone), \"drink\" (eat and drink to recover), "
        "\"reset\" (reset your behavior), \"heal\" (prioritize healing "
        "them - only if YOU are a healer; otherwise decline in character), "
        "\"guide\" (they ask you to LEAD or TAKE them somewhere, however "
        "misspelled: ALWAYS set \"order\":\"guide\" and \"target\" to the "
        "place or NPC they named - a town, an innkeeper, their trainer, "
        "their quest. NEVER claim you do not know the way - the world "
        "itself decides that, you just acknowledge with a follow-me line). "
        "\"target\" is for \"attack\" when they NAME a specific enemy (copy "
        "that name from the NEARBY ENEMIES list in your briefing; if the "
        "named enemy is not on that list, set \"order\":\"\" and say you do "
        "not see it) and for \"guide\" (the destination they named). "
        "Otherwise leave \"target\" empty. "
        "A command addressed to the whole party (\"everyone follow\") is "
        "still ONE order - acknowledge for the group in \"say\". "
        "Merely discussing, planning or asking about an action is "
        "NOT a command. For ANYTHING else (questions, banter, stories, "
        "other requests) set \"order\" to \"\". Never use any other order "
        "word. Keep \"say\" short and in the same language they used.";

    std::string SystemFor(uint32 kind, std::string const& cfgSystem, bool orderable = false)
    {
        std::string const language = CfgCopy().koreanOnly ?
            " All spoken text MUST be Korean, including greetings and item/place names. "
            "You are a person playing WoW, not a lore character. Prioritize the actual party/raid/game situation. "
            "Do not use Latin words or abbreviations in spoken text. Keep required JSON keys/order values and A:/B: scene labels unchanged." : "";
        if (kind == AI_KIND_SOCIAL)
            return "Write a brief two-player WoW conversation from verified context, not lore roleplay. "
                "Return exactly A: and B: lines, Korean speech only. Do not invent mechanics or completed actions.";
        if (kind == AI_KIND_AMBIENT || kind == AI_KIND_SCENE)
            return std::string("You write short in-character lines and dialogue for "
                   "World of Warcraft characters. Follow the TASK in the "
                   "message exactly, including any line count and format. "
                   "Plain text only - no emojis, no markdown, no narration.") + language;
        if (orderable)
            return cfgSystem + ORDER_SYSTEM_ADDENDUM + language;
        return cfgSystem + language;
    }

    std::string UserFor(uint32 kind, std::string const& persona,
        std::string const& userMsg, bool orderable = false)
    {
        if (kind == AI_KIND_AMBIENT || kind == AI_KIND_SCENE || kind == AI_KIND_SOCIAL)
            return persona;                      // the prebuilt TASK prompt
        if (kind == AI_KIND_GREETING)
            return persona + "\nTASK: " + userMsg + "\nYour greeting:";
        if (orderable)
            return persona + "\nPlayer says: " + userMsg
                + "\n(The \"say\" field must be in the exact same language "
                  "as that last message. Reply with the single-line JSON "
                  "object ONLY.)";
        return persona + "\nPlayer says: " + userMsg
            + "\n(Reply ONLY in the same language as that last message.)";
    }

    // Scene scripts are 4 lines, not one chat sentence - they need a bigger
    // completion budget than the chat setting.
    uint32 MaxTokensFor(uint32 kind)
    {
        uint32 const configured = CfgMaxTokens();
        if (configured == 0)
            return 0; // provider default/model limit; OpenAI payload omits the field
        return kind == AI_KIND_SCENE ? 320 : configured;
    }

    // Ollama /api/generate (stream=false). Behavior unchanged, now routed through HttpPost.
    std::string OllamaGenerate(std::string const& persona, std::string const& userMsg, std::string const& model, uint32 kind, bool orderable)
    {
        if (model.empty())
        {
            LOG_ERROR("server", "[aichat] Provider=ollama needs WowLegends.AiChat.Model set to a pulled model (e.g. qwen2.5:1.5b) - bots stay silent until then");
            return "";
        }
        std::string host = CfgHost(), port = CfgPort();
        // Language directive placed LAST (highest recency) so small models obey it over
        // the mixed-language conversation history that precedes it.
        std::string prompt;
        if (kind == AI_KIND_DIRECTED && orderable)
            prompt = persona + "\nPlayer whispers: " + userMsg
                + "\n(The \"say\" field must be in the exact same language as that last whisper, ignoring earlier languages.)\nYour single-line JSON reply:";
        else if (kind == AI_KIND_DIRECTED)
            prompt = persona + "\nPlayer whispers: " + userMsg
                + "\n(Reply ONLY in the exact same language as that last whisper, ignoring earlier languages.)\nYour reply:";
        else if (kind == AI_KIND_GREETING)
            prompt = persona + "\nTASK: " + userMsg + "\nYour greeting:";
        else
            prompt = persona;                    // the prebuilt TASK prompt
        // completion budget follows MaxTokens (clamped: chat lines, not essays);
        // repeat_penalty curbs the loop-happy small local models.
        uint32 numPredict = MaxTokensFor(kind);
        if (numPredict < 16)  numPredict = 16;
        if (numPredict > 384) numPredict = 384;
        std::string payload = "{\"model\":\"" + JsonEscape(model)
            // Ollama native structured output when an order may be flagged -
            // constrains the token stream to valid JSON (small local models
            // follow prompt-only JSON contracts unreliably)
            + std::string(orderable ? "\",\"format\":\"json" : "")
            + "\",\"stream\":false,\"think\":false,\"system\":\"" + JsonEscape(SystemFor(kind, CfgSystem(), orderable))
            + "\",\"prompt\":\"" + JsonEscape(prompt)
            + "\",\"options\":{\"num_predict\":" + std::to_string(numPredict)
            + ",\"temperature\":" + std::to_string(CfgTemp())
            + ",\"repeat_penalty\":1.15}}";

        std::string body = HttpPost(/*https=*/false, host, port, "/api/generate", {}, payload);
        if (body.empty()) return "";
        CountCallTokens(JsonExtractUInt(body, "prompt_eval_count"),
                        JsonExtractUInt(body, "eval_count"));

        std::string const reply = StripThinkSafe(JsonExtractString(body, "response"));

        // Ollama answered, but with no text: practically always an error object
        // ({"error":"model 'x' not found"}, a load that ran out of RAM, a bad
        // option) - which carries no "response" key, so the reply comes out
        // empty and the bot simply goes quiet. Reported from the field
        // (Boethiah, 2026-07-14): the only trace was an empty worker reply,
        // which is unactionable. Surface the reason verbatim - silent bots are
        // THE most common self-hosting support case.
        if (reply.empty())
        {
            std::string const err = JsonExtractString(body, "error");
            char const* hint = "";
            if (err.find("not found") != std::string::npos)
                hint = " | pull it first (`ollama pull <model>`) and make Model match `ollama list` exactly";
            else if (err.find("memory") != std::string::npos)
                hint = " | not enough free RAM to load it - try a smaller model (e.g. qwen2.5:3b)";

            LOG_ERROR("server", "[aichat] ollama model '{}' returned no text: {}{}", model,
                      err.empty() ? body.substr(0, 200) : err, hint);
        }
        return reply;   // Ollama "response" field
    }

    // OpenAI-compatible /chat/completions (OpenAI / DeepSeek / Kimi/Moonshot).
    std::string OpenAiGenerate(std::string const& persona,
                               std::string const& userMsg, std::string const& model, uint32 kind, bool orderable)
    {
        std::string url = CfgApiUrl();
        if (url.empty()) { LOG_ERROR("server", "[aichat] openai provider: ApiUrl empty"); return ""; }
        bool https; std::string host, port, path;
        if (!ParseUrl(url, https, host, port, path)) { LOG_ERROR("server", "[aichat] bad ApiUrl '{}'", url); return ""; }

        std::string userContent = UserFor(kind, persona, userMsg, orderable);

        // No model configured -> omit the field so the endpoint's default applies
        // (the hosted wow-legends.eu proxy enforces its own model server-side anyway).
        std::string modelField = model.empty() ? "" : "\"model\":\"" + JsonEscape(model) + "\",";
        // mild anti-repetition nudge; harmless on OpenAI-compatible proxies
        // MaxTokens=0 means that this client does not impose a completion
        // limit.  OpenAI-compatible endpoints then apply only their model or
        // service-side context limit.  This is intentionally different from
        // sending max_tokens=0, which many providers reject or interpret as
        // an empty completion.
        uint32 const maxTokens = MaxTokensFor(kind);
        std::string const maxTokensField = maxTokens == 0 ? "" :
            "\"max_tokens\":" + std::to_string(maxTokens) + ",";
        std::string payload = "{" + modelField + maxTokensField +
            "\"temperature\":" + std::to_string(CfgTemp()) + ","
            "\"frequency_penalty\":0.4,"
            "\"presence_penalty\":0.3,"
            "\"stream\":false,"
            "\"messages\":["
                "{\"role\":\"system\",\"content\":\"" + JsonEscape(SystemFor(kind, CfgSystem(), orderable)) + "\"},"
                "{\"role\":\"user\",\"content\":\"" + JsonEscape(userContent) + "\"}"
            "]}";

        std::vector<std::string> headers = { "Authorization: Bearer " + CfgApiKey() };
        std::string body = HttpPost(https, host, port, path, headers, payload);
        if (body.empty()) return "";
        CountCallTokens(JsonExtractUInt(body, "prompt_tokens"),
                        JsonExtractUInt(body, "completion_tokens"));

        // Anchor on the message object so reasoning_content is never picked up.
        std::size_t msgPos = JsonFindKey(body, "message");
        std::string content = JsonExtractString(body, "content", msgPos == std::string::npos ? 0 : msgPos);
        if (content.empty()) content = JsonExtractString(body, "content"); // fallback: first content anywhere
        if (content.empty())
        {
            LOG_ERROR("server", "[aichat] openai: no content; body head '{}'", body.substr(0, 200));
            return "";
        }
        return StripThinkSafe(content);
    }

    // Anthropic /v1/messages. System is top-level; max_tokens mandatory; answer is the text block.
    std::string AnthropicGenerate(std::string const& persona,
                                  std::string const& userMsg, std::string const& model, uint32 kind, bool orderable)
    {
        std::string url = CfgApiUrl();
        if (url.empty()) url = "https://api.anthropic.com/v1/messages";
        bool https; std::string host, port, path;
        if (!ParseUrl(url, https, host, port, path)) { LOG_ERROR("server", "[aichat] bad ApiUrl '{}'", url); return ""; }

        std::string userContent = UserFor(kind, persona, userMsg, orderable);

        std::string payload = "{\"model\":\"" + JsonEscape(model) + "\","
            "\"max_tokens\":" + std::to_string(MaxTokensFor(kind)) + ","
            "\"system\":\"" + JsonEscape(SystemFor(kind, CfgSystem(), orderable)) + "\","
            "\"messages\":[{\"role\":\"user\",\"content\":\"" + JsonEscape(userContent) + "\"}]}";

        std::vector<std::string> headers = {
            "x-api-key: " + CfgApiKey(),
            "anthropic-version: 2023-06-01"
        };
        std::string body = HttpPost(https, host, port, path, headers, payload);
        if (body.empty()) return "";
        CountCallTokens(JsonExtractUInt(body, "input_tokens"),
                        JsonExtractUInt(body, "output_tokens"));

        // content is an array of typed blocks. Take the FIRST {"type":"text", ... "text":"..."}.
        // Iterate "type" keys; when one equals "text", read the "text" value after it.
        std::size_t scan = 0;
        std::string answer;
        while (true)
        {
            std::size_t tpos = JsonFindKey(body, "type", scan);
            if (tpos == std::string::npos) break;
            std::string tval = JsonExtractString(body, "type", tpos);   // value of THIS "type"
            std::size_t afterType = body.find(':', tpos);
            scan = (afterType == std::string::npos) ? tpos + 5 : afterType + 1;
            if (tval == "text")
            {
                answer = JsonExtractString(body, "text", scan);
                if (!answer.empty()) break;
            }
            // tval == "thinking" / "redacted_thinking" -> skip; do NOT read its "thinking" field
        }
        if (answer.empty()) answer = JsonExtractString(body, "text");   // last-ditch fallback
        if (answer.empty())
        {
            LOG_ERROR("server", "[aichat] anthropic: no text block; body head '{}'", body.substr(0, 200));
            return "";
        }
        return StripThinkSafe(answer);
    }

    // Worker-thread entry: build request + parse reply for the configured provider.
    // Returns the FINAL answer (thinking already stripped) or "" only on genuine transport/parse failure.
    std::string ProviderGenerate(std::string const& persona,
                                 std::string const& userMsg, std::string const& model, uint32 kind,
                                 bool orderable)
    {
        std::string provider = CfgProvider();
        if (provider == "openai" || provider == "deepseek" || provider == "kimi" || provider == "moonshot")
            return OpenAiGenerate(persona, userMsg, model, kind, orderable);
        if (provider == "anthropic" || provider == "claude")
            return AnthropicGenerate(persona, userMsg, model, kind, orderable);
        return OllamaGenerate(persona, userMsg, model, kind, orderable);   // default + unknown -> local Ollama
    }

    // talk-AND-command: pull {"order":"..","say":".."} out of an orderable
    // reply (worker thread, pure string work). STRICT parse: the envelope
    // must be a flat {"key":"value",...} object and keys are only accepted
    // in KEY POSITION - a say value containing the word "order" can never
    // resurrect an order the model declined. Fail rules:
    //   - genuine prose (text before any '{'): returned untouched as chat.
    //   - an ATTEMPTED envelope that will not parse: a canned nudge, never
    //     the raw JSON blob (it would reach chat + persisted history).
    //   - an order ships ONLY from a clean parse + the follow|stay whitelist.
    std::string ExtractOrderJson(std::string const& reply, std::string& order,
        std::string& target, bool& parseMiss)
    {
        order.clear();
        target.clear();
        parseMiss = false;

        // whitespace or a markdown fence (``` / ```json) before the brace is
        // an envelope; anything else is a PROSE prefix. Models sometimes
        // narrate before complying, so a prose prefix no longer bails out -
        // the walk below runs anyway and the envelope wins IF it yields real
        // content (otherwise the prose ships untouched as chat).
        std::size_t brace = reply.find('{');
        if (brace == std::string::npos)
            return reply;
        bool prosePrefix = false;
        for (std::size_t k = 0; k < brace && !prosePrefix; ++k)
        {
            char c = reply[k];
            if (!std::isspace(static_cast<unsigned char>(c)) && c != '`'
                && c != 'j' && c != 's' && c != 'o' && c != 'n')
                prosePrefix = true;
        }

        std::string verb, say;
        bool wellFormed = false, sawKey = false;
        std::size_t i = brace + 1;
        auto skipWs = [&reply, &i]()
        {
            while (i < reply.size()
                && std::isspace(static_cast<unsigned char>(reply[i])))
                ++i;
        };
        while (true)
        {
            skipWs();
            if (i >= reply.size() || reply[i] != '"')
                break;
            std::size_t keyPos = i;
            std::size_t keyStart = ++i;
            while (i < reply.size() && reply[i] != '"')
                ++i;
            if (i >= reply.size())
                break;
            std::string key = reply.substr(keyStart, i - keyStart);
            ++i;
            skipWs();
            if (i >= reply.size() || reply[i] != ':')
                break;
            ++i;
            skipWs();
            if (i >= reply.size() || reply[i] != '"')
                break;
            ++i;                                       // into the value
            while (i < reply.size() && reply[i] != '"')
                i += (reply[i] == '\\' && i + 1 < reply.size()) ? 2 : 1;
            if (i >= reply.size())
                break;
            ++i;                                       // past closing quote
            sawKey = true;
            if (key == "order")
                verb = JsonExtractString(reply, "order", keyPos);
            else if (key == "target")
                target = JsonExtractString(reply, "target", keyPos);
            else if (key == "say")
                say = JsonExtractString(reply, "say", keyPos);
            skipWs();
            if (i < reply.size() && reply[i] == ',')
            {
                ++i;
                continue;
            }
            wellFormed = i < reply.size() && reply[i] == '}';
            break;
        }

        if (!wellFormed || !sawKey)
        {
            // genuine prose that merely contains a '{' ships as chat; an
            // ATTEMPTED envelope that will not parse never ships the blob
            if (prosePrefix)
                return reply;
            parseMiss = true;
            return "Hm? Say that again.";
        }

        for (char& c : verb)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        verb = TrimWs(verb);
        // whitelist = the injection boundary: only verbs we vouch for ship.
        // attack/tank/pull act on the MASTER's current selection at
        // execution time; mod-playerbots itself refuses no-target ("You
        // have no target"), friendly and invalid targets
        // (AttackAction.cpp:103/:159).
        if (FindOrderVerb(verb))
            order = verb;

        // "target" only means something for a named attack (matched against
        // the LIVE grid at dispatch) or a guide destination (matched against
        // static stores at dispatch); never echoed back, so a bogus value
        // can only ever produce an honest refusal.
        target = TrimWs(target);
        if ((order != "attack" && order != "guide") || target.size() > 60)
            target.clear();

        // the \uXXXX decode above can re-materialize emoji that the provider
        // parser's earlier pass already filtered - filter the say AGAIN
        say = TrimWs(StripUnsupported(say));
        if (say.empty())
        {
            if (!order.empty())
                return OrderAck(order);
            // a flat-but-foreign JSON object inside real prose ({"foo":..})
            // parses "well-formed" yet yields neither order nor say: prose
            if (prosePrefix)
                return reply;
            parseMiss = true;
            return "Hm? Say that again.";
        }
        return say;
    }

    void WorkerLoop()
    {
        while (g_run)
        {
            AiRequest rq;
            {
                std::unique_lock<std::mutex> lk(g_inMtx);
                g_cv.wait(lk, [] { return !g_run || !g_in.empty(); });
                if (!g_run) return;
                rq = g_in.front(); g_in.pop_front();
            }
            t_callTokens = 0;
            std::string reply = StripUnsupported(ProviderGenerate(rq.persona, rq.message, rq.model, rq.kind, rq.orderable));
            if (rq.kind != AI_KIND_DIRECTED)
                g_livingTokensToday += t_callTokens;
            LOG_INFO("server", "[aichat] worker reply provider={} kind={} len={} '{}'",
                     CfgProvider(), rq.kind, (int)reply.size(), reply.substr(0, 60));
            if (reply.empty())
                Stat(&AiStats::failures);
            else
            {
                Stat(&AiStats::replies);
                // orderable replies carry {"order","say"} - split them apart
                // BEFORE trimming (a trim could cut the JSON mid-object)
                std::string order;
                std::string orderTarget;
                if (rq.orderable)
                {
                    bool parseMiss = false;
                    reply = ExtractOrderJson(reply, order, orderTarget, parseMiss);
                    if (parseMiss)
                        Stat(&AiStats::orderParseMiss);
                }
                // scenes carry a whole multi-line dialogue; chat lines stay short
                reply = TrimReplySmart(reply,
                    (rq.kind == AI_KIND_SCENE || rq.kind == AI_KIND_SOCIAL) ? 700 : 230);
                // kill the assistant tic on every conversational line. NOT on a
                // SCENE: that is a scripted multi-line exchange between two bots
                // where a closing question is a legitimate cue for the next line.
                if (rq.kind != AI_KIND_SCENE && rq.kind != AI_KIND_SOCIAL)
                    reply = DropTrailingQuestion(reply);
                if ((CfgCopy().koreanOnly || rq.kind == AI_KIND_SOCIAL) &&
                    WowLegends::HasLatinSpeech(reply, rq.kind == AI_KIND_SCENE || rq.kind == AI_KIND_SOCIAL))
                {
                    LOG_WARN("server", "[aichat] Korean-only guard rejected non-Korean speech kind={}", rq.kind);
                    if (rq.kind == AI_KIND_AMBIENT || rq.kind == AI_KIND_SCENE ||
                        rq.kind == AI_KIND_GREETING || rq.kind == AI_KIND_SOCIAL)
                        continue; // Never retry paid requests or flood chat with fallback greetings.
                    reply = "답변을 한국어로 정리하지 못했어요. 다시 한번 말씀해 주세요.";
                }
                std::lock_guard<std::mutex> lk(g_outMtx);
                g_out.push_back({ rq.bot, rq.player, rq.botName, reply, rq.chatType, rq.kind, order,
                    rq.partyWide, orderTarget, rq.channelId, rq.channelName });
            }
        }
    }

    // Worker POOL: several provider round-trips in flight at once, so a slow
    // call delays only its own bot instead of every reply on the realm. The
    // handles are kept so OnShutdown can stop + join them cleanly.
    void EnsureWorkers()
    {
        if (g_run.exchange(true)) return;     // already running
        uint32 const n = CfgWorkers();
        for (uint32 i = 0; i < n; ++i)
            g_workers.emplace_back(WorkerLoop);
        g_workerCount = n;
        LOG_INFO("server", "[aichat] {} worker thread(s) started", n);
    }

    bool WlIsRealPlayer(Player* p) { return p && p->GetSession() && !p->GetSession()->IsBot(); }
    bool IsBotPlayer(Player* p)  { return p && p->GetSession() &&  p->GetSession()->IsBot(); }

    // Update the (bot,speaker) anger meter from one message; return the matching mood line.
    std::string BumpMood(uint64 ak, std::string const& msg)
    {
        int anger = g_anger[ak];
        if (LooksLikeInsult(msg)) anger += 2; else if (anger > 0) anger -= 1;
        if (anger < 0) anger = 0;
        if (anger > 6) anger = 6;
        g_anger[ak] = anger;
        if (anger >= static_cast<int>(CfgLeaveAnger()))
            return " You are FURIOUS with this player and want nothing more to do with them.";
        if (anger >= 2)
            return " You are irritated and short-tempered with this player.";
        return "";
    }

    // Lowercase + strip ASCII punctuation/whitespace so two near-identical
    // replies compare equal. Bytes >= 0x80 (multibyte UTF-8) pass through
    // unchanged, so non-English text never normalizes to empty.
    std::string NormalizeReply(std::string const& s)
    {
        std::string out;
        out.reserve(s.size());
        for (char ch : s)
        {
            unsigned char c = static_cast<unsigned char>(ch);
            if (c >= 0x80)
                out += ch;
            else if (std::isalnum(c))
                out += static_cast<char>(std::tolower(c));
        }
        return out;
    }

    // Near-identical: equal normalized, or one contains the other with
    // lengths within 20% of each other.
    bool IsNearDuplicate(std::string const& a, std::string const& b)
    {
        if (a.empty() || b.empty())
            return false;
        if (a == b)
            return true;
        std::string const& big = a.size() >= b.size() ? a : b;
        std::string const& sml = a.size() >= b.size() ? b : a;
        if (sml.size() * 5 < big.size() * 4)   // > 20% length gap: different
            return false;
        return big.find(sml) != std::string::npos;
    }

    // True when `reply` near-repeats one of the bot's LAST TWO lines to this
    // player (small models love to loop). Reads g_history: MAIN THREAD ONLY.
    bool IsRepeatReply(uint64 ak, std::string const& reply)
    {
        auto hit = g_history.find(ak);
        if (hit == g_history.end())
            return false;
        std::string norm = NormalizeReply(reply);
        if (norm.empty())
            return false;
        int seen = 0;
        for (auto it = hit->second.rbegin(); it != hit->second.rend() && seen < 2; ++it)
        {
            if (it->rfind("Player: ", 0) == 0)
                continue;                        // player line, not a bot line
            ++seen;
            std::size_t sep = it->find(": ");    // "<Bot>: text" -> text
            std::string prev = (sep == std::string::npos) ? *it : it->substr(sep + 2);
            if (IsNearDuplicate(norm, NormalizeReply(prev)))
                return true;
        }
        return false;
    }

    // True when lowercased `name` occurs in lowercased `low` as a WHOLE word
    // (delimited by non-alphanumerics or the string edges), so a bot named
    // "Al" no longer answers to "also".
    bool ContainsWholeName(std::string const& low, std::string const& name)
    {
        if (name.empty())
            return false;
        std::size_t pos = 0;
        while ((pos = low.find(name, pos)) != std::string::npos)
        {
            bool left  = pos == 0
                || !std::isalnum(static_cast<unsigned char>(low[pos - 1]));
            std::size_t end = pos + name.size();
            bool right = end >= low.size()
                || !std::isalnum(static_cast<unsigned char>(low[end]));
            if (left && right)
                return true;
            ++pos;
        }
        return false;
    }

    // First bot whose name is mentioned (as a whole word) in the message, else nullptr.
    Player* FindNamedBot(std::vector<Player*> const& bots, std::string const& msg)
    {
        std::string low = ToLower(msg);
        for (Player* b : bots)
            if (ContainsWholeName(low, ToLower(b->GetName())))
                return b;
        return nullptr;
    }

    // Build the grounded persona for `bot`, record the player line, and queue the LLM call.
    // chatType decides how the reply is delivered later. Returns false on cooldown/full queue.
    // botInitiates: the BOT speaks first (recognition greeting) - `msg` is then an
    // instruction, not a player line, and is never recorded in the history.
    // MAIN THREAD ONLY.
    bool EnqueueAi(Player* speaker, Player* bot, std::string const& msg, uint32 chatType, std::string const& mood, bool directed, bool botInitiates = false, bool orderable = false, bool partyWide = false, uint32 channelId = 0, std::string const& channelName = "")
    {
        uint32 botLow = bot->GetGUID().GetCounter();
        uint64 ak = AngerKey(botLow, speaker->GetGUID().GetCounter());

        // hydrate persisted history + anger before the persona reads g_history below
        // (no-op if a chat hook already loaded this pair earlier this session).
        LoadMemory(botLow, speaker->GetGUID().GetCounter());

        // cooldown is per (bot,player) so a second player talking to a popular
        // bot isn't silently dropped; the small per-bot outbound floor keeps
        // one bot from machine-gunning many players in the same second.
        // Orderable requests skip THIS pair-cooldown - the hook already
        // applied the 2s order throttle, and a chat cooldown must never
        // swallow an order into total silence.
        time_t now = time(nullptr);
        auto it = g_cooldown.find(ak);
        if (!orderable && it != g_cooldown.end() && it->second > now)
        {
            LOG_INFO("server", "[aichat] {} on cooldown for {} ({}s left) - dropping message",
                     bot->GetName(), speaker->GetName(), (long)(it->second - now));
            return false;
        }
        auto fit = g_botFloor.find(botLow);
        if (fit != g_botFloor.end() && fit->second > now)
        {
            LOG_INFO("server", "[aichat] {} hit per-bot send floor - dropping message", bot->GetName());
            return false;
        }

        std::string sex  = bot->getGender() == 0 ? "male" : "female";
        std::string side = bot->GetTeamId() == TEAM_HORDE ? "the Horde" : "the Alliance";
        LocaleConstant const contextLocale = CfgCopy().koreanOnly ? LOCALE_koKR : LOCALE_enUS;
        std::string where = contextLocale == LOCALE_koKR ? "지역명 미확인" : "an unknown area";
        if (AreaTableEntry const* z = sAreaTableStore.LookupEntry(bot->GetZoneId()))
            if (z->area_name[contextLocale] && *z->area_name[contextLocale])
                where = z->area_name[contextLocale];
        uint32 areaId = bot->GetAreaId();
        if (areaId && areaId != bot->GetZoneId())
            if (AreaTableEntry const* a = sAreaTableStore.LookupEntry(areaId))
                if (a->area_name[contextLocale] && *a->area_name[contextLocale] && where != a->area_name[contextLocale])
                    where += std::string(" (") + a->area_name[contextLocale] + ")";

        std::string spec = SpecName(bot);
        std::string klass = ClassName(bot->getClass());
        std::string classPhrase = spec.empty() ? klass : (spec + " " + klass);

        bool const playerStyle = sConfigMgr->GetOption<bool>("WowLegends.AiChat.PlayerStyle", false);
        std::string persona = (playerStyle ? "You are a WoW player controlling the character " : "You are ") + bot->GetName() + ", a " + sex + " "
                            + RaceName(bot->getRace()) + " " + classPhrase
                            + ", level " + std::to_string(bot->GetLevel()) + ", of " + side
                            + ". You are currently in " + where + ".";
        std::string weapon = MainHandName(bot);
        if (!weapon.empty())
            persona += " You wield " + weapon + " as your weapon.";
        persona += " These facts about you are TRUE - never contradict them or invent a different name, race, class, spec, weapon, faction, or location.";

        // --- per-race + per-faction flavour: ONLY this bot's own race + side (token-cheap) ---
        if (!playerStyle)
        {
            char const* rkey; char const* rdef;
            RaceFlavor(bot->getRace(), rkey, rdef);
            if (rkey[0])
            {
                std::string rflav = CfgRace(rkey, rdef);
                if (!rflav.empty())
                    persona += " " + rflav;
            }
            persona += " ";
            persona += (bot->GetTeamId() == TEAM_HORDE) ? CfgFactionHorde() : CfgFactionAlliance();
        }

        // this bot's OWN deterministic character on top of race/faction, so
        // two orcs sound like different people (stateless, from the GUID)
        if (!playerStyle)
            persona += WlVoiceCardPersona(botLow);
        else
        {
            // Stable player-like voices; never import lore/roleplay identities.
            static char const* const playerVoices[] = {
                " 말투: 짧고 침착한 실전형. 필요한 정보를 먼저 말한다.",
                " 말투: 친절한 협력형. 상대를 탓하지 않고 구체적으로 돕는다.",
                " 말투: 신중한 확인형. 모르는 전술은 추측하지 않고 확인한다.",
                " 말투: 간결한 분석형. 지금 확인된 상황과 다음 할 일을 구분한다.",
                " 말투: 편안한 동료형. 자연스럽고 짧게 대답한다.",
                " 말투: 차분한 지원형. 위험할 때 핵심부터 전달한다.",
                " 말투: 담백한 숙련자형. 과장하거나 훈계하지 않는다.",
                " 말투: 적극적인 협동형. 실행 가능한 제안을 하나씩 말한다."
            };
            persona += playerVoices[botLow % 8];
            uint32 jokeChance = sConfigMgr->GetOption<uint32>("WowLegends.AiChat.JokeChance", 10);
            if (jokeChance > 100) jokeChance = 100;
            bool allowJoke = !bot->IsInCombat() && urand(1, 100) <= jokeChance;
            persona += allowJoke
                ? " Speak as a present-day WoW player, not a lore character. A brief relevant joke is allowed only if it does not distract from the request."
                : " Speak as a present-day WoW player, not a lore character. No jokes this reply. Focus on the user's request and known game situation.";
            persona += " Do not invent raid mechanics, party instructions, cooldowns or readiness not provided in context.";
        }

        // who the bot is talking to — captured NOW on the main thread (the
        // worker must never touch the Player*), same name helpers as above
        persona += " You are talking to " + speaker->GetName() + ", a level "
            + std::to_string(speaker->GetLevel()) + " "
            + RaceName(speaker->getRace()) + " " + ClassName(speaker->getClass());
        if (bot->GetGroup() && bot->GetGroup() == speaker->GetGroup())
            persona += ", who is in your group";
        persona += ".";

        if (Group* group = bot->GetGroup())
        {
            persona += group->isRaidGroup() ? " You are in a RAID." : " You are in a PARTY.";
            persona += PlayerbotAI::IsTank(bot) ? " Your role is tank."
                : PlayerbotAI::IsHeal(bot) ? " Your role is healer." : " Your role is damage dealer.";
            if (group->GetLeaderGUID() == bot->GetGUID())
                persona += " You are the actual group leader. Give concise, situational guidance from supplied facts.";
            else
                persona += " You are not the group leader. Do not impersonate or contradict the leader.";
            persona += " Never claim an action succeeded merely because you said it."
                " Chat alone does not execute a spell, movement, taunt or raid mechanic.";
        }

        // live situation: soft hints only; skipped entirely when neither applies
        if (bot->IsInCombat())
        {
            if (Unit* victim = bot->GetVictim())
                persona += " You are currently fighting " + victim->GetName() + ".";
            else
                persona += " You are currently in combat.";
        }
        if (bot->IsAlive() && bot->GetHealthPct() < 40.0f)
            persona += " You are badly wounded.";

        // tell the bot WHERE this is being said so it answers in the right register
        if (chatType == CHAT_MSG_GUILD)
            persona += " " + speaker->GetName() + " just said this in GUILD chat; give a short reply to the guild.";
        else if (chatType == CHAT_MSG_SAY)
            persona += " You OVERHEARD " + speaker->GetName() + " say this out loud nearby; answer briefly out loud, only if it's worth a word.";
        else if (chatType == CHAT_MSG_CHANNEL)
            persona += " " + speaker->GetName() + " just spoke in a public chat channel, possibly General /1; reply in that same channel.";
        else if (chatType == CHAT_MSG_RAID || chatType == CHAT_MSG_RAID_LEADER || chatType == CHAT_MSG_RAID_WARNING)
            persona += " " + speaker->GetName() + " just spoke in RAID chat; answer briefly for the current raid situation.";
        else if (chatType != CHAT_MSG_WHISPER)
            persona += " Your group member " + speaker->GetName() + " just said this in PARTY chat; give a short reply to the party.";

        if (!playerStyle)
            persona += mood;   // roleplay temper is excluded from player-style replies

        // conversation memory: prior turns with THIS player
        {
            auto hit = g_history.find(ak);
            if (hit != g_history.end() && !hit->second.empty())
            {
                persona += "\nYour recent conversation with this player (oldest first):";
                for (std::string const& line : hit->second)
                    persona += "\n" + line;
            }
        }

        // WOW Legends: if this bot is the speaker's own Companion, weave the
        // adventures they have shared into the persona so it can retell them.
        persona += WlCompanionMemoryNarrative(botLow, speaker->GetGUID().GetCounter());

        // WOW Legends: weave in the bot's social memory of this player (how
        // often they met, where, grudges/friendship) so every conversation is
        // a continuation, not a first encounter.
        persona += WlBotSocialNarrative(botLow, speaker->GetGUID().GetCounter());

        // WOW Legends "The Sage": ground question-shaped DIRECTED messages
        // in server data (items/quests/NPCs) so answers are true on THIS
        // server - for every provider, a local Ollama included
        if (directed && !botInitiates)
            persona += WlBuildSageFacts(speaker, msg);

        // T&C v2: give an orderable call the live target names so a spoken
        // "attack the harpy" can come back as a grounded "target" value, and
        // tell it when the whole party is being addressed.
        if (orderable)
        {
            std::string const table = BuildTargetTable(speaker);
            if (!table.empty())
                persona += "\nNEARBY ENEMIES (the only valid attack targets): " + table + ".";
            if (partyWide)
                persona += "\n" + speaker->GetName() + " is addressing ALL their party bots at once; you speak for the group.";
        }

        if (CfgCopy().koreanOnly)
            persona += "\nReply only in Korean, regardless of the player's language or conversation history.";
        else if (botInitiates)
            persona += "\nMatch the language of your recent conversation with "
                "this player if there is one; otherwise use English.";
        else
            persona += "\nAlways write your reply in the SAME language the player used in their latest message.";

        uint32 const kind = botInitiates ? AI_KIND_GREETING : AI_KIND_DIRECTED;
        std::string model = directed ? CfgModel() : CfgModelFast();   // tiered routing
        {
            std::lock_guard<std::mutex> lk(g_inMtx);
            // greetings respect the Living Chatter headroom cap; the last
            // slots always stay free for player-driven chat.
            std::size_t const cap = botInitiates ? 40 : 50;
            if (g_in.size() >= cap)
            {
                Stat(&AiStats::queueDrops);
                return false;   // caller may fall back (canned greeting)
            }
            g_in.push_back({ bot->GetGUID(), speaker->GetGUID(), bot->GetName(), persona, msg, chatType, model, kind, orderable, partyWide, channelId, channelName });
            Stat(&AiStats::requested);
            if (kind != AI_KIND_DIRECTED)
                ++g_livingReqToday;
        }
        g_cv.notify_one();

        // Side effects only AFTER the request is truly queued. A greeting
        // does NOT stamp the pair cooldown - the player must be able to
        // whisper back immediately.
        if (!botInitiates)
        {
            g_cooldown[ak] = now + static_cast<time_t>(CfgCooldown());
            auto& dq = g_history[ak];
            std::string pline = std::string("Player: ") + msg;
            for (char& c : pline) if (c == '\n' || c == '\r') c = ' ';   // keep one turn = one row
            dq.push_back(pline);
            TrimHistory(dq);
        }
        g_botFloor[botLow] = now + 2;
        return true;
    }
}

/* -------------------------------------------------------------------------- */
/*  Living Chatter entry points (called from other WL modules, world thread)   */
/* -------------------------------------------------------------------------- */

// Queue a raw Living Chatter generation (ambient line or two-bot scene). The
// caller prebuilds the whole prompt; botB is Empty for ambient lines. Returns
// false when AI chat is off, the daily budget is spent, or the queue is busy.
bool WlAiLivingGenerate(uint32 kind, ObjectGuid botA, ObjectGuid botB,
    std::string const& botAName, std::string const& prompt)
{
    if (!CfgEnabled())
        return false;
    if (g_livingReqToday.load() >= CfgLivingBudget())
        return false;
    {
        std::lock_guard<std::mutex> lk(g_inMtx);
        if (g_in.size() >= 40)      // leave headroom for player whispers
            return false;
        g_in.push_back({ botA, botB, botAName, prompt,
            "Follow the task in the context above exactly.",
            CHAT_MSG_SAY, CfgModelFast(), kind });
        Stat(&AiStats::requested);
        ++g_livingReqToday;
    }
    g_cv.notify_one();
    return true;
}

// T&C v2: the group-addressed order surface, shared by PARTY/RAID chat and
// Social requests share the existing daily budget/worker queue, with unique
// correlation IDs so late HTTP replies cannot attach to a newer conversation.
bool WlAiSocialGenerate(uint32 requestId, ObjectGuid botA, ObjectGuid botB,
    std::string const& botName, std::string const& prompt)
{
    if (!CfgEnabled() || g_livingReqToday.load() >= CfgLivingBudget())
        return false;
    {
        std::lock_guard<std::mutex> lock(g_inMtx);
        if (g_in.size() >= 40)
            return false;
        g_in.push_back({botA, botB, botName, prompt, "", CHAT_MSG_SAY, CfgModelFast(),
            AI_KIND_SOCIAL, false, false, requestId, ""});
        ++g_livingReqToday;
        Stat(&AiStats::requested);
    }
    g_cv.notify_one();
    return true;
}

// T&C v2: the group-addressed order surface, shared by PARTY/RAID chat and
// local /say|/yell - players type "everyone follow me" into whatever channel
// their chat box last used, so the spoken channels must obey too.
// `ownedBots` = the speaker's own grouped bots that can hear this message.
// Returns true when the message was CONSUMED as an order (ack queued or one
// orderable AI call enqueued); false = let normal chat handling continue.
bool TryGroupOrder(Player* player, std::vector<Player*> const& ownedBots,
    std::string const& msg, uint32 ackChatType)
{
    if (ownedBots.empty())
        return false;
    if (!CfgAiCommandEnabled() || !CfgPartyOrders()
        || sPlayerbotAIConfig.commandPrefix.empty())
        return false;

    std::string norm = NormalizeOrderText(msg);
    int const address = StripGroupAddress(norm);
    if (address == 0)
        return false;

    // the group answers with ONE stable voice: the first owned bot (keeps
    // its history/persona coherent across orders)
    Player* speakerBot = ownedBots[0];
    std::string verb = MatchOrderIntentNorm(norm);
    std::string targetName = verb.empty() ? ParseAttackTargetName(norm) : "";
    if (!targetName.empty() && !AttackNameResolvesNearby(player, targetName))
        targetName.clear();
    if (!targetName.empty())
        verb = "attack";

    // "everyone take me to X" reads odd but people will say it: ONE bot
    // leads (guide is never a squad-wide fan-out)
    if (verb.empty() && WlBotGuideEnabled())
    {
        std::string const guideTo = ParseGuideTargetName(norm);
        if (!guideTo.empty())
        {
            verb = "guide";
            targetName = guideTo;
        }
    }

    uint64 ck = AngerKey(0, player->GetGUID().GetCounter());
    time_t nowT = time(nullptr);
    if (!verb.empty())
    {
        // exact verb / resolved name: free, so never throttled - a panic
        // "everyone stay" must always land
        g_orderCooldown[ck] = nowT + 2;
        std::string ack = targetName.empty() || verb == "guide" ? OrderAck(verb)
            : "On it - going for the " + targetName + ".";
        std::lock_guard<std::mutex> lk(g_outMtx);
        g_out.push_back({ speakerBot->GetGUID(), player->GetGUID(),
            speakerBot->GetName(), ack, ackChatType,
            AI_KIND_ORDER, verb, verb != "guide", targetName });
        return true;
    }

    // free-form ("everyone wait at the bridge") costs ONE AI call: only for
    // an UNAMBIGUOUS group address ("everyone", "bots", ...), 2s-throttled.
    // Weak addresses ("all right guys...") and throttled repeats fall
    // through to the normal chat tiers instead of being consumed.
    auto cool = g_orderCooldown.find(ck);
    if (address != 2 || (cool != g_orderCooldown.end() && nowT < cool->second))
        return false;

    g_orderCooldown[ck] = nowT + 2;
    uint64 sak = AngerKey(speakerBot->GetGUID().GetCounter(),
                          player->GetGUID().GetCounter());
    LoadMemory(speakerBot->GetGUID().GetCounter(),
               player->GetGUID().GetCounter());
    if (!EnqueueAi(player, speakerBot, msg, ackChatType, BumpMood(sak, msg),
            /*directed=*/true, /*botInitiates=*/false,
            /*orderable=*/true, /*partyWide=*/true))
    {
        std::lock_guard<std::mutex> lk(g_outMtx);
        g_out.push_back({ speakerBot->GetGUID(), player->GetGUID(),
            speakerBot->GetName(), "Give us a moment.", ackChatType,
            AI_KIND_ORDER, "", true, "" });
    }
    return true;
}

// Recognition greeting via the AI: the bot whispers FIRST, grounded in the
// full persona + social narrative; the greeting joins the pair's chat history
// so a whispered answer continues the conversation naturally. Returns false
// (caller falls back to a canned line) when disabled or over budget.
bool WlAiRecognitionGreet(uint32 botLow, uint32 playerLow)
{
    if (!CfgEnabled() || !CfgLivingEnabled() || !CfgAiGreetings())
        return false;
    if (g_livingReqToday.load() >= CfgLivingBudget())
        return false;

    Player* bot = ObjectAccessor::FindPlayer(
        ObjectGuid(HighGuid::Player, botLow));
    Player* player = ObjectAccessor::FindPlayer(
        ObjectGuid(HighGuid::Player, playerLow));
    if (!bot || !player || !player->GetSession())
        return false;

    // in a crowd, many bots may recognise one returning player at once - hold
    // the extra greetings so it reads as one or two, not a mob. A held greeting
    // returns false, so the caller still gives a canned wave (never silence).
    if (!WlSpeechAllow(bot, 2 /*greeting*/, 0))
        return false;

    return EnqueueAi(player, bot,
        "You just spotted this player returning after time apart. YOU speak "
        "first: whisper a short greeting in character, referencing where you "
        "last met or something from your shared history. One or two short "
        "sentences, no more.",
        CHAT_MSG_WHISPER, "", /*directed=*/false, /*botInitiates=*/true);
}

// Enhanced WorldChat consumes its channel hook after formatting the message,
// so it calls this observer before broadcasting. At most one bot answers and
// the reply is sent through WC::SendWorldMessage, which avoids reply loops.
void WlAiObserveWorldChat(Player* speaker, std::string const& msg, uint32 channelId, std::string const& channelName)
{
    if (!speaker || msg.empty() || !CfgEnabled() || !CfgPublicChannelChat())
        return;

    bool const speakerIsBot = IsBotPlayer(speaker);
    if (!speakerIsBot && !WlIsRealPlayer(speaker))
        return;
    if (IsBotOrder(msg))
        return;

    std::vector<Player*> bots;
    for (auto const& pair : ObjectAccessor::GetPlayers())
    {
        Player* candidate = pair.second;
        if (!candidate || candidate == speaker || !candidate->IsInWorld()
            || !candidate->IsAlive() || !IsBotPlayer(candidate))
            continue;
        if (channelId == 1 && (candidate->GetTeamId() != speaker->GetTeamId()
            || candidate->GetZoneId() != speaker->GetZoneId()))
            continue;
        bots.push_back(candidate);
    }
    if (bots.empty())
        return;

    Player* target = FindNamedBot(bots, msg);
    bool const directed = target != nullptr;
    if (!target)
    {
        uint32 const chance = speakerIsBot ? CfgPublicBotChance() : CfgPublicPlayerChance();
        if (!roll_chance_i(static_cast<int32>(chance)))
            return;
        target = bots[urand(0, uint32(bots.size()) - 1)];
        // A real player's public-channel message is a player-driven reply,
        // not low-value ambient chatter.  Give it group-reply priority so the
        // area chatter governor does not routinely suppress it. Bot-originated
        // public chatter stays ambient to prevent loops and API spam.
        if (!WlSpeechAllow(target, speakerIsBot ? 0 /*ambient*/ : 3 /*player reply*/, 0))
            return;
    }

    uint64 const ak = AngerKey(target->GetGUID().GetCounter(), speaker->GetGUID().GetCounter());
    LoadMemory(target->GetGUID().GetCounter(), speaker->GetGUID().GetCounter());
    EnqueueAi(speaker, target, msg, CHAT_MSG_CHANNEL, BumpMood(ak, msg), directed,
        /*botInitiates=*/false, /*orderable=*/false, /*partyWide=*/false, channelId, channelName);
}

/* -------------------------------------------------------------------------- */
/*  PlayerScript: detect a real player's whisper to a bot -> queue an AI call  */
/* -------------------------------------------------------------------------- */
class WowLegendsAiChatPlayer : public PlayerScript
{
public:
    WowLegendsAiChatPlayer() : PlayerScript("WowLegendsAiChatPlayer",
        { PLAYERHOOK_CAN_PLAYER_USE_CHAT,
          PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
          PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
          PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT }) { }

    // --- a real player WHISPERS a bot ---
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Player* receiver) override
    {
        // ignore hidden addon-channel traffic (see the note at the party hook)
        if (lang == LANG_ADDON)
            return true;
        if (!CfgEnabled() || type != CHAT_MSG_WHISPER || msg.empty())
            return true;
        if (!WlIsRealPlayer(player) || !IsBotPlayer(receiver))
            return true;

        // WL: "!"-prefixed whispers (e.g. "!stay", "!follow") are bot ORDERS handled by
        // mod-playerbots, not conversation. Skip AI chat so the order obeys cleanly instead
        // of the companion replying with a roleplay line.
        if (IsBotOrder(msg))
            return true;

        // WL talk-AND-command: the bot's MASTER whispering their grouped bot
        // may be giving an ORDER. Exact phrases ("follow me", "stay here")
        // dispatch instantly - free, zero latency. Everything else goes to
        // the AI with rq.orderable set: the SAME call that writes the reply
        // also flags an order ({"order","say"}), so free-form commands like
        // "wait here a sec" obey too, at exactly one chat reply's cost.
        // Only the master commands the bot (mirrors the security gate the
        // bot enforces on its own tick, so an ack never promises what the
        // bot would refuse); anyone else's words stay normal AI chat. An
        // EMPTY command prefix disables the bridge: mod-playerbots' own
        // whisper hook already feeds every raw whisper to the command
        // parser then, and dispatching a second copy could double-execute.
        bool orderable = false;
        if (CfgAiCommandEnabled() && !sPlayerbotAIConfig.commandPrefix.empty())
        {
            Group* grp = receiver->GetGroup();
            if (grp && grp == player->GetGroup())
            {
                PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(receiver);
                if (botAI && botAI->GetMaster() == player)
                {
                    uint64 ck = AngerKey(receiver->GetGUID().GetCounter(),
                                         player->GetGUID().GetCounter());
                    time_t nowT = time(nullptr);

                    std::string norm = NormalizeOrderText(msg);
                    std::string verb = MatchOrderIntentNorm(norm);
                    // "attack the young nightsaber" works with NO AI at all -
                    // but ONLY when the name resolves against the live grid
                    // right here ("kill time" stays chat for the AI to
                    // disambiguate); dispatch re-resolves it again anyway
                    // (the mob can die while the order sits queued).
                    std::string targetName = verb.empty()
                        ? ParseAttackTargetName(norm) : "";
                    if (!targetName.empty()
                        && !AttackNameResolvesNearby(player, targetName))
                        targetName.clear();
                    if (!targetName.empty())
                        verb = "attack";

                    // "take me to the crossroads" - the destination resolves
                    // at dispatch (static stores, honest refusal if unknown)
                    if (verb.empty() && WlBotGuideEnabled())
                    {
                        std::string const guideTo = ParseGuideTargetName(norm);
                        if (!guideTo.empty())
                        {
                            verb = "guide";
                            targetName = guideTo;
                        }
                    }

                    if (!verb.empty())
                    {
                        // exact phrases cost nothing - they bypass the 2s
                        // LLM throttle so a panic "stay! stay!" always lands
                        g_orderCooldown[ck] = nowT + 2;
                        std::string ack = targetName.empty() || verb == "guide"
                            ? OrderAck(verb)
                            : "On it - going for the " + targetName + ".";
                        std::lock_guard<std::mutex> lk(g_outMtx);
                        g_out.push_back({ receiver->GetGUID(), player->GetGUID(),
                            receiver->GetName(), ack,
                            CHAT_MSG_WHISPER, AI_KIND_ORDER, verb,
                            false, targetName });
                        return true;
                    }

                    // free-form costs one AI call: 2s per-pair throttle. On
                    // cooldown the whisper stays NORMAL CHAT instead of
                    // being swallowed whole (no order, but also no silence).
                    auto cool = g_orderCooldown.find(ck);
                    if (cool == g_orderCooldown.end() || nowT >= cool->second)
                    {
                        g_orderCooldown[ck] = nowT + 2;
                        orderable = true;   // the AI decides order-vs-chat
                    }
                }
            }
        }

        uint64 ak = AngerKey(receiver->GetGUID().GetCounter(), player->GetGUID().GetCounter());
        LoadMemory(receiver->GetGUID().GetCounter(), player->GetGUID().GetCounter());
        std::string mood = BumpMood(ak, msg);
        int anger = g_anger[ak];

        // physical actions (whisper context only)
        if (CfgRudeActions())
        {
            Group* grp = receiver->GetGroup();
            bool grouped = grp && grp == player->GetGroup();
            if (grouped && anger >= static_cast<int>(CfgLeaveAnger()))
            {
                receiver->HandleEmoteCommand(EMOTE_ONESHOT_RUDE);
                receiver->RemoveFromGroup();   // anger NOT reset: keeps escalating
            }
            else if (!grouped && anger >= static_cast<int>(CfgRoarAnger())
                     && receiver->IsAlive() && receiver->IsWithinDist(player, 40.0f))
            {
                receiver->HandleEmoteCommand(EMOTE_ONESHOT_ROAR);
                g_anger[ak] = 0;   // vented into the roar
            }
        }

        if (!EnqueueAi(player, receiver, msg, CHAT_MSG_WHISPER, mood,
                /*directed=*/true, /*botInitiates=*/false, orderable)
            && orderable)
        {
            // the queue cap / per-bot floor swallowed an order candidate: a
            // busy ack via the dedupe-free orders path (empty order = ack
            // only, nothing dispatched) so it is never silently dropped
            std::lock_guard<std::mutex> lk(g_outMtx);
            g_out.push_back({ receiver->GetGUID(), player->GetGUID(),
                receiver->GetName(), "Give me a moment.",
                CHAT_MSG_WHISPER, AI_KIND_ORDER, "" });
        }
        return true;
    }

    // --- a real player speaks in PARTY/RAID chat: maybe one group-bot chimes in ---
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Group* group) override
    {
        // ignore hidden addon-channel traffic: comms addons (Questie, DBM,
        // healer/boss sync...) broadcast over LANG_ADDON on PARTY/RAID/GUILD/
        // WHISPER, which the player never sees in chat. Feeding it to the bot
        // AI as if it were the player's words made bots "reply" to invisible
        // addon data (e.g. ranting about "Questie"/"addon filth").
        if (lang == LANG_ADDON)
            return true;
        if (!CfgEnabled() || !CfgPartyChat() || !group || msg.empty() || !WlIsRealPlayer(player))
            return true;

        WlSocialPlayerSpoke(player, type);

        // WL: ignore "!"-prefixed bot orders in party/raid chat too (handled by mod-playerbots).
        if (IsBotOrder(msg))
            return true;

        std::vector<Player*> bots;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            if (Player* m = itr->GetSource())
                if (m != player && IsBotPlayer(m))
                    bots.push_back(m);
        if (bots.empty())
            return true;

        uint32 outType = (type == CHAT_MSG_RAID || type == CHAT_MSG_RAID_LEADER || type == CHAT_MSG_RAID_WARNING)
                       ? CHAT_MSG_RAID : CHAT_MSG_PARTY;

        // --- T&C v2: party-wide orders ("everyone follow me") ---------------
        // Only group-ADDRESSED phrasings turn into commands, so normal party
        // banter never moves a bot; the fan-out to every owned bot happens at
        // dispatch (DispatchBotOrder, partyWide).
        if (!FindNamedBot(bots, msg))      // "guys, Mira stay here" -> Mira's
                                           // own order surface further below
        {
            std::vector<Player*> owned;
            for (Player* b : bots)
                if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(b))
                    if (ai->GetMaster() == player)
                        owned.push_back(b);
            if (TryGroupOrder(player, owned, msg, outType))
                return true;
        }

        // a named bot answers directly (quality tier); otherwise ONE random bot may chime in (fast tier)
        Player* target = FindNamedBot(bots, msg);
        bool directed = (target != nullptr);
        if (!target)
        {
            if (!roll_chance_i(static_cast<int>(CfgAmbientChance())))
                return true;
            target = bots[urand(0, uint32(bots.size()) - 1)];
            // This is still a real player's group message, so give the one
            // selected bot player-reply priority. Only one bot is selected.
            if (!WlSpeechAllow(target, 3 /*player reply*/, 0))
                return true;
        }

        // T&C v2: "<BotName> follow me" in party chat is an order surface for
        // the bot's own master, exactly like a whisper (the LLM decides
        // order-vs-chat; same 2s per-pair order throttle)
        bool orderable = false;
        if (directed && CfgAiCommandEnabled() && CfgPartyOrders()
            && !sPlayerbotAIConfig.commandPrefix.empty())
            if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(target))
                if (ai->GetMaster() == player)
                {
                    uint64 ck = AngerKey(target->GetGUID().GetCounter(),
                                         player->GetGUID().GetCounter());
                    time_t nowT = time(nullptr);
                    auto cool = g_orderCooldown.find(ck);
                    orderable = cool == g_orderCooldown.end()
                        || nowT >= cool->second;
                    if (orderable)
                        g_orderCooldown[ck] = nowT + 2;
                }

        uint64 ak = AngerKey(target->GetGUID().GetCounter(), player->GetGUID().GetCounter());
        LoadMemory(target->GetGUID().GetCounter(), player->GetGUID().GetCounter());
        EnqueueAi(player, target, msg, outType, BumpMood(ak, msg), directed,
            /*botInitiates=*/false, orderable);
        return true;
    }

    // --- a real player speaks in GUILD chat: one guild-bot may answer ---
    bool OnPlayerCanUseChat(Player* player, uint32 /*type*/, uint32 lang, std::string& msg, Guild* guild) override
    {
        // ignore hidden addon-channel traffic (see the note at the party hook)
        if (lang == LANG_ADDON)
            return true;
        if (!CfgEnabled() || !CfgGuildChat() || !guild || msg.empty() || !WlIsRealPlayer(player))
            return true;

        WlSocialPlayerSpoke(player, CHAT_MSG_GUILD);

        // WL: ignore "!"-prefixed bot orders in guild chat too (handled by mod-playerbots).
        if (IsBotOrder(msg))
            return true;

        std::vector<Player*> bots;
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* p = pair.second;
            if (!p || p == player || p->GetGuildId() != guild->GetId() || !IsBotPlayer(p))
                continue;
            bots.push_back(p);
        }
        if (bots.empty())
            return true;

        Player* target = FindNamedBot(bots, msg);
        bool const directed = target != nullptr;
        if (!target)
        {
            if (!roll_chance_i(static_cast<int32>(CfgGuildChance())))
                return true;
            target = bots[urand(0, uint32(bots.size()) - 1)];
            if (!WlSpeechAllow(target, 3 /*player reply*/, 0))
                return true;
        }

        uint64 ak = AngerKey(target->GetGUID().GetCounter(), player->GetGUID().GetCounter());
        LoadMemory(target->GetGUID().GetCounter(), player->GetGUID().GetCounter());
        EnqueueAi(player, target, msg, CHAT_MSG_GUILD, BumpMood(ak, msg), directed);
        return true;
    }

    // --- a real player SAYS something out loud: a nearby bot may answer aloud (proximity/city) ---
    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg) override
    {
        // ignore hidden addon-channel traffic (see the note at the party hook)
        if (lang == LANG_ADDON)
            return true;
        if (!CfgEnabled() || msg.empty() || !WlIsRealPlayer(player))
            return true;
        if (type != CHAT_MSG_SAY && type != CHAT_MSG_YELL)
            return true;

        // gather bots near the speaker via a grid search (local, not all-online)
        // - the wider of the chat range and order earshot (~30y), shared by
        // spoken orders and proximity chat below
        float range = std::max(CfgProximityRange(), 30.0f);
        std::list<Player*> nearby;
        Acore::AnyPlayerInObjectRangeCheck check(player, range);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(player, nearby, check);
        Cell::VisitObjects(player, searcher, range);

        std::vector<Player*> bots;
        for (Player* p : nearby)
            if (p != player && IsBotPlayer(p) && p->IsAlive())
                bots.push_back(p);
        if (bots.empty())
            return true;

        // T&C v2: SPOKEN orders - "everyone follow me" said out loud works
        // exactly like party chat (players type into whatever channel the
        // chat box last used; Kneuma's first live test proved it). Only the
        // speaker's own GROUPED bots within earshot obey; independent of the
        // ProximityChat toggle (orders are not ambient chatter).
        if (!FindNamedBot(bots, msg))
        {
            if (Group* grp = player->GetGroup())
            {
                std::vector<Player*> owned;
                for (Player* b : bots)
                    if (b->GetGroup() == grp)
                        if (PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(b))
                            if (ai->GetMaster() == player)
                                owned.push_back(b);
                if (TryGroupOrder(player, owned, msg, CHAT_MSG_SAY))
                    return true;
            }
        }

        if (!CfgProximityChat())
            return true;

        // proximity CHAT keeps its own (possibly narrower) configured range
        // even though the order earshot above may be wider
        std::vector<Player*> chatBots;
        float const chatRange = CfgProximityRange();
        for (Player* b : bots)
            if (player->GetDistance(b) <= chatRange)
                chatBots.push_back(b);
        if (chatBots.empty())
            return true;

        // a named bot answers (quality tier); otherwise ONE random nearby bot may pipe up (fast tier)
        Player* target = FindNamedBot(chatBots, msg);
        bool directed = (target != nullptr);
        if (!target)
        {
            if (!roll_chance_i(static_cast<int>(CfgProximityChance())))
                return true;
            target = chatBots[urand(0, uint32(chatBots.size()) - 1)];
            // A real player's visible /say or /yell is player-driven chat,
            // even when no bot name was mentioned.
            if (!WlSpeechAllow(target, 3 /*player reply*/, 0))
                return true;
        }

        uint64 ak = AngerKey(target->GetGUID().GetCounter(), player->GetGUID().GetCounter());
        LoadMemory(target->GetGUID().GetCounter(), player->GetGUID().GetCounter());
        EnqueueAi(player, target, msg, CHAT_MSG_SAY, BumpMood(ak, msg), directed);
        return true;
    }
};

/* -------------------------------------------------------------------------- */
/*  WorldScript: pump the worker + deliver ready replies on the main thread    */
/* -------------------------------------------------------------------------- */
class WowLegendsAiChatWorld : public WorldScript
{
public:
    WowLegendsAiChatWorld() : WorldScript("WowLegendsAiChatWorld",
        { WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_STARTUP,
          WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_SHUTDOWN }) { }

    // Workers read config constantly while `.reload config` repopulates the
    // ConfigMgr map on the world thread - and the core's reads take NO lock,
    // so workers must never touch sConfigMgr. Refresh their cache here.
    void OnAfterConfigLoad(bool /*reload*/) override
    {
        RefreshWorkerCfg();
    }

    // Stop + JOIN the workers before static destruction: a detached worker
    // returning from a slow provider call after teardown would touch
    // destroyed queues/mutexes. The join is bounded by the socket timeout.
    void OnShutdown() override
    {
        if (!g_run.exchange(false))
            return;
        g_cv.notify_all();
        for (std::thread& t : g_workers)
            if (t.joinable())
                t.join();
        g_workers.clear();
    }

    // Auto-create the persistent-memory table on every worldserver start, so each
    // repack owner gets it with ZERO manual SQL (this is a repack default). Runs once
    // at startup; CREATE TABLE IF NOT EXISTS is idempotent. DirectExecute blocks here,
    // which is the documented pattern for one-time startup DDL.
    //
    // Random bots re-rolled via `.playerbots rndbot init` get NEW character
    // guids, orphaning their old (bot_guid, player_guid) rows — swept below,
    // mirroring the companion module's integrity sweep.
    void OnStartup() override
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS playerbot_ai_chat_memory ("
            "bot_guid INT UNSIGNED NOT NULL, "
            "player_guid INT UNSIGNED NOT NULL, "
            "history MEDIUMTEXT, "
            "anger INT NOT NULL DEFAULT 0, "
            "updated_at TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, "
            "PRIMARY KEY (bot_guid, player_guid)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
        LOG_INFO("server", "[aichat] playerbot_ai_chat_memory table ready (persistent chat memory)");

        // build the Sage fact indexes now instead of hitching the world
        // tick on the first question
        WlSageWarmup();

        // Orphan sweep: drop memory rows whose bot character no longer exists.
        // Count first (DirectExecute has no affected-rows return); blocking is
        // fine for one-time startup maintenance.
        uint64 orphans = 0;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM playerbot_ai_chat_memory "
                "WHERE bot_guid NOT IN (SELECT guid FROM characters)"))
            orphans = r->Fetch()[0].Get<uint64>();
        if (orphans)
            CharacterDatabase.DirectExecute(
                "DELETE FROM playerbot_ai_chat_memory "
                "WHERE bot_guid NOT IN (SELECT guid FROM characters)");
        LOG_INFO("server", "[aichat] orphan-memory sweep removed {} rows", orphans);
    }

    // Try to deliver one ready reply. Returns true if delivered (or no longer
    // deliverable, e.g. the group is gone), false if the recipient/bot simply
    // isn't connected right now — so the caller retries on a later tick.
    static bool TryDeliver(AiResult const& r)
    {
        if (r.chatType == CHAT_MSG_WHISPER)
        {
            Player* pl = ObjectAccessor::FindConnectedPlayer(r.player);
            if (!pl) return false;
            // Deliver straight to the player's session — no live bot needed.
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, Language(LANG_UNIVERSAL),
                                         r.bot, r.player, r.reply, /*chatTag=*/0,
                                         r.botName, pl->GetName());
            pl->SendDirectMessage(&data);
            LOG_INFO("server", "[aichat] deliver whisper {} -> {} (sessionDirect)", r.botName, pl->GetName());
            return true;
        }
        if (r.chatType == CHAT_MSG_PARTY || r.chatType == CHAT_MSG_RAID)
        {
            Player* pl = ObjectAccessor::FindConnectedPlayer(r.player);
            if (!pl) return false;
            Group* grp = pl->GetGroup();
            if (!grp) return true;   // no group left to deliver to — stop retrying
            WorldPacket data;
            ChatHandler::BuildChatPacket(data, ChatMsg(r.chatType), Language(LANG_UNIVERSAL),
                                         r.bot, ObjectGuid::Empty, r.reply, /*chatTag=*/0,
                                         r.botName, "");
            grp->BroadcastPacket(&data, false);
            LOG_INFO("server", "[aichat] deliver {} {} -> party (sessionDirect)",
                     r.chatType == CHAT_MSG_RAID ? "raid" : "party", r.botName);
            return true;
        }
        // CHAT_MSG_GUILD / CHAT_MSG_SAY / CHAT_MSG_CHANNEL need the live bot.
        Player* bot = ObjectAccessor::FindConnectedPlayer(r.bot);
        if (!bot) return false;
        if (r.chatType == CHAT_MSG_GUILD)
        {
            if (Guild* g = bot->GetGuild())
                g->BroadcastToGuild(bot->GetSession(), false, r.reply, LANG_UNIVERSAL);
        }
        else if (r.chatType == CHAT_MSG_SAY)
            bot->Say(r.reply, LANG_UNIVERSAL);
        else if (r.chatType == CHAT_MSG_CHANNEL)
        {
            if (r.channelId != 0)
            {
                Player* speaker = ObjectAccessor::FindConnectedPlayer(r.player);
                if (!speaker)
                    return true;
                if (ChannelMgr* manager = ChannelMgr::forTeam(speaker->GetTeamId()))
                {
                    if (Channel* channel = manager->GetChannel(r.channelName, speaker, false))
                    {
                        if (channel->GetChannelId() == r.channelId)
                        {
                            channel->Say(bot->GetGUID(), r.reply, LANG_UNIVERSAL);
                            return true;
                        }
                    }
                }
                LOG_INFO("server", "[aichat] drop channel reply {}: channel {} membership changed",
                    r.botName, r.channelId);
                return true;
            }
            WC::SendWorldMessage(*bot, r.reply, -1);
        }
        LOG_INFO("server", "[aichat] deliver {} {}",
            r.chatType == CHAT_MSG_GUILD ? "guild" :
            (r.chatType == CHAT_MSG_CHANNEL ? "world" : "say"), r.botName);
        return true;
    }

    // WL-owned role-NPC spawn catalog for the guide (the playerbots travel
    // catalog is never populated in this fork - dead data). Built lazily
    // ONCE on the world thread from the startup-loaded spawn store.
    struct WlRoleNpcSpawn
    {
        uint32 mapId;
        float x, y, z;
        uint32 entry;
        uint32 npcflag;
    };
    static std::vector<WlRoleNpcSpawn> const& WlRoleNpcCatalog()
    {
        static std::vector<WlRoleNpcSpawn> catalog;
        static bool built = false;
        if (!built)
        {
            built = true;
            uint32 const roleMask = UNIT_NPC_FLAG_INNKEEPER
                | UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_REPAIR
                | UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_BANKER
                | UNIT_NPC_FLAG_AUCTIONEER | UNIT_NPC_FLAG_STABLEMASTER
                | UNIT_NPC_FLAG_TRAINER;
            for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
            {
                CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(data.id);
                if (!tmpl || !(tmpl->npcflag & roleMask))
                    continue;
                catalog.push_back({ data.mapid, data.posX, data.posY, data.posZ,
                    data.id, tmpl->npcflag });
            }
            LOG_INFO("server", "[aichat] guide role-NPC catalog: {} spawns",
                     catalog.size());
        }
        return catalog;
    }

    // "BootyBay" -> "Booty Bay" for spoken labels
    static std::string WlPrettifyName(std::string const& raw)
    {
        std::string out;
        out.reserve(raw.size() + 4);
        for (std::size_t i = 0; i < raw.size(); ++i)
        {
            if (i && std::isupper(static_cast<unsigned char>(raw[i]))
                && std::islower(static_cast<unsigned char>(raw[i - 1])))
                out += ' ';
            out += raw[i];
        }
        return out;
    }

    // --- "The Guide" destination resolver -------------------------------
    // World thread, READ-ONLY static stores loaded at startup. Turns a
    // spoken destination into "wl guide <mapId> <x> <y> <z> <label>" or an
    // honest refusal. The spoken name is only ever MATCHED against server
    // data - never executed, never echoed back into chat.
    static bool WlResolveGuideDestination(Player* owner, std::string const& spoken,
        std::string& payload, std::string& refusal)
    {
        std::string name = NormalizeOrderText(spoken);
        refusal = "I don't know the way there.";
        if (name.empty())
            return false;

        // continents are not destinations - be honest instead of clueless
        for (char const* cont : { "eastern kingdoms", "kalimdor", "outland",
                                  "northrend", "azeroth" })
            if (name == cont)
            {
                refusal = "That's a whole continent - name a place on it and I'll lead the way.";
                return false;
            }

        // descriptive aliases players actually say: "the troll starting
        // zone" is not a place NAME, but there are only nine of them ever
        if (name.find("start") != std::string::npos)
        {
            struct StartZone
            {
                char const* race;
                char const* zone;
            };
            static StartZone const startZones[] =
            {
                { "troll",     "valley of trials" },
                { "orc",       "valley of trials" },
                { "tauren",    "camp narache" },
                { "undead",    "deathknell" },
                { "forsaken",  "deathknell" },
                { "human",     "northshire" },
                { "dwarf",     "coldridge valley" },
                { "gnome",     "coldridge valley" },
                { "night elf", "shadowglen" },
                { "nightelf",  "shadowglen" },
                { "blood elf", "sunstrider isle" },
                { "bloodelf",  "sunstrider isle" },
                { "draenei",   "ammen vale" },
            };
            bool aliased = false;
            for (StartZone const& sz : startZones)
                if (name.find(sz.race) != std::string::npos)
                {
                    name = sz.zone;
                    aliased = true;
                    break;
                }
            if (!aliased)
            {
                // no race named: the SPEAKER's own starting zone
                switch (owner->getRace())
                {
                    case RACE_ORC:
                    case RACE_TROLL:    name = "valley of trials"; break;
                    case RACE_TAUREN:   name = "camp narache"; break;
                    case RACE_UNDEAD_PLAYER: name = "deathknell"; break;
                    case RACE_HUMAN:    name = "northshire"; break;
                    case RACE_DWARF:
                    case RACE_GNOME:    name = "coldridge valley"; break;
                    case RACE_NIGHTELF: name = "shadowglen"; break;
                    case RACE_BLOODELF: name = "sunstrider isle"; break;
                    case RACE_DRAENEI:  name = "ammen vale"; break;
                    default: break;
                }
            }
        }

        uint32 const mapId = owner->GetMapId();
        // zTolerant: quest destinations get their z from a surface probe, so
        // arrival may honestly happen far above/below the stored z; precise
        // destinations (teleports, NPCs, entrances) must be truly reached
        auto emit = [&](float x, float y, float z, std::string const& label,
                        bool zTolerant = false)
        {
            payload = "wl guide " + std::to_string(mapId) + " "
                + std::to_string(x) + " " + std::to_string(y) + " "
                + std::to_string(z) + " " + std::string(zTolerant ? "1" : "0")
                + " " + label;
            return true;
        };

        // 1) "take me to my quest" - a ready TURN-IN first (the most useful
        // ask), else the objective area of an active quest (quest_poi)
        if (name.find("quest") != std::string::npos)
        {
            for (int pass = 0; pass < 2; ++pass)
            {
                QuestStatus const wantStatus = pass == 0
                    ? QUEST_STATUS_COMPLETE : QUEST_STATUS_INCOMPLETE;
                for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
                {
                    uint32 const questId = owner->GetQuestSlotQuestId(slot);
                    if (!questId || owner->GetQuestStatus(questId) != wantStatus)
                        continue;

                    QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(questId);
                    if (!pois)
                        continue;

                    for (QuestPOI const& poi : *pois)
                    {
                        // turn-in POI (-1) for complete quests, objective
                        // POIs for active ones
                        if (poi.MapId != mapId || poi.points.empty())
                            continue;
                        if (pass == 0 ? poi.ObjectiveIndex != -1
                                      : poi.ObjectiveIndex < 0)
                            continue;

                        float ax = 0.0f, ay = 0.0f;
                        for (QuestPOIPoint const& p : poi.points)
                        {
                            ax += p.x;
                            ay += p.y;
                        }
                        ax /= float(poi.points.size());
                        ay /= float(poi.points.size());

                        // probe near the player's own height first so indoor
                        // objectives don't resolve to the roof above them
                        float az = owner->GetMap()->GetHeight(
                            owner->GetPhaseMask(), ax, ay,
                            owner->GetPositionZ() + 5.0f);
                        if (az <= INVALID_HEIGHT)
                            az = owner->GetMap()->GetHeight(
                                owner->GetPhaseMask(), ax, ay, MAX_HEIGHT);
                        if (az <= INVALID_HEIGHT)
                            continue;

                        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
                        return emit(ax, ay, az,
                            quest ? quest->GetTitle() : "your quest",
                            /*zTolerant=*/true);
                    }
                }
            }
            refusal = "None of your quests lead anywhere near here.";
            return false;
        }

        // 2) role NPCs ("the innkeeper", "my trainer", ...) - nearest spawn
        // from the playerbots travel catalog (startup-built, read-only)
        uint32 roleFlag = 0;
        bool classTrainer = false;
        std::string roleLabel;
        if (name.find("keeper") != std::string::npos || name == "inn"
            || name.find("tavern") != std::string::npos)
        {
            // "keeper" also forgives the inevitable "inkeeper" typo
            roleFlag = UNIT_NPC_FLAG_INNKEEPER;
            roleLabel = "the innkeeper";
        }
        else if (name.find("flight") != std::string::npos
                 || name.find("gryphon") != std::string::npos
                 || name.find("wind rider") != std::string::npos)
        {
            roleFlag = UNIT_NPC_FLAG_FLIGHTMASTER;
            roleLabel = "the flight master";
        }
        else if (name.find("repair") != std::string::npos)
        {
            roleFlag = UNIT_NPC_FLAG_REPAIR;
            roleLabel = "a repairer";
        }
        else if (name.find("vendor") != std::string::npos
                 || name.find("merchant") != std::string::npos)
        {
            roleFlag = UNIT_NPC_FLAG_VENDOR;
            roleLabel = "a vendor";
        }
        else if (name.find("bank") != std::string::npos)
        {
            roleFlag = UNIT_NPC_FLAG_BANKER;
            roleLabel = "the bank";
        }
        else if (name.find("auction") != std::string::npos)
        {
            roleFlag = UNIT_NPC_FLAG_AUCTIONEER;
            roleLabel = "the auction house";
        }
        else if (name.find("stable") != std::string::npos)
        {
            roleFlag = UNIT_NPC_FLAG_STABLEMASTER;
            roleLabel = "the stable master";
        }
        else if (name.find("trainer") != std::string::npos)
        {
            roleFlag = UNIT_NPC_FLAG_TRAINER;
            classTrainer = true;
            roleLabel = "your trainer";
        }

        if (roleFlag)
        {
            WlRoleNpcSpawn const* best = nullptr;
            float bestDist = 100000.0f;
            for (WlRoleNpcSpawn const& spawn : WlRoleNpcCatalog())
            {
                if (spawn.mapId != mapId || !(spawn.npcflag & roleFlag))
                    continue;

                if (classTrainer)
                {
                    Trainer::Trainer const* trainer = sObjectMgr->GetTrainer(spawn.entry);
                    if (!trainer
                        || trainer->GetTrainerType() != Trainer::Type::Class
                        || trainer->GetTrainerRequirement() != owner->getClass())
                        continue;
                }

                float const d = owner->GetDistance(spawn.x, spawn.y, spawn.z);
                if (d < bestDist)
                {
                    bestDist = d;
                    best = &spawn;
                }
            }
            if (best)
            {
                CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(best->entry);
                std::string label = roleLabel;
                if (tmpl)
                    label += " - " + tmpl->Name;
                return emit(best->x, best->y, best->z, label);
            }
            refusal = "I can't find " + roleLabel + " around here.";
            return false;
        }

        // 3) doorstep destinations: places whose catalog point is interior or
        // underground, where GROUND routing cannot end (lift cities, sealed
        // courts). Field case: "Undercity" teleports to z=-52 - a node pocket
        // disconnected from the mainland - so the guide apologized at random
        // spots. The honest, correct escort ends at the DOORSTEP, and the
        // label says what to do from there. (Verified: the gate point below
        // resolves 8yd from a mainland graph node.)
        std::string query = name;
        query.erase(std::remove(query.begin(), query.end(), ' '), query.end());
        {
            struct Doorstep
            {
                char const* key;
                uint32 map;
                float x, y, z;
                char const* label;
            };
            static Doorstep const doorsteps[] =
            {
                // labels must fit the guide's 48-char spoken-label cap
                { "undercity", 0, 1595.0f, 451.0f, 38.0f,
                  "the Undercity gates - take the lifts down" },
            };
            for (Doorstep const& d : doorsteps)
            {
                if (d.map != mapId)
                    continue;
                // anchored match (review catch: "thunder city" space-strips
                // to "thundercity", which CONTAINS "undercity")
                std::size_t const pos = name.find(d.key);
                bool const spacedHit = pos != std::string::npos
                    && (pos == 0 || name[pos - 1] == ' ');
                if (query == d.key || spacedHit)
                    return emit(d.x, d.y, d.z, d.label);
            }
        }

        // 4) dungeon entrances on this map (area trigger positions). Checked
        // BEFORE the tele catalog: an instance's catalog tele sits INSIDE
        // the walls (field case: "scarlet monastery" resolved to the walled
        // courtyard and the road ended on the cliff above it) - when the
        // spoken name IS an instance, the entrance trigger is the walkable
        // truth. Guards (review catches): capital/broken names must never be
        // shadowed ("stormwind" is INSIDE "Stormwind Stockade"; the Sunwell
        // entrance sits on a disconnected island), short/generic words must
        // not match ("keep" - "Shadowfang Keep"), and matching must survive
        // punctuation ("onyxias lair" vs "Onyxia's Lair").
        {
            static char const* neverEntrance[] =
            {
                "stormwind", "orgrimmar", "ironforge", "darnassus",
                "thunder bluff", "silvermoon", "the exodar", "exodar",
                "shattrath", "dalaran", "sunwell",
            };
            bool blocked = name.size() < 5;
            for (char const* c : neverEntrance)
                if (!blocked && name == c)
                    blocked = true;
            if (!blocked)
            {
                for (auto const& [id, tp] : sObjectMgr->GetAllAreaTriggerTeleports())
                {
                    AreaTrigger const* at = sObjectMgr->GetAreaTrigger(id);
                    if (!at || at->map != mapId)
                        continue;

                    MapEntry const* entry = sMapStore.LookupEntry(tp.target_mapId);
                    if (!entry || !entry->name[0] || !entry->name[0][0])
                        continue;

                    // normalize the DBC name the same way the spoken side was
                    // (lowercase, punctuation stripped) so "Zul'Farrak" and
                    // "Onyxia's Lair" match their spoken forms; compare both
                    // spaced and space-stripped
                    std::string const lowName = NormalizeOrderText(entry->name[0]);
                    std::string lowStripped = lowName;
                    lowStripped.erase(std::remove(lowStripped.begin(), lowStripped.end(), ' '),
                        lowStripped.end());
                    std::size_t const pos = lowName.find(name);
                    bool const spacedHit = pos != std::string::npos
                        && (pos == 0 || lowName[pos - 1] == ' ');
                    if (!spacedHit && lowStripped.find(query) == std::string::npos)
                        continue;

                    return emit(at->x, at->y, at->z,
                        std::string(entry->name[0]) + " entrance");
                }
            }
        }

        // 5) named locations from the .tele catalog. DB names are space-free
        // CamelCase ("BootyBay") while the spoken query has spaces, so both
        // sides are matched space-stripped; SAME-MAP entries win, and a
        // cross-map-only match keeps the honest boat/zeppelin refusal below.
        bool crossMapTele = false;
        if (query.size() >= 3)
        {
            GameTele const* exactHit = nullptr;
            GameTele const* subHit = nullptr;
            for (auto const& [teleId, tele] : sObjectMgr->GetGameTeleMap())
            {
                std::string low;
                low.reserve(tele.name.size());
                for (char c : tele.name)
                    if (c != ' ')
                        low += char(std::tolower(static_cast<unsigned char>(c)));
                if (low.find(query) == std::string::npos)
                    continue;

                if (tele.mapId != mapId)
                {
                    crossMapTele = true;
                    continue;
                }
                if (low == query)
                {
                    exactHit = &tele;
                    break;
                }
                if (!subHit)
                    subHit = &tele;
            }
            if (GameTele const* hit = exactHit ? exactHit : subHit)
                return emit(hit->position_x, hit->position_y, hit->position_z,
                    WlPrettifyName(hit->name));
        }

        // the name IS known, just not walkable from here
        if (crossMapTele)
            refusal = "That's beyond this land - we'd need a ship or zeppelin.";
        return false;
    }

    // talk-AND-command: hand one matched order to the bot. World thread
    // (OnUpdate drain) ONLY. PlayerbotAI::HandleCommand just enqueues into
    // the bot's deferredChatCommands and the bot runs it on its OWN tick -
    // we never touch the engine, never run off-tick (#2474-safe by
    // construction). Returns true if the order was handed over; on false,
    // r.reply has been replaced with an HONEST refusal for the ack whisper
    // (the AI's say may have promised an action that will not happen).
    static bool DispatchBotOrder(AiResult& r)
    {
        auto refuse = [&r](char const* text)
        {
            r.reply = text;
            return false;
        };

        // a `.reload config` can disable the feature while an order is in
        // flight
        if (!CfgAiCommandEnabled())
            return refuse("Can't do that right now.");

        Player* bot = ObjectAccessor::FindConnectedPlayer(r.bot);
        Player* owner = ObjectAccessor::FindConnectedPlayer(r.player);
        if (!bot || !owner || !owner->GetSession())
            return refuse("Can't do that right now.");

        // the world moved while the order sat queued: still the same group?
        Group* grp = bot->GetGroup();
        if (!grp || grp != owner->GetGroup())
            return refuse("Can't do that right now.");

        // mod-playerbots' own cached prefix ("$" on WL) - the exact value
        // HandleCommandInternal strips, shared default included, no drift.
        std::string const& prefix = sPlayerbotAIConfig.commandPrefix;
        if (prefix.empty())
            return refuse("Can't do that right now.");

        // verb -> verified chat trigger; "heal" carries the owner's name
        OrderVerb const* v = FindOrderVerb(r.order);
        if (!v)
            return refuse("Can't do that right now.");

        // T&C v2 attack-by-name: re-resolve the spoken name against the LIVE
        // grid around the owner. The selection write happens LATER, only
        // once at least one bot is guaranteed to take the order - a refusal
        // must never leave the owner's selection hijacked. The name is
        // matched, never echoed, so a bogus LLM value can only produce this
        // one refusal.
        Unit* mark = nullptr;
        if (r.order == "attack" && !r.orderTarget.empty())
        {
            // one mark at a time per owner: two named attacks relayed
            // through the shared selection would silently retarget the
            // first (the bot reads the master's target on its OWN tick)
            uint32 const ownerLow = owner->GetGUID().GetCounter();
            time_t const nowT = time(nullptr);
            auto busy = g_markCooldown.find(ownerLow);
            if (busy != g_markCooldown.end() && nowT < busy->second)
                return refuse("One mark at a time - let me finish this one.");

            mark = FindNearbyEnemyByName(owner, r.orderTarget);
            if (!mark)
                return refuse("I don't see it around here.");
        }

        // "The Guide": resolve the spoken destination now (world thread,
        // static stores); the coords ride the command text to the bot
        std::string guidePayload;
        if (r.order == "guide")
        {
            // an LLM can still emit the verb while the feature is off - a
            // disabled guide must refuse, never promise and freeze
            if (!WlBotGuideEnabled())
                return refuse("I don't do escorts right now.");

            if (r.orderTarget.empty())
                return refuse("Where to? Name a place, an innkeeper, your trainer, your quest...");

            std::string guideRefusal;
            if (!WlResolveGuideDestination(owner, r.orderTarget, guidePayload, guideRefusal))
            {
                r.reply = guideRefusal;
                return false;
            }
            // WL FIX (2026-07-20): an escort is inherently a ONE-bot job. Said
            // to the group ("guys, take me to Orgrimmar") it used to fan out:
            // every owned bot planned its own route (N duplicate jobs against
            // an 8-deep pool - two players could exhaust it) and every one of
            // them announced "Follow me!" and tried to lead. One guide, one
            // guide bot.
            r.partyWide = false;
        }

        // party-wide home/grind: hearthing or grind-flipping the WHOLE
        // owned squad on one sentence is drastic - ask once, obey the
        // repeat. 45s window: a human reads the ask, maybe types something
        // else first, then repeats (the first live test took 37s and the
        // old 15s window silently expired into a second identical ask).
        if (r.partyWide && (r.order == "home" || r.order == "grind"))
        {
            uint32 const ownerLow = owner->GetGUID().GetCounter();
            time_t const nowT = time(nullptr);
            auto pend = g_partyConfirm.find(ownerLow);
            if (pend == g_partyConfirm.end() || nowT > pend->second.second
                || pend->second.first != r.order)
            {
                g_partyConfirm[ownerLow] = { r.order, nowT + 45 };
                return refuse(r.order == "home"
                    ? "All of us hearth out? Say it again and we will."
                    : "All of us, kill everything here? Say it again and we will.");
            }
            g_partyConfirm.erase(pend);
        }

        // per-bot gate: master re-check + class gate. Returns the AI handle
        // to dispatch to, or nullptr with the honest refusal set.
        auto checkBot = [&](Player* member, char const*& refusal) -> PlayerbotAI*
        {
            PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(member);
            if (!botAI)
                return nullptr;

            // authoritative re-check: only the MASTER commands this bot (the
            // world may have changed while the order sat queued). Plain
            // accessor read on the world thread - engine untouched.
            if (botAI->GetMaster() != owner)
                return nullptr;

            // PullRequestAction hard-requires a TANK and fails SILENTLY for
            // everyone else (PullActions.cpp:39) - refuse honestly up front.
            // bySpec=true reads only talents/class/auras (plain core reads);
            // the default variant would read the ENGINE's strategy set
            // (#2474).
            if (r.order == "pull" && !botAI->IsTank(member, /*bySpec=*/true))
            {
                refusal = "Pulling is a tank's job - I'm not built for it.";
                return nullptr;
            }

            // UseHearthStone fails SILENTLY on cooldown - the ack must not
            // promise a hearth that will not happen (spell 8690 = Hearthstone)
            if (r.order == "home" && member->HasSpellCooldown(8690))
            {
                refusal = "My hearthstone is still cooling down.";
                return nullptr;
            }
            return botAI;
        };
        auto sendToBot = [&](PlayerbotAI* botAI)
        {
            std::string text;
            if (r.order == "heal")
                text = "focus heal +" + owner->GetName();
            else if (r.order == "guide")
                text = guidePayload;
            else
                text = v->dispatch;
            // a flee order parks the bot in the 'passive' stance and
            // PassiveMultiplier then vetoes combat actions WITHOUT any reply -
            // a later explicit combat order must wake the bot first, exactly
            // like upstream's follow/stay/grind shortcuts escape passive.
            // The 'co'/'nc' wakes ride at relevance + 1 (see WL note in
            // ChatCommandHandlerStrategy) so they always execute before the
            // order they unblock; only bots WE parked pay the wake latency.
            uint32 const botLow = botAI->GetBot()->GetGUID().GetCounter();
            if (r.order == "flee")
                g_passiveParked.insert(botLow);
            else if (r.order == "follow" || r.order == "stay" || r.order == "grind")
                g_passiveParked.erase(botLow); // these shortcuts clear passive themselves
            else if ((r.order == "attack" || r.order == "heal" || r.order == "pull")
                     && g_passiveParked.erase(botLow))
            {
                botAI->HandleCommand(CHAT_MSG_WHISPER, prefix + "co -passive", owner);
                botAI->HandleCommand(CHAT_MSG_WHISPER, prefix + "nc -passive", owner);
            }
            botAI->HandleCommand(CHAT_MSG_WHISPER, prefix + text, owner);
        };
        auto setMark = [&]()
        {
            if (!mark)
                return;
            // pure core unit-field write; the playerbots attack path reads
            // the master's target at bot-tick time. ~2s hold covers the
            // bot's react latency before the next named attack may re-mark.
            owner->SetSelection(mark->GetGUID());
            g_markCooldown[owner->GetGUID().GetCounter()] = time(nullptr) + 2;
        };

        char const* refusal = "Can't do that right now.";
        if (!r.partyWide)
        {
            PlayerbotAI* botAI = checkBot(bot, refusal);
            if (!botAI)
                return refuse(refusal);
            setMark();
            sendToBot(botAI);
            LOG_DEBUG("server", "[aichat] order '{}' -> {} (from {})",
                      r.order, r.botName, owner->GetName());
            return true;
        }

        // party-wide: every grouped bot mastered by the owner obeys; the ONE
        // ack (r.reply) speaks for the group, per-bot refusals stay silent
        // unless nobody at all could obey. Pre-pass first so the selection
        // write only happens when somebody will actually take the order.
        std::vector<PlayerbotAI*> squad;
        for (GroupReference* itr = grp->GetFirstMember(); itr; itr = itr->next())
        {
            Player* member = itr->GetSource();
            if (!member || member == owner || !IsBotPlayer(member))
                continue;
            char const* memberRefusal = "";
            if (PlayerbotAI* ai = checkBot(member, memberRefusal))
                squad.push_back(ai);
        }
        if (squad.empty())
            return refuse(r.order == "pull"
                ? "None of us is a tank - pulling is a tank's job."
                : "Can't do that right now.");
        setMark();
        for (PlayerbotAI* ai : squad)
            sendToBot(ai);
        LOG_DEBUG("server", "[aichat] party order '{}' -> {} bot(s) (from {})",
                  r.order, uint32(squad.size()), owner->GetName());
        return true;
    }

    void OnUpdate(uint32 /*diff*/) override
    {
        if (CfgEnabled())
            EnsureWorkers();

        // Roll the "today" stats + Living Chatter budget at UTC midnight.
        uint32 const day = uint32(time(nullptr) / 86400);
        if (g_statsDay.exchange(day) != day)
        {
            ResetStats(g_statsToday);
            g_livingReqToday = 0;
            g_livingTokensToday = 0;

            // hygiene: sweep expired throttle entries once a day (absent ==
            // expired for all of these, so this is behavior-neutral). The
            // conversational state maps (g_anger/g_history) are NOT expiring
            // throttles and stay.
            time_t const sweepNow = time(nullptr);
            std::erase_if(g_cooldown, [sweepNow](auto const& e) { return e.second <= sweepNow; });
            std::erase_if(g_orderCooldown, [sweepNow](auto const& e) { return e.second <= sweepNow; });
            std::erase_if(g_markCooldown, [sweepNow](auto const& e) { return e.second <= sweepNow; });
            std::erase_if(g_partyConfirm, [sweepNow](auto const& e) { return e.second.second <= sweepNow; });
        }

        // Hold-and-retry: a reply is delivered the instant the recipient is
        // connected, and held for up to 30s if they're momentarily offline
        // (remote/flaky links blink in and out). Main thread only.
        static std::deque<std::pair<AiResult, time_t>> pending;
        time_t now = time(nullptr);

        // 1) pull newly-ready replies in; record each in history exactly once.
        // Living Chatter results are set aside and handed over AFTER the lock
        // is released (their delivery broadcasts packets); orders likewise -
        // their dispatch resolves players and calls into mod-playerbots.
        std::vector<AiResult> living;
        std::vector<AiResult> orders;
        {
            std::lock_guard<std::mutex> lk(g_outMtx);
            for (AiResult& r : g_out)
            {
                if (r.kind == AI_KIND_ORDER
                    || (r.kind == AI_KIND_DIRECTED && !r.order.empty()))
                {
                    orders.push_back(std::move(r));
                    continue;
                }
                if (r.kind == AI_KIND_AMBIENT || r.kind == AI_KIND_SCENE || r.kind == AI_KIND_SOCIAL)
                {
                    (r.kind == AI_KIND_AMBIENT ? g_livingLines
                                               : g_livingScenes) += 1;
                    living.push_back(std::move(r));
                    continue;
                }
                if (r.kind == AI_KIND_GREETING)
                    ++g_livingGreets;

                uint64 ak = AngerKey(r.bot.GetCounter(), r.player.GetCounter());
                // anti-loop: drop a reply that near-repeats one of this bot's
                // last two lines to the same player (g_history is main-thread
                // safe here; the player's line was already recorded at enqueue).
                // Greetings are exempt: a similar-sounding hello is fine, and
                // dropping one would leave the wave with no follow-up.
                if (CfgDedupe() && r.kind != AI_KIND_GREETING
                    && IsRepeatReply(ak, r.reply))
                {
                    LOG_INFO("server", "[aichat] dedupe dropped repeat from {} ('{}')",
                             r.botName, r.reply.substr(0, 40));
                    Stat(&AiStats::dedupeDrops);
                    continue;
                }
                std::string who = r.botName.empty() ? std::string("You") : r.botName;
                std::string line = who + ": " + r.reply;
                for (char& c : line) if (c == '\n' || c == '\r') c = ' ';   // keep one turn = one row
                auto& dq = g_history[ak];
                dq.push_back(line);
                TrimHistory(dq);
                SaveMemory(r.bot.GetCounter(), r.player.GetCounter());      // async upsert (history + anger)
                pending.emplace_back(r, now + 30);
            }
            g_out.clear();
        }

        for (AiResult const& r : living)
        {
            if (r.kind == AI_KIND_SOCIAL)
                WlSocialDeliver(r.channelId, r.bot, r.player, r.reply);
            else
                WlLivingDeliver(r.kind, r.bot, r.player, r.reply);
        }

        // talk-AND-command: dispatch each order, then whisper its ack via the
        // normal pending/TryDeliver path (skips history + dedupe on purpose -
        // orders are not conversation). A drain-time failure (logout, group
        // or master changed while queued) gets an honest refusal, never
        // silence - the whisper was already consumed away from AI chat.
        for (AiResult& r : orders)
        {
            // an EMPTY order is the queue-busy ack riding this dedupe-free
            // path: deliver the notice, dispatch nothing
            if (!r.order.empty())
            {
                if (DispatchBotOrder(r))
                    Stat(&AiStats::orders);
                else
                    // DispatchBotOrder already replaced r.reply with the
                    // honest refusal
                    LOG_DEBUG("server", "[aichat] order '{}' -> {} refused at drain",
                              r.order, r.botName);
            }
            // an AI-parsed order is part of the CONVERSATION: record what the
            // bot actually spoke so later chat continues coherently (slice-1
            // phrase orders stay out - canned acks aren't conversation)
            if (r.kind == AI_KIND_DIRECTED)
            {
                uint64 ak = AngerKey(r.bot.GetCounter(), r.player.GetCounter());
                std::string line = (r.botName.empty() ? std::string("You")
                                                      : r.botName) + ": " + r.reply;
                for (char& c : line)
                    if (c == '\n' || c == '\r')
                        c = ' ';
                auto& dq = g_history[ak];
                dq.push_back(line);
                TrimHistory(dq);
                SaveMemory(r.bot.GetCounter(), r.player.GetCounter());
            }
            pending.emplace_back(std::move(r), now + 30);
        }

        if (pending.empty()) return;

        // 2) deliver what we can; remove on success or after the hold window.
        for (auto it = pending.begin(); it != pending.end(); )
        {
            if (TryDeliver(it->first))
            {
                Stat(&AiStats::delivered);
                it = pending.erase(it);
            }
            else if (now >= it->second)
            {
                LOG_INFO("server", "[aichat] reply expired (recipient offline 30s): {} -> {}",
                         it->first.botName, it->first.player.ToString());
                it = pending.erase(it);
            }
            else
                ++it;
        }
    }
};

/* -------------------------------------------------------------------------- */
/*  .aichat stats/test - telemetry + Living Chatter trials for GMs             */
/* -------------------------------------------------------------------------- */
using namespace Acore::ChatCommands;

// wowlegends_livingchatter.cpp: force one ambient line / scene around the GM.
// Bypass the Enabled toggle and cooldowns (NOT the budget) for live trials.
bool WlLivingForceAmbient(Player* around);
bool WlLivingForceScene(Player* around);

class WowLegendsAiChatCommand : public CommandScript
{
public:
    WowLegendsAiChatCommand() : CommandScript("WowLegendsAiChatCommand") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable testSub =
        {
            { "ambient", HandleTestAmbient, SEC_GAMEMASTER, Console::No },
            { "scene",   HandleTestScene,   SEC_GAMEMASTER, Console::No },
        };
        static ChatCommandTable sub =
        {
            { "stats", HandleStats, SEC_GAMEMASTER, Console::Yes },
            { "test",  testSub },
        };
        static ChatCommandTable base =
        {
            { "aichat", sub },
        };
        return base;
    }

    static bool HandleTestAmbient(ChatHandler* handler)
    {
        if (WlLivingForceAmbient(handler->GetPlayer()))
            handler->SendSysMessage("주변 대화를 요청했습니다. 근처 봇이 잠시 후 말합니다.");
        else
            handler->SendSysMessage("근처에 가능한 봇이 없거나 AI 채팅이 꺼져 있거나 일일 대화 한도를 소진했습니다.");
        return true;
    }

    static bool HandleTestScene(ChatHandler* handler)
    {
        if (WlLivingForceScene(handler->GetPlayer()))
            handler->SendSysMessage("봇 간 대화를 요청했습니다. 근처 봇 두 명이 잠시 후 대화를 시작합니다.");
        else
            handler->SendSysMessage("서로 가까이 있는 봇 두 명이 필요합니다. AI 채팅 설정과 일일 한도도 확인하세요.");
        return true;
    }

    static bool HandleStats(ChatHandler* handler)
    {
        std::size_t queued;
        {
            std::lock_guard<std::mutex> lk(g_inMtx);
            queued = g_in.size();
        }
        handler->PSendSysMessage(
            "AI 채팅: 공급자 {} | 작업자 {} | 대기 {}/50 | 중복 방지 {}",
            CfgProvider(), g_workerCount.load(), uint32(queued),
            CfgDedupe() ? "on" : "off");

        auto line = [handler](char const* label, AiStats const& s)
        {
            std::string msg = Acore::StringFormat(
                "{}: requests {} | replies {} | delivered {} | drops "
                "queue {} / dup {} | failures {} | tokens in {} / out {}",
                label, s.requested.load(), s.replies.load(),
                s.delivered.load(), s.queueDrops.load(),
                s.dedupeDrops.load(), s.failures.load(),
                s.promptTokens.load(), s.replyTokens.load());
            // gated so disabled output stays byte-identical to pre-feature
            if (CfgAiCommandEnabled())
                msg += Acore::StringFormat(" | orders {} / parse-miss {}",
                    s.orders.load(), s.orderParseMiss.load());
            handler->SendSysMessage(msg.c_str());
        };
        line("Today", g_statsToday);
        line("Since start", g_stats);
        handler->PSendSysMessage(
            "자동 대화: {} | 오늘 요청 {} / 한도 {} | 토큰 {} | 발화 {} | 대화 장면 {} | AI 인사 {}",
            CfgLivingEnabled() ? "ON" : "off", g_livingReqToday.load(),
            CfgLivingBudget(), g_livingTokensToday.load(),
            g_livingLines.load(), g_livingScenes.load(),
            g_livingGreets.load());
        handler->SendSysMessage(
            "(토큰 수는 공급자가 보고한 사용량입니다. 호스팅 크레딧은 토큰이 아니라 응답마다 차감됩니다.)");
        return true;
    }
};

void AddWowLegendsAiChatScripts()
{
    new WowLegendsAiChatPlayer();
    new WowLegendsAiChatWorld();
    new WowLegendsAiChatCommand();
}
