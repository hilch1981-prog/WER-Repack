/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - personal AI Companion NPC
 *
 * Opt-in (NEVER auto): a player types `.companion create <race> <class> <name>`
 * to claim ONE permanent battle companion - a real bot that fights at their side
 * with the full mod-playerbots combat AI, and chats/remembers them via the
 * existing AI-chat + persistent-memory system (a companion is just a bound bot,
 * so AiChat and playerbot_ai_chat_memory already apply with ZERO extra wiring).
 *
 * How it works:
 *   - We pick a FREE character from the AddClass bot pool (accounts marked
 *     account_type = 2 in playerbots_account_type) matching the requested
 *     race + class + the owner's faction, that is offline, unguilded, not a
 *     loaded bot, and not the AHBot character.
 *   - We RENAME that offline pool char to the player's chosen name and record
 *     the binding (owner_guid -> bot_guid) in `wowlegends_companion`.
 *   - We summon it with the SAME path the proven gear module uses: dispatching
 *     a `.playerbots bot ...` chat command through the player's own session
 *     (ChatHandler::ParseCommands), so we stay decoupled from mod-playerbots
 *     headers/link order. `.playerbots bot` is SEC_PLAYER, so the player is
 *     allowed to drive it, and OnBotLogin auto-groups + follows + fights.
 *
 * Guards: AddClass-pool chars only; never the AHBot account/GUID; never an
 * online char; name must be valid + unique; cross-faction races rejected;
 * UNIQUE(bot_guid) makes a double-claim race lose gracefully (we retry another
 * pool char). One companion per player (PRIMARY KEY owner_guid).
 *
 * GUID-cap note: companions are strictly opt-in (one per player), so they are
 * naturally limited - no hard cap needed for v1.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Player.h"
#include "WorldSession.h"
#include "CharacterCache.h"
#include "ObjectMgr.h"
#include "World.h"
#include "ObjectGuid.h"
#include "DatabaseEnv.h"
#include "Configuration/Config.h"
#include "SharedDefines.h"
#include "Log.h"
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

// WOW Legends: companion long-term memory lives in wowlegends_companion_memory.cpp.
// Keep its bot->owner cache fresh when a companion is claimed or released.
void WlCompanionMemoryInvalidate(uint32 botGuid);

namespace
{
    // ---- account-type pool DB lives in the PLAYERBOTS database (playerbots_account_type),
    //      everything else (characters, our binding) lives in the CHARACTERS database. ----

    bool WlIsRealPlayer(Player* p)
    {
        return p && p->GetSession() && !p->GetSession()->IsBot();
    }

