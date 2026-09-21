/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_NEWRPGBASEACTION_H
#define PLAYERBOTS_NEWRPGBASEACTION_H

#include "LastMovementValue.h"
#include "MovementActions.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "TravelMgr.h"

#include <array>
#include <vector>

struct POIInfo
{
    G3D::Vector2 pos;
    int32 objectiveIdx;
};

// WL "Legend Roads": precomputed slope-capped pathway network, provided by
// mod-wowlegends (wowlegends_pathways.cpp). No coverage -> route() = false
// and callers fall back to live navmesh stepping.
extern bool WlBotPathwaysEnabled();
extern bool WlBotPathwaysAllBots();
extern bool WlPathwaysAvailable(uint32 mapId);
// whyOut (optional): a static human-readable reason when the call returns
// false - "no road near start", "disconnected landmasses", "budget bail
// (transient)"... - so escort planning can LOG why a road route was refused
// instead of failing silently.
extern bool WlPathwaysRoute(uint32 mapId, float sx, float sy, float sz,
                            float dx, float dy, float dz,
                            std::vector<std::array<float, 3>>& out,
                            char const** whyOut = nullptr);
// Async route jobs (guide escorts): long mountain routes on the
// climb-inflated graph need 150k+ A* pops - too heavy for a map tick.
// Request returns a job id (0 = unavailable/queue full - fall back);
// the world tick computes in slices; poll returns 0 pending, 1 done
// (path in `out`), -1 failed (reason in whyOut). Cancel is optional
// (jobs are GC'd), but polite on retarget.
extern uint32 WlPathwaysRouteAsync(uint32 mapId, float sx, float sy, float sz,
                                   float dx, float dy, float dz);
extern int WlPathwaysRoutePoll(uint32 jobId,
                               std::vector<std::array<float, 3>>& out,
                               char const** whyOut);
extern void WlPathwaysRouteCancel(uint32 jobId);

/// A base (composition) class for all new rpg actions
/// All functions that may be shared by multiple actions should be declared here
/// And we should make all actions composable instead of inheritable
class NewRpgBaseAction : public MovementAction
{
public:
    NewRpgBaseAction(PlayerbotAI* botAI, std::string name) : MovementAction(botAI, name) {}

protected:
    /* MOVEMENT RELATED */
    // WL: allowTeleport=false disables the stuck-recovery teleport (a guide
    // escorting a real player must never blink away); outStuck reports the
    // give-up so the caller can apologize and abort.
    bool MoveFarTo(WorldPosition dest, bool allowTeleport = true, bool* outStuck = nullptr);
    bool MoveWorldObjectTo(ObjectGuid guid, float distance = INTERACTION_DISTANCE);
    bool MoveRandomNear(float moveStep = 50.0f, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL, WorldObject* center = nullptr);
    bool ForceToWait(uint32 duration, MovementPriority priority = MovementPriority::MOVEMENT_NORMAL);

    /* QUEST RELATED CHECK */
    ObjectGuid ChooseNpcOrGameObjectToInteract(bool questgiverOnly = false, float distanceLimit = 0.0f);
    bool HasQuestToAcceptOrReward(WorldObject* object);
    bool InteractWithNpcOrGameObjectForQuest(ObjectGuid guid);
    bool CanInteractWithQuestGiver(Object* questGiver);
    bool IsWithinInteractionDist(Object* object);
    uint32 BestRewardIndex(Quest const* quest);
    bool IsQuestWorthDoing(Quest const* quest);
    bool IsQuestCapableDoing(Quest const* quest);

    /* QUEST RELATED ACTION */
    bool SearchQuestGiverAndAcceptOrReward();
    bool AcceptQuest(Quest const* quest, ObjectGuid guid);
    bool TurnInQuest(Quest const* quest, ObjectGuid guid);
    bool OrganizeQuestLog();

protected:
    bool GetQuestPOIPosAndObjectiveIdx(uint32 questId, std::vector<POIInfo>& poiInfo, bool toComplete = false);
    static WorldPosition SelectRandomGrindPos(Player* bot);
    static WorldPosition SelectRandomCampPos(Player* bot);
    bool SelectRandomFlightTaxiNode(uint32& flightMasterEntry, WorldPosition& flightMasterPos, std::vector<uint32>& path);
    bool RandomChangeStatus(std::vector<NewRpgStatus> candidateStatus);
    bool CheckRpgStatusAvailable(NewRpgStatus status);

protected:
    /* FOR MOVE FAR */
    // WL: cached Legend Roads route for the current long-travel destination
    // (one consumer per action object: either MoveFarTo or the guide)
    std::vector<std::array<float, 3>> pwRoute;
    uint32 pwIdx = 0;
    WorldPosition pwDest{};

    const float pathFinderDis = 70.0f;
    // Time without real progress toward dest before MoveFarTo
    // falls back to teleport recovery. Kept short enough that a
    // bot truly oscillating around an unreachable destination
    // (mmap returning non-progressing partial paths, or NOPATH +
    // cone fallback wandering) doesn't spin for 5 minutes before
    // the teleport fires, but long enough that a genuine long
    // walk that is slowly making progress never triggers it.
    const uint32 stuckTime = 90 * 1000;
};

#endif
