/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GrindingStrategy.h"
#include "Playerbots.h"

std::vector<NextAction> GrindingStrategy::getDefaultActions()
{
    return {
        NextAction("drink", 4.2f),
        NextAction("food", 4.1f),
    };
}

void GrindingStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // reduce lower than loot
    triggers.push_back(
        new TriggerNode(
            "no target",
            {
                NextAction("attack anything", 4.0f)
            }
        )
    );
}

void MoveRandomStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    triggers.push_back(
        new TriggerNode(
            "often",
            {
                NextAction("move random", 1.5f)
            }
        )
    );
}

std::vector<NextAction> WlPitchInStrategy::getDefaultActions()
{
    // recover before re-engaging: strictly above the 3.8 pitch-in attack,
    // below grind's own 4.2/4.1 (same guard grind ships for its 4.0)
    return {
        NextAction("drink", 4.0f),
        NextAction("food", 3.9f),
    };
}

void WlPitchInStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // below loot and grind so pitching in never outranks either
    triggers.push_back(
        new TriggerNode(
            "no target",
            {
                NextAction("pitch in attack", 3.8f)
            }
        )
    );
}

void WlIdleLifeStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // the 1.1-1.5 idle band: above follow's 1.0 default action but below
    // everything that matters (collision 2.0, pitch in 3.8, loot 5+). The
    // actions' settled gates flip them off the moment the master moves, so
    // follow instantly wins again.
    triggers.push_back(
        new TriggerNode(
            "wl idle",
            {
                NextAction("wl idle stroll", 1.2f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "wl idle seldom",
            {
                NextAction("wl idle emote", 1.15f)
            }
        )
    );
    triggers.push_back(
        new TriggerNode(
            "wl master sitting",
            {
                NextAction("wl campfire spot", 1.4f)
            }
        )
    );
}

void WlGuideStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    // above idle life (1.4) and follow (1.0) - guiding is active duty; below
    // collision 2.0 / pitch in 3.8 / loot 5+ so real work still interrupts
    triggers.push_back(
        new TriggerNode(
            "wl guide active",
            {
                NextAction("wl guide move", 1.8f)
            }
        )
    );
}