    // Lowercase a copy.
    std::string ToLower(std::string s)
    {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // class name -> id, mirroring mod-playerbots addclass (PlayerbotMgr.cpp:1097-1142).
    // 0 = unknown.
    uint8 ClassIdFromName(std::string const& nameRaw)
    {
        std::string n = ToLower(nameRaw);
        if (n == "warrior")              return 1;
        if (n == "paladin")              return 2;
        if (n == "hunter")               return 3;
        if (n == "rogue")                return 4;
        if (n == "priest")               return 5;
        if (n == "dk" || n == "deathknight" || n == "death_knight") return 6;
        if (n == "shaman")               return 7;
        if (n == "mage")                 return 8;
        if (n == "warlock")              return 9;
        if (n == "druid")                return 11;
        return 0;
    }

    // race name -> id (3.3.5a). 0 = unknown. Matches the aichat RaceFlavor switch.
    uint8 RaceIdFromName(std::string const& nameRaw)
    {
        std::string n = ToLower(nameRaw);
        if (n == "human")                       return 1;
        if (n == "orc")                         return 2;
        if (n == "dwarf")                       return 3;
        if (n == "nightelf" || n == "night_elf" || n == "nelf") return 4;
        if (n == "undead" || n == "forsaken")   return 5;
        if (n == "tauren")                      return 6;
        if (n == "gnome")                       return 7;
        if (n == "troll")                       return 8;
        if (n == "bloodelf" || n == "blood_elf" || n == "belf") return 10;
        if (n == "draenei")                     return 11;
        return 0;
    }

    char const* ClassDisplayName(uint8 c)
    {
        switch (c)
        {
            case 1:  return "전사";
            case 2:  return "성기사";
            case 3:  return "사냥꾼";
            case 4:  return "도적";
            case 5:  return "사제";
            case 6:  return "죽음의 기사";
            case 7:  return "주술사";
            case 8:  return "마법사";
            case 9:  return "흑마법사";
            case 11: return "드루이드";
            default: return "알 수 없음";
        }
    }

    char const* RaceDisplayName(uint8 r)
    {
        switch (r)
        {
            case 1:  return "인간";
            case 2:  return "오크";
            case 3:  return "드워프";
            case 4:  return "나이트 엘프";
            case 5:  return "언데드";
            case 6:  return "타우렌";
            case 7:  return "노움";
            case 8:  return "트롤";
            case 10: return "블러드 엘프";
            case 11: return "드레나이";
            default: return "알 수 없음";
        }
    }

    // Alliance race set, identical to RandomPlayerbotMgr::PrepareAddclassCache
    // (RandomPlayerbotMgr.cpp:1745): 1,3,4,7,11. Everything else is Horde.
    bool RaceIsAlliance(uint8 race)
    {
        return race == 1 || race == 3 || race == 4 || race == 7 || race == 11;
    }

    // The AHBot character/account to NEVER claim (config defaults 0 = none).
    uint32 CfgAhbotAccount() { return sConfigMgr->GetOption<uint32>("AuctionHouseBot.Account", 0); }
    uint32 CfgAhbotGuid()    { return sConfigMgr->GetOption<uint32>("AuctionHouseBot.GUID", 0); }

    // Use the same Unicode normalization and realm policy as character creation.
    bool NameLooksValid(std::string& name)
    {
        return normalizePlayerName(name) && ObjectMgr::CheckPlayerName(name, true) == CHAR_NAME_SUCCESS &&
            !sObjectMgr->IsReservedName(name);
    }

    // SQL-escape a player-supplied string for embedding in a quoted literal.
    std::string Escape(std::string s)
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

    // Case-insensitive: is this character name already taken in characters.name?
    bool NameTaken(std::string const& name)
    {
        QueryResult r = CharacterDatabase.Query(
            "SELECT 1 FROM characters WHERE LOWER(name) = LOWER('{}') LIMIT 1", Escape(name));
        return r != nullptr;
    }

    // Collect the account ids marked AddClass (account_type = 2) from the PLAYERBOTS db.
    // Mirrors RandomPlayerbotMgr::IsAccountType(acc, 2) / addClassTypeAccounts.
    std::string AddClassAccountIdCsv()
    {
        std::string csv;
        if (QueryResult r = PlayerbotsDatabase.Query(
                "SELECT account_id FROM playerbots_account_type WHERE account_type = 2"))
        {
            do
            {
                if (!csv.empty())
                    csv.push_back(',');
                csv += std::to_string(r->Fetch()[0].Get<uint32>());
            } while (r->NextRow());
        }
        return csv;
    }

    // Pick a free AddClass-pool character guid matching race+class+team, skipping
    // the just-tried guids (so a UNIQUE(bot_guid) collision can retry the next one).
    // Returns 0 if none available. All guards are applied in SQL:
    //   - account is AddClass-type (account ids resolved from the playerbots db)
    //   - matching race & class
    //   - offline (online = 0)
    //   - NOT in a guild (guildmember has no row)        -> excludes "real guild" members
    //   - NOT the AHBot character guid
    //   - NOT a guid we already failed to bind this call
    uint32 PickPoolChar(uint8 race, uint8 clazz, std::string const& addClassAccCsv,
                        uint32 ahbotGuid, std::vector<uint32> const& skip)
    {
        std::string skipCsv;
        for (uint32 g : skip)
        {
            if (!skipCsv.empty())
                skipCsv.push_back(',');
            skipCsv += std::to_string(g);
        }

        std::string sql =
            "SELECT c.guid FROM characters c "
            "WHERE c.account IN (" + addClassAccCsv + ") "
            "AND c.race = " + std::to_string(race) + " "
            "AND c.class = " + std::to_string(clazz) + " "
            "AND c.online = 0 "
            "AND c.guid NOT IN (SELECT guid FROM guild_member) ";
        if (ahbotGuid)
            sql += "AND c.guid <> " + std::to_string(ahbotGuid) + " ";
        if (!skipCsv.empty())
            sql += "AND c.guid NOT IN (" + skipCsv + ") ";
        sql += "LIMIT 1";

        if (QueryResult r = CharacterDatabase.Query("{}", sql))
            return r->Fetch()[0].Get<uint32>();
        return 0;
    }

    // Dispatch a `.playerbots bot <sub>` command through the OWNER's session,
    // exactly like wowlegends_gear.cpp drives `.playerbots bot initself`. The
    // playerbots command is SEC_PLAYER, so the owner is authorized; OnBotLogin
    // (mod-playerbots) auto-groups, follows and enables combat for `add`.
    void DispatchPlayerbots(Player* owner, std::string const& sub)
    {
        std::string command = ".playerbots bot " + sub;
        ChatHandler(owner->GetSession()).ParseCommands(command);
    }
}

/* -------------------------------------------------------------------------- */
/*  WorldScript: auto-create the binding table + startup integrity sweep        */
/* -------------------------------------------------------------------------- */
class WowLegendsCompanionWorld : public WorldScript
{
public:
    WowLegendsCompanionWorld() : WorldScript("WowLegendsCompanionWorld",
        { WORLDHOOK_ON_STARTUP }) { }

