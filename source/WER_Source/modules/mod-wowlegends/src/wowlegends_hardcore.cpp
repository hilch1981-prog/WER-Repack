/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - Hardcore mode + Mak'gora
 *
 * Hardcore = permanent death. When a hardcore character dies it becomes a
 * "fallen hero": kept on the account but locked (can't be played; a GM can
 * revive by clearing the `dead` flag). Two ways to be hardcore:
 *   - Realm-wide (owner forces it for everyone via conf), or
 *   - Per-character opt-in with `.hardcore on` (LEVEL 1 ONLY, permanent).
 *
 * Mak'gora = a consensual duel to the death between two hardcore players.
 * Both type `.makgora` (with the other targeted) to arm it; the next duel
 * within 30s is lethal — the loser falls. Normal duels never kill.
 *
 * Playerbots are excluded (so realm-wide mode doesn't massacre the bot pool).
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Player.h"
#include "Creature.h"
#include "SpellAuras.h"
#include "Duration.h"
#include "ScriptedGossip.h"
#include "GossipDef.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"
#include "Channel.h"
#include "ChannelMgr.h"
#include "DatabaseEnv.h"
#include "Configuration/Config.h"
#include "SharedDefines.h"
#include <ctime>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

using namespace Acore::ChatCommands;

// WOW Legends "Paths of Legends" (wowlegends_paths.cpp): the Herald of the
// Fallen also offers the Paths. One CreatureScript per NPC, so the Herald's
// gossip lives here and routes to these externs (action ids 200-399).
void WlPathsAddGossip(Player* player);
bool WlPathsGossipSelect(Player* player, uint32 action);
void WlPathsHeraldHonor(Player* player, Creature* herald);

namespace
{
    // challenger guid -> (target guid, expiry epoch). Mutual entries = armed Mak'gora.
    std::unordered_map<uint32, std::pair<uint32, time_t>> g_makgoraPending;
    // active duels flagged lethal, keyed by the sorted guid pair.
    std::set<std::pair<uint32, uint32>> g_lethalDuels;
    // transient "who killed me" for cause text, consumed on death.
    std::unordered_map<uint32, std::string> g_lastCause;
    // two-step confirm for the chat command: guid -> expiry epoch.
    std::unordered_map<uint32, time_t> g_hcConfirm;
    // in-memory "already fallen" guard (dedupes within a tick; DB write is async).
    std::set<uint32> g_fallen;

    bool CfgRealmWide() { return sConfigMgr->GetOption<bool>("WowLegends.Hardcore.RealmWide", false); }
    bool CfgAllowOptIn() { return sConfigMgr->GetOption<bool>("WowLegends.Hardcore.AllowOptIn", true); }
    bool CfgAnnounce() { return sConfigMgr->GetOption<bool>("WowLegends.Hardcore.AnnounceDeaths", true); }
    bool CfgMakgora() { return sConfigMgr->GetOption<bool>("WowLegends.Hardcore.Makgora", true); }
    // Optional visual aura for living hardcore characters. 0 = off (default).
    // Set any spell id with a persistent visual + relog to preview, if wanted later.
    uint32 CfgAuraSpell() { return sConfigMgr->GetOption<uint32>("WowLegends.Hardcore.AuraSpell", 0); }

    std::pair<uint32, uint32> SortedPair(uint32 a, uint32 b)
    {
        return (a < b) ? std::make_pair(a, b) : std::make_pair(b, a);
    }

    std::string SqlSafe(std::string s)
    {
        for (char& c : s)
            if (c == '\'' || c == '\\' || c == '`')
                c = ' ';
        return s;
    }

    bool WlIsRealPlayer(Player* p)
    {
        return p && p->GetSession() && !p->GetSession()->IsBot();
    }

    void Announce(std::string const& msg)
    {
        for (auto const& itr : sWorldSessionMgr->GetAllSessions())
            if (WorldSession* s = itr.second)
                if (s->GetPlayer())
                    ChatHandler(s).PSendSysMessage("{}", msg);
    }

    bool RowFlag(uint32 guid, char const* column)
    {
        if (QueryResult r = CharacterDatabase.Query("SELECT `{}` FROM wowlegends_hardcore WHERE guid={}", column, guid))
            return r->Fetch()[0].Get<uint8>() != 0;
        return false;
    }

    bool IsHardcore(Player* p)
    {
        if (!WlIsRealPlayer(p))
            return false;
        if (CfgRealmWide())
            return true;
        return RowFlag(p->GetGUID().GetCounter(), "enabled");
    }

    bool IsFallen(Player* p)
    {
        if (!p)
            return false;
        return RowFlag(p->GetGUID().GetCounter(), "dead");
    }

    // Mark a hardcore character as fallen: persist, announce, lock (kick).
    void MarkFallen(Player* p, std::string const& cause)
    {
        if (!WlIsRealPlayer(p))
            return;

        uint32 guid = p->GetGUID().GetCounter();
        g_fallen.insert(guid);
        uint8 lvl = p->GetLevel();
        uint32 now = static_cast<uint32>(time(nullptr));
        std::string safeCause = SqlSafe(cause);

        CharacterDatabase.Execute(
            "INSERT INTO wowlegends_hardcore (guid,enabled,dead,death_level,death_cause,death_time) "
            "VALUES ({},1,1,{},'{}',{}) "
            "ON DUPLICATE KEY UPDATE enabled=1,dead=1,death_level={},death_cause='{}',death_time={}",
            guid, lvl, safeCause, now, lvl, safeCause, now);

        if (CfgAnnounce())
            Announce("|cffff2020[Hardcore]|r " + p->GetName() + " has fallen at level " +
                     std::to_string(lvl) + " - " + cause + ".");

        // No kick/logout here (that crashes mid-update, and is jarring). The hero
        // simply stays dead: OnPlayerCanResurrect blocks every revive path, so the
        // fallen character lingers as a permanent ghost that can never come back.
        ChatHandler(p->GetSession()).PSendSysMessage(
            "|cffff2020하드코어 여정이 끝났습니다.|r {}레벨에 쓰러졌습니다({}). 죽음은 영구적이며 이 캐릭터는 부활할 수 없습니다.",
            lvl, cause);
    }

    // "" = eligible to enable hardcore; otherwise a human-readable reason why not.
    std::string HardcoreIneligibleReason(Player* p)
    {
        if (p->GetSession()->GetSessionDbLocaleIndex() == LOCALE_koKR)
        {
            if (CfgRealmWide()) return "이 서버는 모든 캐릭터에 하드코어 모드가 적용됩니다.";
            if (!CfgAllowOptIn()) return "이 서버에서는 하드코어 모드를 선택할 수 없습니다.";
            if (IsFallen(p)) return "이미 사망한 하드코어 캐릭터입니다.";
            if (IsHardcore(p)) return "이미 하드코어 모드가 활성화되어 있습니다.";
            if (p->GetLevel() != 1) return "하드코어 모드는 모험을 시작하기 전, 1레벨에서만 선택할 수 있습니다.";
            return "";
        }
        if (CfgRealmWide())   return "This realm is fully hardcore already - every character is hardcore.";
        if (!CfgAllowOptIn()) return "Hardcore opt-in is disabled on this realm.";
        if (IsFallen(p))      return "This character has already fallen.";
        if (IsHardcore(p))    return "You are already hardcore.";
        if (p->GetLevel() != 1) return "Hardcore can only be enabled at level 1, before your journey begins.";
        return "";
    }

    // Apply the persistent hardcore visual aura (visible to the player and others).
    void ApplyHardcoreAura(Player* p)
    {
        uint32 sp = CfgAuraSpell();
        if (!sp || p->HasAura(sp))
            return;
        if (Aura* a = p->AddAura(sp, p))
            a->SetDuration(-1);   // permanent (no expiry)
    }

    // Flip the character to hardcore (assumes eligibility already checked).
    void DoEnableHardcore(Player* p)
    {
        CharacterDatabase.Execute("INSERT INTO wowlegends_hardcore (guid,enabled,dead) VALUES ({},1,0) "
                                  "ON DUPLICATE KEY UPDATE enabled=1", p->GetGUID().GetCounter());
        ApplyHardcoreAura(p);
        ChatHandler(p->GetSession()).PSendSysMessage(
            "|cff20ff20하드코어 모드가 활성화되었습니다.|r 이제 죽음은 영구적이며 되돌릴 수 없습니다. 행운을 빕니다, {}.",
            p->GetName());
        if (CfgAnnounce())
            Announce("|cff20ff20[Hardcore]|r " + p->GetName() + " has embraced hardcore mode!");
    }
}

/* -------------------------------------------------------------------------- */
/*  PlayerScript: death, login, duels                                          */
/* -------------------------------------------------------------------------- */
class WowLegendsHardcorePlayer : public PlayerScript
{
public:
    WowLegendsHardcorePlayer() : PlayerScript("WowLegendsHardcorePlayer", {
        PLAYERHOOK_ON_PLAYER_JUST_DIED,
        PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
        PLAYERHOOK_ON_PVP_KILL,
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_DELETE,
        PLAYERHOOK_CAN_RESURRECT,
        PLAYERHOOK_ON_DUEL_START,
        PLAYERHOOK_ON_DUEL_END
    }) { }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (killer && killed)
            g_lastCause[killed->GetGUID().GetCounter()] = "slain by " + killer->GetName();
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (killer && killed)
            g_lastCause[killed->GetGUID().GetCounter()] = "slain in battle by " + killer->GetName();
    }

    // Block every resurrection path (corpse reclaim, run-back, spirit healer) for a fallen hero.
    bool OnPlayerCanResurrect(Player* player) override
    {
        return !(WlIsRealPlayer(player) && (g_fallen.count(player->GetGUID().GetCounter()) || IsFallen(player)));
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!IsHardcore(player) || g_fallen.count(player->GetGUID().GetCounter()) || IsFallen(player))
            return;

        std::string cause = "the world";
        auto it = g_lastCause.find(player->GetGUID().GetCounter());
        if (it != g_lastCause.end())
        {
            cause = it->second;
            g_lastCause.erase(it);
        }
        MarkFallen(player, cause);
    }

    void OnPlayerLogin(Player* player) override
    {
        if (!WlIsRealPlayer(player))
            return;

        // Public channel membership is per session; rejoin on login without changing permissions.
        if (sConfigMgr->GetOption<bool>("WowLegends.Chat.AutoJoinWorld", false))
            if (ChannelMgr* manager = ChannelMgr::forTeam(player->GetTeamId()))
                if (Channel* channel = manager->GetJoinChannel("World", 0))
                    channel->JoinChannel(player, "");

        // Realm-wide: ensure a row exists so death-tracking works from level 1.
        if (CfgRealmWide())
            CharacterDatabase.Execute("INSERT IGNORE INTO wowlegends_hardcore (guid,enabled,dead) VALUES ({},1,0)",
                                      player->GetGUID().GetCounter());

        if (IsFallen(player))
        {
            g_fallen.insert(player->GetGUID().GetCounter());   // ensure rez stays blocked this session
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffff2020이 캐릭터는 전사했습니다.|r 영혼은 남아 있지만 부활할 수 없습니다.");
            return;
        }

        if (IsHardcore(player))
        {
            ApplyHardcoreAura(player);   // re-apply the visible marker each login
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff20ff20하드코어 모드가 적용 중입니다.|r 죽으면 부활할 수 없으니 신중하게 싸우세요.");
        }
    }

    // Clean up the hardcore record when a character is deleted, so a future
    // character that reuses this GUID never inherits a stale dead/enabled flag.
    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        CharacterDatabase.Execute("DELETE FROM wowlegends_hardcore WHERE guid={}", guid.GetCounter());
    }

    void OnPlayerDuelStart(Player* player1, Player* player2) override
    {
        if (!CfgMakgora() || !WlIsRealPlayer(player1) || !WlIsRealPlayer(player2))
            return;

        uint32 g1 = player1->GetGUID().GetCounter();
        uint32 g2 = player2->GetGUID().GetCounter();
        time_t now = time(nullptr);

        auto a = g_makgoraPending.find(g1);
        auto b = g_makgoraPending.find(g2);
        bool mutual = a != g_makgoraPending.end() && b != g_makgoraPending.end() &&
                      a->second.first == g2 && b->second.first == g1 &&
                      a->second.second >= now && b->second.second >= now;

        if (!mutual)
            return;

        g_lethalDuels.insert(SortedPair(g1, g2));
        g_makgoraPending.erase(g1);
        g_makgoraPending.erase(g2);

        ChatHandler(player1->GetSession()).PSendSysMessage("|cffff2020막고라!|r 죽음으로 끝나는 결투입니다. 항복할 수 없습니다.");
        ChatHandler(player2->GetSession()).PSendSysMessage("|cffff2020막고라!|r 죽음으로 끝나는 결투입니다. 항복할 수 없습니다.");
    }

    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override
    {
        if (!winner || !loser)
            return;

        auto key = SortedPair(winner->GetGUID().GetCounter(), loser->GetGUID().GetCounter());
        auto it = g_lethalDuels.find(key);
        if (it == g_lethalDuels.end())
            return;

        g_lethalDuels.erase(it);

        if (type == DUEL_WON)
        {
            MarkFallen(loser, "slain in Mak'gora by " + winner->GetName());
            // The duel restores the loser to 1 HP AFTER this hook, so a kill here is undone.
            // Defer to the next tick + use KillSelf (full kill: zeroes health, real dead state)
            // instead of KillPlayer (which only flips the state and leaves them at 1 HP).
            loser->m_Events.AddEventAtOffset([loser]() { loser->KillSelf(); }, Milliseconds(1));
        }
        else
        {
            ChatHandler(winner->GetSession()).PSendSysMessage("사망자 없이 막고라가 끝났습니다.");
            ChatHandler(loser->GetSession()).PSendSysMessage("사망자 없이 막고라가 끝났습니다.");
        }
    }
};

