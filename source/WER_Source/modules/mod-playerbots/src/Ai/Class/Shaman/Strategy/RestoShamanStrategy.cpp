/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RestoShamanStrategy.h"
#include "Playerbots.h"

RestoShamanStrategy::RestoShamanStrategy(PlayerbotAI* botAI) : GenericShamanStrategy(botAI)
{
    // No custom ActionNodeFactory needed
}

// ===== Trigger Initialization ===
void RestoShamanStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    GenericShamanStrategy::InitTriggers(triggers);

    // Totem Triggers
    triggers.push_back(new TriggerNode("call of the elements", { NextAction("call of the elements", 60.0f) }));
    triggers.push_back(new TriggerNode("low health", { NextAction("stoneclaw totem", 40.0f) }));
    triggers.push_back(new TriggerNode("medium mana", { NextAction("mana tide totem", ACTION_HIGH + 5) }));

    // Healing Triggers
    //
    // WL FIX (2026-08-01, reported by StretchtGorilla via a player): these were
    // raw hard-coded floats, and they sat BELOW the standard action bands - so a
    // Restoration Shaman lost every contest against ordinary movement and healed
    // almost nothing in combat. The critical-health heals scored 25/24/23 while
    // ACTION_MOVE is 30, so literally any movement action outranked saving a
    // dying tank; the medium-health heals at 14-16 were under ACTION_MEDIUM_HEAL
    // (20) as well. Out of combat nothing competes, so it healed fine the moment
    // the fight ended - exactly the reported symptom, and exactly why swapping in
    // a priest "fixed" it.
    //
    // Now expressed in the same constants the Priest uses (HealPriestStrategy),
    // band for band. The relative order WITHIN each trigger is unchanged - only
    // the band each one sits in. "group heal setting" keeps its exact numbers
    // (27/26 == ACTION_MEDIUM_HEAL + 7/+6); it was already in the right place.
    triggers.push_back(new TriggerNode("group heal setting", { NextAction("riptide on party", ACTION_MEDIUM_HEAL + 7),
                                                               NextAction("chain heal on party", ACTION_MEDIUM_HEAL + 6) }));

    triggers.push_back(new TriggerNode("party member critical health", { NextAction("riptide on party", ACTION_CRITICAL_HEAL + 5),
                                                                         NextAction("healing wave on party", ACTION_CRITICAL_HEAL + 4),
                                                                         NextAction("lesser healing wave on party", ACTION_CRITICAL_HEAL + 3) }));

    triggers.push_back(new TriggerNode("party member low health", { NextAction("riptide on party", ACTION_MEDIUM_HEAL + 4),
                                                                    NextAction("healing wave on party", ACTION_MEDIUM_HEAL + 3),
                                                                    NextAction("lesser healing wave on party", ACTION_MEDIUM_HEAL + 2) }));

    triggers.push_back(new TriggerNode("party member medium health", { NextAction("riptide on party", ACTION_LIGHT_HEAL + 9),
                                                                       NextAction("healing wave on party", ACTION_LIGHT_HEAL + 7),
                                                                       NextAction("lesser healing wave on party", ACTION_LIGHT_HEAL + 6) }));

    triggers.push_back(new TriggerNode("party member almost full health", { NextAction("riptide on party", ACTION_LIGHT_HEAL + 3),
                                                                            NextAction("lesser healing wave on party", ACTION_LIGHT_HEAL + 2) }));

    triggers.push_back(new TriggerNode("earth shield on main tank", { NextAction("earth shield on main tank", ACTION_HIGH + 7) }));

    // Dispel Triggers
    triggers.push_back(new TriggerNode("party member cleanse spirit poison", { NextAction("cleanse spirit poison on party", ACTION_DISPEL + 2) }));
    triggers.push_back(new TriggerNode("party member cleanse spirit disease", { NextAction("cleanse spirit disease on party", ACTION_DISPEL + 2) }));
    triggers.push_back(new TriggerNode("party member cleanse spirit curse",{ NextAction("cleanse spirit curse on party", ACTION_DISPEL + 2) }));

    // Range/Mana Triggers
    triggers.push_back(new TriggerNode("enemy too close for spell", { NextAction("flee", ACTION_MOVE + 9) }));
    // WL: raised +1 -> +10 to match the Priest, and it is REQUIRED by the heal
    // fix above rather than cosmetic. "Get in range" must outrank the heal
    // itself, or a shaman whose target is out of range picks the heal, fails the
    // range check and never closes the distance. Before the fix this sat at 31
    // and beat the 25 heals by accident; now the heals are 33-35, so without
    // this it would invert and the fix would introduce a new bug.
    triggers.push_back(new TriggerNode("party member to heal out of spell range", { NextAction("reach party member to heal", ACTION_CRITICAL_HEAL + 10) }));
    triggers.push_back(new TriggerNode("water shield", { NextAction("water shield", 19.5f) }));
}

void ShamanHealerDpsStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(new TriggerNode("healer should attack", { NextAction("flame shock", ACTION_DEFAULT + 0.2f),
                                                                 NextAction("lava burst", ACTION_DEFAULT + 0.1f),
                                                                 NextAction("lightning bolt", ACTION_DEFAULT) }));

    triggers.push_back( new TriggerNode("medium aoe and healer should attack", { NextAction("chain lightning", ACTION_DEFAULT + 0.3f) }));
}
