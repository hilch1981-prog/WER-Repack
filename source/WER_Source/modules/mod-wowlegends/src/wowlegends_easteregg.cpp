/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - Easter egg: "Don't Make Me Get My Main"
 *
 * A tribute to Cranius' Blizzcon-2009-winning machinima. At the song's
 * filming spot in Stranglethorn Vale, Salandrine <The Camper> (a Blood
 * Knight) loiters by her campfire on the roadside - laughing and taunting -
 * while Morticus <Her Main> (a level 80 UNDEAD rogue, as in the video) waits
 * COMPLETELY UNSEEN on the far side. When a REAL Alliance player comes close,
 * Salandrine attacks and taunts him - moments later the main materializes
 * with a Cheap Shot. After a kill he Cannibalizes the
 * corpse, machinima-accurate. Horde players just see a suspicious blood elf
 * enjoying the jungle.
 *
 * Both NPCs ship as faction 35 (friendly to all) so nothing auto-aggros
 * either way; the ambush is 100% script-driven (SetFaction + AttackStart),
 * which is what makes it fire REGARDLESS of PvP flags or the World PvP
 * mode - camping is a lifestyle, not a rule set. Playerbots never trigger
 * it (real sessions only), so the pair won't grind the bot population.
 *
 * The rogue is hidden with SetVisible(false) - truly unseen, not mere stealth
 * that can be detected - and opens with a TRIGGERED Cheap Shot so the stun
 * always lands the instant he appears (no broken-stealth opener race).
 * His daggers are set on him so they show the moment he reveals.
 *
 * Thread model: all state is per-creature-instance (EventMap + members) -
 * MoveInLineOfSight/UpdateAI run on the creature's own map update, so no
 * shared state and no locking. The toggle is a cached atomic.
 *
 * Config: WowLegends.EasterEgg.Enabled (default 1). Disabled = the two
 * just stand there, harmless and mysterious (campfire included).
 */

#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "WorldSession.h"
#include "SharedDefines.h"
#include "Random.h"
#include "Configuration/Config.h"
#include <atomic>

namespace
{
    std::atomic<bool> g_eggEnabled{true};

    constexpr float  TRIGGER_RANGE = 14.0f;
    constexpr uint32 FACTION_AMBUSH = 14;      // hostile monster (fight back!)

    constexpr uint32 SPELL_JUDGEMENT   = 20271;
    constexpr uint32 SPELL_HAMMER_JUST = 853;
    constexpr uint32 SPELL_HOLY_LIGHT  = 635;
    constexpr uint32 SPELL_CHEAP_SHOT  = 30986;   // stealth opener, triggered
    constexpr uint32 SPELL_SINISTER    = 14873;
    constexpr uint32 SPELL_GOUGE       = 12540;
    constexpr uint32 SPELL_CANNIBALIZE = 20577;   // the undead racial

    constexpr uint32 DAGGER_ITEM = 3184;          // Hook Dagger, both hands

    // Salandrine jeers between swings - the song's whole vibe.
    constexpr char const* PALADIN_TAUNTS[] =
    {
        "Aww, is the little Alliance lost?",
        "Should've stayed in Elwynn!",
        "This is MY road, sweetie.",
        "Swing harder - it tickles!",
    };
    constexpr uint32 PALADIN_TAUNT_COUNT = 4;

    enum EggEvents
    {
        EVENT_UNCLOAK = 1,
        EVENT_JUDGEMENT,
        EVENT_HAMMER,
        EVENT_HEAL,
        EVENT_TAUNT,
        EVENT_SINISTER,
        EVENT_GOUGE,
        EVENT_FEAST_END,
    };

    // A REAL Alliance player (no bots - 10000 of them would farm the poor
    // camper into the ground; the joke is for humans).
    bool IsAmbushTarget(Unit* who)
    {
        Player* p = who->ToPlayer();
        return p && p->GetSession() && !p->GetSession()->IsBot()
            && p->GetTeamId() == TEAM_ALLIANCE && p->IsAlive()
            && !p->IsGameMaster();
    }
}

