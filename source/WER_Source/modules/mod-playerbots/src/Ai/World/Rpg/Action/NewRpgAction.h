/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGACTION_H
#define PLAYERBOTS_NEWRPGACTION_H

#include "Duration.h"
#include "MovementActions.h"
#include "NewRpgBaseAction.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"
#include <string>

class Player;

class TellRpgStatusAction : public NewRpgBaseAction
{
public:
    TellRpgStatusAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "rpg status") {}

    bool Execute(Event event) override;

private:
    static constexpr char const* RPG_STATUS_CHANGED_KEY = "rpg_status_changed";
    static constexpr char const* RPG_STATUS_CHANGED_DEFAULT = "rpg status -> %status";

    void WhisperStatusChange(Player* owner, std::string const& statusName);
};

class StartRpgDoQuestAction : public Action
{
public:
    StartRpgDoQuestAction(PlayerbotAI* botAI) : Action(botAI, "start rpg do quest") {}

    bool Execute(Event event) override;
};

class NewRpgStatusUpdateAction : public NewRpgBaseAction
{
public:
    NewRpgStatusUpdateAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg status update")
    {
        // int statusCount = RPG_STATUS_END - 1;

        // transitionMat.resize(statusCount, std::vector<int>(statusCount, 0));

        // transitionMat[RPG_IDLE][RPG_GO_GRIND] = 20;
        // transitionMat[RPG_IDLE][RPG_GO_CAMP] = 15;
        // transitionMat[RPG_IDLE][RPG_WANDER_NPC] = 30;
        // transitionMat[RPG_IDLE][RPG_DO_QUEST] = 35;
    }
    bool Execute(Event event) override;

protected:
    // static NewRpgStatusTransitionProb transitionMat;
    const int32 statusWanderNpcDuration = 5 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusWanderRandomDuration = 5 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusRestDuration = 30 * IN_MILLISECONDS ;
    const int32 statusDoQuestDuration = 30 * MINUTE  * IN_MILLISECONDS ;
    const int32 statusOutDoorPvPDuration = HOUR * IN_MILLISECONDS ;
};

class NewRpgGoGrindAction : public NewRpgBaseAction
{
public:
    NewRpgGoGrindAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg go grind") {}
    bool Execute(Event event) override;
};

class NewRpgGoCampAction : public NewRpgBaseAction
{
public:
    NewRpgGoCampAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg go camp") {}
    bool Execute(Event event) override;
};

class NewRpgWanderRandomAction : public NewRpgBaseAction
{
public:
    NewRpgWanderRandomAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg wander random") {}
    bool Execute(Event event) override;
};

class NewRpgWanderNpcAction : public NewRpgBaseAction
{
public:
    NewRpgWanderNpcAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg move npcs") {}
    bool Execute(Event event) override;

    const uint32 npcStayTime = 8 * 1000;
};

class NewRpgDoQuestAction : public NewRpgBaseAction
{
public:
    NewRpgDoQuestAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg do quest") {}
    bool Execute(Event event) override;

protected:
    bool DoIncompleteQuest(NewRpgInfo::DoQuest& data);
    bool DoCompletedQuest(NewRpgInfo::DoQuest& data);

    const uint32 poiStayTime = 5 * 60 * 1000;
};

class NewRpgTravelFlightAction : public NewRpgBaseAction
{
public:
    NewRpgTravelFlightAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "new rpg travel flight") {}
    bool Execute(Event event) override;

protected:
    void ContinueCrossMapTaxi();
};

// WOW Legends "The Guide": escort a REAL PLAYER to a destination.
//
// The world-thread order bridge resolves a spoken destination to coords and
// hands them over as chat-command TEXT ("wl guide <mapId> <x> <y> <z>
// <label>") - parsed HERE on the bot's own tick, stored in the bot's own
// context values ("wl guide pos"/"wl guide label"), #2474-safe by
// construction. "wl guide stop" cancels.
class WlGuideAction : public Action
{
public:
    WlGuideAction(PlayerbotAI* botAI) : Action(botAI, "wl guide") {}

