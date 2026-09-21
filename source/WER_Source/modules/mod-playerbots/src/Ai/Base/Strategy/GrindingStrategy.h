/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GRINDINGSTRATEGY_H
#define PLAYERBOTS_GRINDINGSTRATEGY_H

#include "NonCombatStrategy.h"

class PlayerbotAI;

class GrindingStrategy : public NonCombatStrategy
{
public:
    GrindingStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI) {}

    std::string const getName() override { return "grind"; }
    uint32 GetType() const override { return STRATEGY_TYPE_DPS; }
    std::vector<NextAction> getDefaultActions() override;
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

class MoveRandomStrategy : public NonCombatStrategy
{
public:
    MoveRandomStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI) {}
    std::string const getName() override { return "move random"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// WL: dormant marker strategy. Upstream gates its grind debug narration on
// HasStrategy("debug grind") but never REGISTERS such a strategy, so
// "nc +debug grind" was a silent no-op (dead affordance). Registering it
// makes both the grind and the pitch-in debug whispers reachable.
class WlDebugGrindStrategy : public NonCombatStrategy
{
public:
    WlDebugGrindStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI) {}

    std::string const getName() override { return "debug grind"; }
};

// WOW Legends "pitch in" (v1.4.0 The Living Party): when idle with no target,
// player-owned grouped bots attack a nearby mob their own quest log still
// needs instead of just standing behind the master. Per bot: "nc -pitch in";
// global gate WowLegends.BotPitchIn.Enabled (mod-wowlegends).
class WlPitchInStrategy : public NonCombatStrategy
{
public:
    WlPitchInStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI) {}

    // no GetType() override: a DPS type here would leak into the engine's
    // strategyTypeMask and flip IsDps() true for healer/tank bots
    std::string const getName() override { return "pitch in"; }
    std::vector<NextAction> getDefaultActions() override;
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// WOW Legends "idle life" (v1.4.0 The Living Party): settled-master ambience
// for player-owned grouped bots - short walking strolls around the player,
// joining the campfire circle when the player sits, an occasional emote.
// Every action self-gates on the master being settled, so movement, orders
// and combat override instantly. Per bot: "nc -idle life"; global gate
// WowLegends.BotIdleLife.Enabled (mod-wowlegends).
class WlIdleLifeStrategy : public NonCombatStrategy
{
public:
    WlIdleLifeStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI) {}

    std::string const getName() override { return "idle life"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};

// WOW Legends "The Guide": while an escort destination is set (WlGuideAction,
// via the order bridge), lead the master there - outranks idle life and
// follow, still below collision/pitch-in/loot so real duties interrupt.
class WlGuideStrategy : public NonCombatStrategy
{
public:
    WlGuideStrategy(PlayerbotAI* botAI) : NonCombatStrategy(botAI) {}

    std::string const getName() override { return "guide"; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
};
#endif
