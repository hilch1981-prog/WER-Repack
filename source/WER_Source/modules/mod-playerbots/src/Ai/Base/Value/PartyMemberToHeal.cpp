/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "PartyMemberToHeal.h"
#include "Playerbots.h"
#include "ServerFacade.h"

// WL "Triage Healer": provided by mod-wowlegends (wowlegends_botflags.cpp).
// Off => exact upstream behaviour (protect value stays stubbed, heal picker
// stays pure lowest-HP).
extern bool WlTriageHealerEnabled();

class IsTargetOfHealingSpell : public SpellEntryPredicate
{
public:
    bool Check(SpellInfo const* spellInfo) override
    {
        for (uint8 i = 0; i < 3; ++i)
        {
            if (spellInfo->Effects[i].Effect == SPELL_EFFECT_HEAL ||
                spellInfo->Effects[i].Effect == SPELL_EFFECT_HEAL_MAX_HEALTH ||
                spellInfo->Effects[i].Effect == SPELL_EFFECT_HEAL_MECHANICAL)
                return true;
        }

        return false;
    }
};

inline bool compareByHealth(Unit const* u1, Unit const* u2) { return u1->GetHealthPct() < u2->GetHealthPct(); }

Unit* PartyMemberToHeal::Calculate()
{
    IsTargetOfHealingSpell predicate;

    Group* group = bot->GetGroup();
    if (!group)
        return bot;

    bool isRaid = bot->GetGroup()->isRaidGroup();
    MinValueCalculator calc(100);
    // WL triage: the human we follow (and the tank) get topped off before an
    // equal-health DPS. Lower probeValue = higher priority.
    Player* const master = botAI->GetMaster();

    // If focus heal targets strategy is active, only heal those targets
    if (botAI->HasStrategy("focus heal targets", BOT_STATE_COMBAT))
    {
        std::list<ObjectGuid> const focusHealTargets =
            AI_VALUE(std::list<ObjectGuid>, "focus heal targets");

        for (ObjectGuid const& focusHealTarget : focusHealTargets)
        {
            Player* player = ObjectAccessor::FindPlayer(focusHealTarget);
            if (!player || !player->IsInWorld() || !player->IsAlive() || !player->IsInSameGroupWith(bot))
                continue;

            float health = player->GetHealthPct();
            if (isRaid || health < sPlayerbotAIConfig.mediumHealth ||
                !IsTargetOfSpellCast(player, predicate))
            {
                float probeValue = 100.0f;
                if (player->GetDistance2d(bot) > sPlayerbotAIConfig.healDistance)
                    probeValue = health + 30.0f;
                else
                    probeValue = health + player->GetDistance2d(bot) / 10.0f;

                if (probeValue < calc.minValue && Check(player))
                    calc.probe(probeValue, player);
            }
        }

        return (Unit*)calc.param;
    }

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (player->IsGameMaster())
            continue;
        if (player && player->IsAlive())
        {
            float health = player->GetHealthPct();
            if (isRaid || health < sPlayerbotAIConfig.mediumHealth || !IsTargetOfSpellCast(player, predicate))
            {
                float probeValue = 100.0f;
                if (player->GetDistance2d(bot) > sPlayerbotAIConfig.healDistance)
                {
                    probeValue = health + 30.0f;
                }
                else
                {
                    probeValue = health + player->GetDistance2d(bot) / 10.0f;
                }
                // WL triage bias: prioritise the tank (keeps taking hits) and
                // the human master. The two do NOT stack (take the larger) so
                // the combined bias stays <=15 - a genuinely low DPS still
                // outranks a comfortable tank-master (e.g. 55% -> 40.5% loses
                // to a DPS at 33%), instead of a stacked -25 starving it.
                if (WlTriageHealerEnabled())
                {
                    float bias = 0.0f;
                    if (botAI->IsTank(player))
                        bias = 15.0f;
                    if (player == master && bias < 10.0f)
                        bias = 10.0f;
                    probeValue -= bias;
                }
                // delay Check player to here for better performance
                if (probeValue < calc.minValue && Check(player))
                {
                    calc.probe(probeValue, player);
                }
            }
        }

        Pet* pet = player->GetPet();
        if (pet && pet->IsAlive())
        {
            float health = ((Unit*)pet)->GetHealthPct();
            float probeValue = 100.0f;
            if (isRaid || health < sPlayerbotAIConfig.mediumHealth)
                probeValue = health + 30.0f;
            // delay Check pet to here for better performance
            if (probeValue < calc.minValue && Check(pet))
            {
                calc.probe(probeValue, pet);
            }
        }

        Unit* charm = player->GetCharm();
        if (charm && charm->IsAlive())
        {
            float health = charm->GetHealthPct();
            float probeValue = 100.0f;
            if (isRaid || health < sPlayerbotAIConfig.mediumHealth)
                probeValue = health + 30.0f;
            // delay Check charm to here for better performance
            if (probeValue < calc.minValue && Check(charm))
            {
                calc.probe(probeValue, charm);
            }
        }
    }
    return (Unit*)calc.param;
}