    bool Execute(Event event) override;
};

// the escort driver: lead toward the destination in navmesh legs, wait with
// a callout when the master falls behind, announce arrival, abort honestly
// when no path exists. Owns its own stepping logic: a deterministic fan of
// detour angles with endpoint MEMORY (no revisiting = no ping-pong loops)
// and a steepness filter (never lead up a face a player can't walk). A
// guide never teleports.
class WlGuideMoveAction : public NewRpgBaseAction
{
public:
    WlGuideMoveAction(PlayerbotAI* botAI) : NewRpgBaseAction(botAI, "wl guide move") {}

    bool Execute(Event event) override;
    bool isUseful() override;

private:
    bool LeadStep(WorldPosition const& dest, bool& stuck);
    bool RouteStep(WorldPosition const& dest, bool& stuck);

    WorldPosition lastDest;
    float recentEnds[4][3] = {};
    uint8 recentCount = 0;
    uint8 recentHead = 0;
    float bestDist = 0.0f;
    uint32 lastProgressMs = 0;
    uint32 waitingMs = 0;
    // one full re-plan (road route + fresh detour memory) before giving up -
    // a wedged plan is usually recoverable from where the bot now stands
    bool replanTried = false;
    // trip narration: starting distance + which quarter-milestones were said
    float tripStartDist = 0.0f;
    uint8 milestoneMask = 0;
    // lead->wait->comeback cycles this escort: each cycle resets the lead
    // watchdogs ("waiting is not being stuck"), so a master who structurally
    // cannot follow would yo-yo forever without this cap
    uint8 comebackCount = 0;
    // Legend Roads state: seq detects a brand-new escort (even to identical
    // coords); pwGiveLead hands the rest of an escort to LeadStep after a
    // comeback or a wedged road plan; pwProgressMs is the road watchdog
    // (kept apart from LeadStep's bestDist/lastProgressMs)
    uint32 lastSeq = 0;
    bool pwGiveLead = false;
    uint32 pwProgressMs = 0;
    // corridor following (field-proven replacement for touch-every-node):
    // pwArc[i] = polyline arc length up to node i; pwProgress = the bot's
    // monotonic along-route progress (projection onto the corridor). The
    // bot aims at a LOOKAHEAD point ahead on the corridor and never has to
    // touch individual nodes - a node atop a gravestone can't wedge a trip,
    // grid zigzags get corner-cut, and walking backward is impossible.
    std::vector<float> pwArc;
    float pwProgress = 0.0f;
    // zone-crossing callout ("Entering the Barrens - onward!") - the
    // hub-to-hub feel of a zone-gate design without any gate data
    uint32 lastZoneId = 0;
    // watchdog fairness: time not spent road-walking (combat, waiting for
    // the master, other actions winning the tick) must not count as stall
    uint32 lastRouteTickMs = 0;
    // road-vs-lag discrimination: a comeback only exiles the trip from the
    // road when the PREVIOUS comeback also produced no arc progress - a
    // player pausing to fight a zombie is lag, not unfollowable terrain
    float arcAtLastComeback = -1.0f;
    // consecutive MoveTo failures toward the aim point (unreachable poly)
    uint8 pwMoveFails = 0;
    // in-flight async route job (0 = none); LeadStep leads while it computes
    uint32 pwJobId = 0;
    // queue-full backoff: a refused route request is transient, so it must be
    // retried (it used to be dropped for the whole trip) - but not every tick
    uint32 pwRetryMs = 0;
    // consecutive replans with zero arc progress: a bot on the wrong layer
    // (field case: walked into a cave under the road) replans forever - the
    // fresh plan resolves the same surface entry it cannot reach, and every
    // replan resets the stall watchdog. Three strikes -> LeadStep, whose
    // navmesh KNOWS the cave and walks back out.
    uint8 pwReplans = 0;
    // one-shot LeadStep re-init when the corridor hands over the final
    // stretch (stale LeadStep bookkeeping must not instantly cry "stuck")
    bool pwHandedFinal = false;
};

#endif