/* -------------------------------------------------------------------------- */
/*  CommandScript: .hardcore on / status, .makgora                             */
/* -------------------------------------------------------------------------- */
class WowLegendsHardcoreCommand : public CommandScript
{
public:
    WowLegendsHardcoreCommand() : CommandScript("WowLegendsHardcoreCommand") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable hardcoreTable =
        {
            { "on",     HandleHardcoreOnCommand,     SEC_PLAYER, Console::No },
            { "status", HandleHardcoreStatusCommand, SEC_PLAYER, Console::No },
        };

        static ChatCommandTable baseTable =
        {
            { "hardcore", hardcoreTable },
            { "makgora",  HandleMakgoraCommand, SEC_PLAYER, Console::No },
        };

        return baseTable;
    }

    static bool HandleHardcoreOnCommand(ChatHandler* handler)
    {
        Player* me = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!me)
            return false;

        std::string reason = HardcoreIneligibleReason(me);
        if (!reason.empty())
        {
            handler->SendSysMessage(reason.c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        // two-step confirm (the 3.3.5 client has no Accept/Decline popup for chat commands;
        // for the real dialog, talk to the Herald of the Fallen NPC at your starting zone).
        uint32 g = me->GetGUID().GetCounter();
        time_t now = time(nullptr);
        auto it = g_hcConfirm.find(g);
        if (it == g_hcConfirm.end() || it->second < now)
        {
            g_hcConfirm[g] = now + 30;
            handler->SendSysMessage("|cffff2020경고:|r 하드코어는 되돌릴 수 없습니다. 죽으면 이 캐릭터로 다시 플레이할 수 없습니다.");
            handler->SendSysMessage("확정하려면 30초 안에 |cffffff00.hardcore on|r을 다시 입력하거나 시작 지역의 몰락자의 전령과 대화하세요.");
            return true;
        }

        g_hcConfirm.erase(g);
        DoEnableHardcore(me);
        return true;
    }

    static bool HandleHardcoreStatusCommand(ChatHandler* handler)
    {
        Player* me = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!me)
            return false;

        if (IsFallen(me))
            handler->SendSysMessage("하드코어 상태: |cffff2020전사|r - 사망한 캐릭터입니다.");
        else if (IsHardcore(me))
            handler->PSendSysMessage("하드코어 상태: |cff20ff20활성|r - {}레벨, 생존 중. 사망 시 부활할 수 없습니다.", me->GetLevel());
        else
            handler->SendSysMessage("하드코어 상태: 일반. 1레벨에 .hardcore on으로 시작할 수 있습니다.");
        return true;
    }

    static bool HandleMakgoraCommand(ChatHandler* handler)
    {
        if (!CfgMakgora())
        {
            handler->SendSysMessage("이 서버에서는 막고라가 비활성화되어 있습니다.");
            return true;
        }

        Player* me = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        Player* target = handler->getSelectedPlayer();
        if (!me)
            return false;
        if (!target || target == me)
        {
            handler->SendSysMessage("도전할 하드코어 플레이어를 선택하고 .makgora를 입력하세요.");
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (!IsHardcore(me) || !IsHardcore(target))
        {
            handler->SendSysMessage("자신과 대상 모두 하드코어 캐릭터여야 합니다.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 myGuid = me->GetGUID().GetCounter();
        uint32 tGuid = target->GetGUID().GetCounter();
        time_t now = time(nullptr);

        g_makgoraPending[myGuid] = { tGuid, now + 30 };

        auto it = g_makgoraPending.find(tGuid);
        bool mutual = it != g_makgoraPending.end() && it->second.first == myGuid && it->second.second >= now;

        if (mutual)
        {
            ChatHandler(me->GetSession()).PSendSysMessage("|cffff8000{}와 막고라가 성립됐습니다!|r 30초 안에 시작하는 다음 결투는 죽음으로 끝납니다.", target->GetName());
            ChatHandler(target->GetSession()).PSendSysMessage("|cffff8000{}와 막고라가 성립됐습니다!|r 30초 안에 시작하는 다음 결투는 죽음으로 끝납니다.", me->GetName());
        }
        else
        {
            ChatHandler(me->GetSession()).PSendSysMessage("{}에게 막고라를 신청했습니다. 상대가 30초 안에 자신을 선택하고 .makgora를 입력하면 수락됩니다.", target->GetName());
            ChatHandler(target->GetSession()).PSendSysMessage("|cffff8000{}이 죽음의 결투인 막고라를 신청했습니다!|r 상대를 선택하고 .makgora를 입력하면 수락됩니다.", me->GetName());
        }
        return true;
    }
};

/* -------------------------------------------------------------------------- */
/*  CreatureScript: "Herald of the Fallen" — opt in via a confirm dialog       */
/* -------------------------------------------------------------------------- */
class npc_wowlegends_hardcore : public CreatureScript
{
public:
    npc_wowlegends_hardcore() : CreatureScript("npc_wowlegends_hardcore") { }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        if (!WlIsRealPlayer(player))
            return true;

        ClearGossipMenuFor(player);
        std::string reason = HardcoreIneligibleReason(player);
        if (reason.empty())
        {
            // eligible -> offer with a real Accept/Decline confirmation box
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                "각오했습니다. 죽음을 영구적으로 받아들이겠습니다.",
                GOSSIP_SENDER_MAIN, 1,
                "하드코어는 영구 적용됩니다. 이 캐릭터가 죽으면 다시 부활할 수 없습니다. 정말 받아들이시겠습니까?",
                0, false);
        }
        else
        {
            // not eligible -> just show why (no actionable option)
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, reason, GOSSIP_SENDER_MAIN, 99);
        }
        WlPathsAddGossip(player);   // the Herald also keeps the Paths
        WlPathsHeraldHonor(player, creature);   // he bows to the fulfilled
        SendGossipMenuFor(player, 990000, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* /*creature*/, uint32 /*sender*/, uint32 action) override
    {
        CloseGossipMenuFor(player);
        if (WlPathsGossipSelect(player, action))
            return true;
        if (action == 1)
        {
            std::string reason = HardcoreIneligibleReason(player);
            if (reason.empty())
                DoEnableHardcore(player);
            else
                ChatHandler(player->GetSession()).PSendSysMessage("{}", reason);
        }
        return true;
    }
};

void AddWowLegendsHardcoreScripts()
{
    new WowLegendsHardcorePlayer();
    new WowLegendsHardcoreCommand();
    new npc_wowlegends_hardcore();
}