    // Auto-create the binding table on every start (idempotent, repack-default,
    // zero manual SQL), then drop any binding whose bot character no longer
    // exists (the AddClass pool was wiped / re-rolled). Mirrors the aichat
    // OnStartup DDL pattern (DirectExecute = documented one-time startup DDL).
    void OnStartup() override
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS wowlegends_companion ("
            "owner_guid INT UNSIGNED NOT NULL, "
            "bot_guid INT UNSIGNED NOT NULL, "
            "companion_name VARCHAR(24) NOT NULL, "
            "created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
            "PRIMARY KEY (owner_guid), "
            "UNIQUE KEY uk_bot_guid (bot_guid)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci");
        LOG_INFO("server", "[companion] wowlegends_companion table ready (personal companion bindings)");

        // Integrity sweep: delete bindings whose bot character is gone.
        CharacterDatabase.DirectExecute(
            "DELETE FROM wowlegends_companion WHERE bot_guid NOT IN (SELECT guid FROM characters)");
        LOG_INFO("server", "[companion] orphan-binding sweep complete");
    }
};

/* -------------------------------------------------------------------------- */
/*  CommandScript: .companion create / summon / dismiss / forget / (status)     */
/* -------------------------------------------------------------------------- */
class WowLegendsCompanionCommand : public CommandScript
{
public:
    WowLegendsCompanionCommand() : CommandScript("WowLegendsCompanionCommand") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable companionTable =
        {
            { "create",  HandleCompanionCreateCommand,  SEC_PLAYER, Console::No },
            { "summon",  HandleCompanionSummonCommand,  SEC_PLAYER, Console::No },
            { "dismiss", HandleCompanionDismissCommand, SEC_PLAYER, Console::No },
            { "forget",  HandleCompanionForgetCommand,  SEC_PLAYER, Console::No },
            { "",        HandleCompanionStatusCommand,  SEC_PLAYER, Console::No },
        };

        static ChatCommandTable baseTable =
        {
            { "companion", companionTable },
        };