bool PartyMemberToHeal::Check(Unit* player)
{
    // return player && player != bot && player->GetMapId() == bot->GetMapId() && player->IsInWorld() &&
    //     ServerFacade::instance().GetDistance2d(bot, player) < (player->IsPlayer() && botAI->IsTank((Player*)player) ? 50.0f
    //     : 40.0f);
    return player->GetMapId() == bot->GetMapId() && !player->IsCharmed() &&
           bot->GetDistance2d(player) < sPlayerbotAIConfig.healDistance * 2 && bot->IsWithinLOSInMap(player);
}

Unit* HealerLowMana::Calculate()
{
    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    MinValueCalculator calc(100);

    for (GroupReference* gref = group->GetFirstMember(); gref; gref = gref->next())
    {
        Player* player = gref->GetSource();
        if (!player || player == bot)
            continue;
        if (player->IsGameMaster() || !player->IsAlive())
            continue;
        if (!botAI->IsHeal(player))
            continue;

        float mana = player->GetPowerPct(POWER_MANA);
        if (mana < calc.minValue)
            calc.probe(mana, player);
    }

    return (Unit*)calc.param;
}

Unit* PartyMemberToProtect::Calculate()
{
    // WL: upstream shipped this entire ally-save system wired end to end
    // (ProtectPartyMemberTrigger -> this value -> CastProtectSpellAction fires
    // Pain Suppression / Hand of Protection / Guardian Spirit on the
    // endangered ally) but left it switched OFF behind an unconditional early
    // return. We enable it, and put the human MASTER first so "my healer
    // bubbled ME right as I was about to die" actually happens.
    if (!WlTriageHealerEnabled())
        return nullptr;

    // a TANK must never be the protector: it is busy holding threat, and the
    // wired protect actions would make it charge off (Warrior Intervene) or
    // waste a mitigation cooldown meant for others. Only healers/DPS protect.
    if (botAI->IsTank(bot))
        return nullptr;

    Group* group = bot->GetGroup();
    if (!group)
        return nullptr;

    std::vector<Unit*> needProtect;
    Player* const master = botAI->GetMaster();

    GuidVector attackers = botAI->GetAiObjectContext()->GetValue<GuidVector>("attackers")->Get();
    for (GuidVector::iterator i = attackers.begin(); i != attackers.end(); ++i)
    {
        Unit* unit = botAI->GetUnit(*i);
        if (!unit)
            continue;

        Unit* pVictim = unit->GetVictim();
        if (!pVictim || !pVictim->IsPlayer())
            continue;

        if (pVictim == bot)
            continue;

        float attackDistance = 30.0f;
        if (ServerFacade::instance().GetDistance2d(pVictim, unit) > attackDistance)
            continue;

        // emergency-only thresholds so a 3-5 min external is not burned on a
        // routine dip a normal heal covers: a tank only at death's door, the
        // human master a touch earlier (saving them is the point), everyone
        // else a genuine emergency.
        float const victimHp = pVictim->GetHealthPct();
        if (botAI->IsTank((Player*)pVictim))
        {
            if (victimHp > 10.0f)
                continue;
        }
        else if (pVictim == master)
        {
            if (victimHp > 35.0f)
                continue;
        }
        else if (victimHp > 25.0f)
            continue;

        if (find(needProtect.begin(), needProtect.end(), pVictim) == needProtect.end())
            needProtect.push_back(pVictim);
    }

    if (needProtect.empty())
        return nullptr;

    // the human we follow comes first - their life matters most in a solo
    // player's group; otherwise the lowest-health endangered ally
    if (master)
        for (Unit* u : needProtect)
            if (u == master)
                return master;

    sort(needProtect.begin(), needProtect.end(), compareByHealth);

    return needProtect[0];
}