class WowLegendsEasterEggWorld : public WorldScript
{
public:
    WowLegendsEasterEggWorld() : WorldScript("WowLegendsEasterEggWorld",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_eggEnabled = sConfigMgr->GetOption<bool>(
            "WowLegends.EasterEgg.Enabled", true);
    }
};

/* -------------------------------------------------------------------------- */
/*  Salandrine <The Camper> - tends her fire, engages on sight, jeers away     */
/* -------------------------------------------------------------------------- */
class npc_wl_camper_paladin : public CreatureScript
{
public:
    npc_wl_camper_paladin() : CreatureScript("npc_wl_camper_paladin") { }

    struct npc_wl_camper_paladinAI : public ScriptedAI
    {
        npc_wl_camper_paladinAI(Creature* creature) : ScriptedAI(creature) { }

        void Reset() override
        {
            _events.Reset();
            me->SetFaction(me->GetCreatureTemplate()->faction);
        }

        void MoveInLineOfSight(Unit* who) override
        {
            if (!g_eggEnabled.load() || me->IsInCombat())
                return;
            if (!IsAmbushTarget(who)
                || !me->IsWithinDistInMap(who, TRIGGER_RANGE)
                || !me->IsWithinLOSInMap(who))
                return;

            me->Say("여기는 네가 올 만한 정글이 아니다.", LANG_UNIVERSAL);
            me->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH);
            me->SetFaction(FACTION_AMBUSH);
            AttackStart(who);
        }

        void JustEngagedWith(Unit* /*who*/) override
        {
            _events.ScheduleEvent(EVENT_JUDGEMENT, 4s);
            _events.ScheduleEvent(EVENT_TAUNT, 6s);
            _events.ScheduleEvent(EVENT_HAMMER, 8s);
            _events.ScheduleEvent(EVENT_HEAL, 12s);
        }

        void UpdateAI(uint32 diff) override
        {
            if (!UpdateVictim())
                return;

            _events.Update(diff);
            while (uint32 eventId = _events.ExecuteEvent())
            {
                switch (eventId)
                {
                    case EVENT_TAUNT:
                    {
                        uint32 i = urand(0, PALADIN_TAUNT_COUNT - 1);
                        me->Say(PALADIN_TAUNTS[i], LANG_UNIVERSAL);
                        me->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH);
                        _events.ScheduleEvent(EVENT_TAUNT, 10s);
                        break;
                    }
                    case EVENT_JUDGEMENT:
                        DoCastVictim(SPELL_JUDGEMENT);
                        _events.ScheduleEvent(EVENT_JUDGEMENT, 9s);
                        break;
                    case EVENT_HAMMER:
                        DoCastVictim(SPELL_HAMMER_JUST);
                        _events.ScheduleEvent(EVENT_HAMMER, 14s);
                        break;
                    case EVENT_HEAL:
                        if (me->GetHealthPct() < 45.0f)
                            DoCastSelf(SPELL_HOLY_LIGHT);
                        _events.ScheduleEvent(EVENT_HEAL, 12s);
                        break;
                    default:
                        break;
                }
            }
            DoMeleeAttackIfReady();
        }

    private:
        EventMap _events;
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_wl_camper_paladinAI(creature);
    }
};

/* -------------------------------------------------------------------------- */
/*  Morticus <Her Main> - completely unseen; materializes 2.5s after a mark    */
/*  strays into the pocket, opens with a Cheap Shot. Level 80. You were told.   */
/* -------------------------------------------------------------------------- */
class npc_wl_camper_rogue : public CreatureScript
{
public:
    npc_wl_camper_rogue() : CreatureScript("npc_wl_camper_rogue") { }

    struct npc_wl_camper_rogueAI : public ScriptedAI
    {
        npc_wl_camper_rogueAI(Creature* creature) : ScriptedAI(creature) { }

        void Reset() override
        {
            _events.Reset();
            _mark.Clear();
            _feasting = false;
            me->SetFaction(me->GetCreatureTemplate()->faction);
            me->SetVirtualItem(0, DAGGER_ITEM);    // main hand
            me->SetVirtualItem(1, DAGGER_ITEM);    // off hand
            me->SetVisible(false);                 // unseen until he strikes
        }