        return baseTable;
    }

    // Read the caller's binding: returns true + fills out-params if one exists.
    static bool GetBinding(uint32 ownerGuid, uint32& botGuid, std::string& name)
    {
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT bot_guid, companion_name FROM wowlegends_companion WHERE owner_guid = {}", ownerGuid))
        {
            Field* f = r->Fetch();
            botGuid = f[0].Get<uint32>();
            name = f[1].Get<std::string>();
            return true;
        }
        return false;
    }

    /* ---- .companion (no arg): status ------------------------------------- */
    static bool HandleCompanionStatusCommand(ChatHandler* handler)
    {
        Player* me = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!WlIsRealPlayer(me))
            return false;

        uint32 botGuid = 0;
        std::string name;
        if (!GetBinding(me->GetGUID().GetCounter(), botGuid, name))
        {
            handler->SendSysMessage("동반자가 없습니다. |cffffff00.companion create <race> <class> <name>|r으로 생성하세요.");
            handler->SendSysMessage("종족: human orc dwarf nightelf undead tauren gnome troll bloodelf draenei. 직업: warrior paladin hunter rogue priest dk shaman mage warlock druid.");
            return true;
        }

        // Look up the companion's race/class from its character row for a richer status line.
        uint8 race = 0, clazz = 0;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT race, class FROM characters WHERE guid = {}", botGuid))
        {
            Field* f = r->Fetch();
            race = f[0].Get<uint8>();
            clazz = f[1].Get<uint8>();
        }

        handler->PSendSysMessage("동반자: |cff20ff20{}|r ({} {}).", name, RaceDisplayName(race), ClassDisplayName(clazz));
        handler->SendSysMessage("|cffffff00.companion summon|r으로 부르거나 |cffffff00.companion dismiss|r로 돌려보내세요. |cffffff00.companion forget|r은 영구 해제합니다.");
        return true;
    }

    /* ---- .companion create <race> <class> <name> ------------------------- */
    static bool HandleCompanionCreateCommand(ChatHandler* handler, char const* args)
    {
        Player* me = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!WlIsRealPlayer(me))
            return false;

        // a. one companion per player
        uint32 ownerGuid = me->GetGUID().GetCounter();
        uint32 existingBot = 0;
        std::string existingName;
        if (GetBinding(ownerGuid, existingBot, existingName))
        {
            handler->PSendSysMessage("이미 동반자 |cff20ff20{}|r이 있습니다. |cffffff00.companion summon|r으로 부르거나 |cffffff00.companion forget|r으로 먼저 해제하세요.", existingName);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // parse: <race> <class> <name>
        std::string raceStr, classStr, nameStr;
        {
            std::string a = args ? args : "";
            std::istringstream is(a);
            is >> raceStr >> classStr >> nameStr;
        }
        if (raceStr.empty() || classStr.empty() || nameStr.empty())
        {
            handler->SendSysMessage("사용법: .companion create <race> <class> <name>");
            handler->SendSysMessage("예: .companion create orc warrior 든든이");
            handler->SetSentErrorMessage(true);
            return false;
        }

        // b. parse race + class
        uint8 race = RaceIdFromName(raceStr);
        if (!race)
        {
            handler->PSendSysMessage("'{}' 종족을 찾을 수 없습니다. 사용 가능: human orc dwarf nightelf undead tauren gnome troll bloodelf draenei.", raceStr);
            handler->SetSentErrorMessage(true);
            return false;
        }
        uint8 clazz = ClassIdFromName(classStr);
        if (!clazz)
        {
            handler->PSendSysMessage("'{}' 직업을 찾을 수 없습니다. 사용 가능: warrior paladin hunter rogue priest dk shaman mage warlock druid.", classStr);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // c. race must match the owner's faction (no cross-faction companions)
        bool ownerAlliance = (me->GetTeamId(true) == TEAM_ALLIANCE);
        if (RaceIsAlliance(race) != ownerAlliance)
        {
            handler->PSendSysMessage("{} 종족은 {} 소속입니다. 동반자는 자신과 같은 진영({})이어야 합니다.",
                RaceDisplayName(race),
                RaceIsAlliance(race) ? "얼라이언스" : "호드",
                ownerAlliance ? "얼라이언스" : "호드");
            handler->SetSentErrorMessage(true);
            return false;
        }

        // d. validate name (2-12 letters) + uniqueness
        if (!NameLooksValid(nameStr))
        {
            handler->SendSysMessage("이름이 올바르지 않습니다. 서버의 캐릭터 이름 규칙에 맞는 한글 또는 영문 이름을 사용하세요.");
            handler->SetSentErrorMessage(true);
            return false;
        }
        // Normalize to WoW name casing (First letter upper, rest lower) BEFORE storing/looking up.
        // characters.name is case-sensitive (utf8mb4_bin) and the whisper system capitalizes the
        // target name, so a lowercase name like "compi" cannot be whispered. "compi" -> "Compi".
        // NameLooksValid already normalized UTF-8; never change individual bytes.

        if (NameTaken(nameStr))
        {
            handler->PSendSysMessage("'{}' 이름은 이미 사용 중입니다. 다른 이름을 선택하세요.", nameStr);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // e. find a free AddClass-pool char of that race+class+team
        std::string addClassAccCsv = AddClassAccountIdCsv();
        if (addClassAccCsv.empty())
        {
            handler->SendSysMessage("동반자 후보가 설정되어 있지 않습니다. 잠시 후 다시 시도하세요.");
            handler->SetSentErrorMessage(true);
            return false;
        }
        uint32 ahbotGuid = CfgAhbotGuid();

        // f/g. rename a pool char + insert the binding; UNIQUE(bot_guid) loses
        //      races gracefully -> on a duplicate, try the next pool char.
        std::vector<uint32> tried;
        uint32 botGuid = 0;
        bool bound = false;
        for (int attempt = 0; attempt < 8 && !bound; ++attempt)
        {
            botGuid = PickPoolChar(race, clazz, addClassAccCsv, ahbotGuid, tried);
            if (!botGuid)
                break;   // pool exhausted for this race+class
            tried.push_back(botGuid);

            // Claim the binding FIRST (UNIQUE bot_guid is the race guard).
            QueryResult dup = CharacterDatabase.Query(
                "SELECT 1 FROM wowlegends_companion WHERE bot_guid = {}", botGuid);
            if (dup)
                continue;   // already claimed by someone else; try another

            CharacterDatabase.DirectExecute(
                "INSERT INTO wowlegends_companion (owner_guid, bot_guid, companion_name) VALUES ({}, {}, '{}')",
                ownerGuid, botGuid, Escape(nameStr));

            // Confirm WE won the row (lost an owner_guid race? someone else inserted
            // our owner row; lost a bot_guid race? the insert silently no-op'd if it
            // collided — re-read to be sure this binding is ours).
            QueryResult check = CharacterDatabase.Query(
                "SELECT bot_guid FROM wowlegends_companion WHERE owner_guid = {}", ownerGuid);
            if (!check)
                continue;   // extremely unlikely; retry
            uint32 ownedBot = check->Fetch()[0].Get<uint32>();
            if (ownedBot != botGuid)
            {
                // We lost the owner_guid race to a parallel create; stop.
                handler->SendSysMessage("이미 동반자가 있습니다. |cffffff00.companion summon|r으로 부르세요.");
                handler->SetSentErrorMessage(true);
                return false;
            }
            bound = true;
        }

        if (!bound || !botGuid)
        {
            handler->PSendSysMessage("현재 {} {} 동반자 후보가 없습니다. 다른 종족이나 직업을 선택하거나 나중에 시도하세요.",
                RaceDisplayName(race), ClassDisplayName(clazz));
            handler->SetSentErrorMessage(true);
            return false;
        }

        // f (rename). Offline pool char -> safe direct rename. We MUST also update
        // the running CharacterCache so the new name resolves to this guid: the
        // summon below does `.playerbots bot add <name>`, which looks the bot up by
        // name (sCharacterCache->GetCharacterGuidByName). A raw UPDATE alone would
        // leave the cache stale and the immediate summon would fail to find the bot.
        ObjectGuid botObjGuid(HighGuid::Player, botGuid);
        CharacterDatabase.DirectExecute(
            "UPDATE characters SET name = '{}' WHERE guid = {}", Escape(nameStr), botGuid);
        sCharacterCache->UpdateCharacterData(botObjGuid, nameStr);

        // h. summon (OnBotLogin auto-groups + follow + combat)
        DispatchPlayerbots(me, "add " + nameStr);

        // i. confirm
        handler->PSendSysMessage("|cff20ff20동반자를 생성했습니다!|r {} ({} {})이 함께 싸웁니다. 귓속말로 대화하면 여러분을 기억합니다.",
            nameStr, RaceDisplayName(race), ClassDisplayName(clazz));
        handler->SendSysMessage("|cffffff00.companion dismiss|r / |cffffff00.companion summon|r으로 보내거나 부르세요. |cffffff00.companion forget|r은 영구 해제합니다.");
        WlCompanionMemoryInvalidate(botGuid);  // pool char is now a companion
        LOG_INFO("server", "[companion] {} (guid {}) created companion '{}' (botGuid {}, race {}, class {})",
            me->GetName(), ownerGuid, nameStr, botGuid, (uint32)race, (uint32)clazz);
        return true;
    }

    /* ---- .companion summon ---------------------------------------------- */
    static bool HandleCompanionSummonCommand(ChatHandler* handler)
    {
        Player* me = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!WlIsRealPlayer(me))
            return false;

        uint32 botGuid = 0;
        std::string name;
        if (!GetBinding(me->GetGUID().GetCounter(), botGuid, name))
        {
            handler->SendSysMessage("동반자가 없습니다. |cffffff00.companion create <race> <class> <name>|r으로 생성하세요.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        DispatchPlayerbots(me, "add " + name);
        handler->PSendSysMessage("동반자 |cff20ff20{}|r을 부릅니다.", name);
        return true;
    }

    /* ---- .companion dismiss --------------------------------------------- */
    static bool HandleCompanionDismissCommand(ChatHandler* handler)
    {
        Player* me = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!WlIsRealPlayer(me))
            return false;

        uint32 botGuid = 0;
        std::string name;
        if (!GetBinding(me->GetGUID().GetCounter(), botGuid, name))
        {
            handler->SendSysMessage("돌려보낼 동반자가 없습니다.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        DispatchPlayerbots(me, "remove " + name);
        handler->PSendSysMessage("|cff20ff20{}|r이 떠났습니다. |cffffff00.companion summon|r으로 언제든 다시 부를 수 있습니다.", name);
        return true;
    }

    /* ---- .companion forget ---------------------------------------------- */
    static bool HandleCompanionForgetCommand(ChatHandler* handler)
    {
        Player* me = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!WlIsRealPlayer(me))
            return false;

        uint32 ownerGuid = me->GetGUID().GetCounter();
        uint32 botGuid = 0;
        std::string name;
        if (!GetBinding(ownerGuid, botGuid, name))
        {
            handler->SendSysMessage("해제할 동반자가 없습니다.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        // Send it away first if it's out, then drop the binding + its chat memory.
        DispatchPlayerbots(me, "remove " + name);
        CharacterDatabase.Execute("DELETE FROM wowlegends_companion WHERE owner_guid = {}", ownerGuid);
        CharacterDatabase.Execute("DELETE FROM playerbot_ai_chat_memory WHERE bot_guid = {}", botGuid);
        CharacterDatabase.Execute("DELETE FROM wowlegends_companion_memory WHERE bot_guid = {}", botGuid);
        WlCompanionMemoryInvalidate(botGuid);  // pool char is free again

        handler->PSendSysMessage("|cff20ff20{}|r과의 동반자 관계를 해제했습니다. 기억과 관계가 사라지고 다른 플레이어의 후보로 돌아갑니다.", name);
        handler->SendSysMessage("|cffffff00.companion create <race> <class> <name>|r으로 새 동반자를 생성할 수 있습니다.");
        LOG_INFO("server", "[companion] {} (guid {}) forgot companion '{}' (botGuid {})",
            me->GetName(), ownerGuid, name, botGuid);
        return true;
    }
};

void AddWowLegendsCompanionScripts()
{
    new WowLegendsCompanionWorld();
    new WowLegendsCompanionCommand();
}
