/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_CHOOSETARGETACTIONS_H
#define PLAYERBOTS_CHOOSETARGETACTIONS_H

#include "AttackAction.h"
#include "EmoteAction.h"

class PlayerbotAI;

class DpsAoeAction : public AttackAction
{
public:
    DpsAoeAction(PlayerbotAI* botAI) : AttackAction(botAI, "dps aoe") {}

    std::string const GetTargetName() override { return "dps aoe target"; }
};

class DpsAssistAction : public AttackAction
{
public:
    DpsAssistAction(PlayerbotAI* botAI) : AttackAction(botAI, "dps assist") {}

    std::string const GetTargetName() override { return "dps target"; }
    bool isUseful() override;
};

class TankAssistAction : public AttackAction
{
public:
    TankAssistAction(PlayerbotAI* botAI) : AttackAction(botAI, "tank assist") {}

    std::string const GetTargetName() override { return "tank target"; }
};

class AggressiveTargetAction : public AttackAction
{
public:
    AggressiveTargetAction(PlayerbotAI* botAI) : AttackAction(botAI, "aggressive target") {}

    std::string const GetTargetName() override { return "aggressive target"; }
    bool isUseful() override;
};

class AttackAnythingAction : public AttackAction
{
public:
    AttackAnythingAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack anything") {}

    std::string const GetTargetName() override { return "grind target"; }
    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override;
};

// WOW Legends "idle life": settled-master ambience. Every action self-gates
// on the master being a settled real player (not moving/mounted/fighting),
// so the moment the master does anything, follow/combat take back over.

// a short walking stroll anchored to the MASTER's position
class WlIdleStrollAction : public MovementAction
{
public:
    WlIdleStrollAction(PlayerbotAI* botAI) : MovementAction(botAI, "wl idle stroll") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// the master sat down: walk to a deterministic seat around the nearest
// campfire (or around the master) - the core posture mirror sits us there
class WlCampfireSpotAction : public MovementAction
{
public:
    WlCampfireSpotAction(PlayerbotAI* botAI) : MovementAction(botAI, "wl campfire spot") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

// stock random emote, but only while settled (bypasses the global
// RandomBotEmote conf, keeps EmoteAction's own "last emote" throttle)
class WlIdleEmoteAction : public EmoteAction
{
public:
    WlIdleEmoteAction(PlayerbotAI* botAI) : EmoteAction(botAI) {}

    bool isUseful() override;
};

// WOW Legends "pitch in": out-of-combat attack on a mob the bot's own quest
// log still needs, chosen by WlPitchInTargetValue (leashed to the master).
class WlPitchInAttackAction : public AttackAction
{
public:
    WlPitchInAttackAction(PlayerbotAI* botAI) : AttackAction(botAI, "pitch in attack") {}

    std::string const GetTargetName() override { return "pitch in target"; }
    bool Execute(Event event) override;
    bool isUseful() override;
    bool isPossible() override { return GetTarget() && AttackAction::isPossible(); }
};

class AttackLeastHpTargetAction : public AttackAction
{
public:
    AttackLeastHpTargetAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack least hp target") {}

    std::string const GetTargetName() override { return "least hp target"; }
};

class AttackEnemyPlayerAction : public AttackAction
{
public:
    AttackEnemyPlayerAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack enemy player") {}

    std::string const GetTargetName() override { return "enemy player target"; }
    bool isUseful() override;
};

class AttackRtiTargetAction : public AttackAction
{
public:
    AttackRtiTargetAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack rti target") {}

    std::string const GetTargetName() override { return "rti target"; }
    bool Execute(Event event) override;
    bool isUseful() override;
};

class AttackEnemyFlagCarrierAction : public AttackAction
{
public:
    AttackEnemyFlagCarrierAction(PlayerbotAI* botAI) : AttackAction(botAI, "attack enemy flag carrier") {}

    std::string const GetTargetName() override { return "enemy flag carrier"; }
    bool isUseful() override;
};

class DropTargetAction : public Action
{
public:
    DropTargetAction(PlayerbotAI* botAI) : Action(botAI, "drop target") {}

    bool Execute(Event event) override;
};

#endif