        // The canon beat: after killing a player he eats them, exactly like
        // the machinima. The feast suppresses the evade-home for its
        // duration; taking damage interrupts it (someone always objects).
        void KilledUnit(Unit* victim) override
        {
            if (!victim->IsPlayer() || _feasting)
                return;
            _feasting = true;
            _events.Reset();
            me->StopMoving();
            me->AttackStop();
            if (me->CastSpell(me, SPELL_CANNIBALIZE, false) != SPELL_CAST_OK)
                me->CastSpell(me, SPELL_CANNIBALIZE, true);
            _events.ScheduleEvent(EVENT_FEAST_END, 11s);
        }

        void EnterEvadeMode(EvadeReason why) override
        {
            if (_feasting)
                return;                    // dinner first
            ScriptedAI::EnterEvadeMode(why);
        }

        void DamageTaken(Unit* /*attacker*/, uint32& damage,
            DamageEffectType /*type*/, SpellSchoolMask /*mask*/) override
        {
            if (_feasting && damage > 0)
            {
                _feasting = false;
                me->InterruptNonMeleeSpells(false);
            }
        }

        void MoveInLineOfSight(Unit* who) override
        {
            if (!g_eggEnabled.load() || me->IsInCombat() || _mark)
                return;
            if (!IsAmbushTarget(who)
                || !me->IsWithinDistInMap(who, TRIGGER_RANGE)
                || !me->IsWithinLOSInMap(who))
                return;

            _mark = who->GetGUID();
            _events.ScheduleEvent(EVENT_UNCLOAK, 2500ms);   // the reveal
        }

        void JustEngagedWith(Unit* /*who*/) override
        {
            _events.ScheduleEvent(EVENT_SINISTER, 3s);
            _events.ScheduleEvent(EVENT_GOUGE, 9s);
        }

        void UpdateAI(uint32 diff) override
        {
            _events.Update(diff);

            if (_feasting)
            {
                if (_events.ExecuteEvent() == EVENT_FEAST_END)
                {
                    _feasting = false;
                    EnterEvadeMode(EVADE_REASON_OTHER);   // back to the shadows
                }
                return;
            }

            if (!me->IsInCombat())
            {
                if (_events.ExecuteEvent() == EVENT_UNCLOAK)
                {
                    Player* mark = ObjectAccessor::GetPlayer(*me, _mark);
                    _mark.Clear();
                    if (!mark || !mark->IsAlive()
                        || !me->IsWithinDistInMap(mark, 30.0f))
                        return;

                    me->SetVisible(true);          // materialize
                    me->Say("그녀는 혼자 사냥하지 않는다.", LANG_UNIVERSAL);
                    me->SetFaction(FACTION_AMBUSH);
                    // triggered: bypasses the "requires stealth" opener rule,
                    // so the Cheap Shot stun always lands on the reveal
                    DoCast(mark, SPELL_CHEAP_SHOT, true);
                    AttackStart(mark);
                }
                return;
            }

            if (!UpdateVictim())
                return;

            while (uint32 eventId = _events.ExecuteEvent())
            {
                switch (eventId)
                {
                    case EVENT_SINISTER:
                        DoCastVictim(SPELL_SINISTER);
                        _events.ScheduleEvent(EVENT_SINISTER, 6s);
                        break;
                    case EVENT_GOUGE:
                        DoCastVictim(SPELL_GOUGE);
                        _events.ScheduleEvent(EVENT_GOUGE, 15s);
                        break;
                    default:
                        break;
                }
            }
            DoMeleeAttackIfReady();
        }

    private:
        EventMap _events;
        ObjectGuid _mark;
        bool _feasting = false;
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_wl_camper_rogueAI(creature);
    }
};

void AddWowLegendsEasterEggScripts()
{
    new WowLegendsEasterEggWorld();
    new npc_wl_camper_paladin();
    new npc_wl_camper_rogue();
}
