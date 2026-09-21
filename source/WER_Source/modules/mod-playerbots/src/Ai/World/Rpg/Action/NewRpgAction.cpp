/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "NewRpgAction.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>

#include "AiObjectContext.h"
#include "AreaDefines.h"
#include "Playerbots.h"
#include "BroadcastHelper.h"
#include "ChatHelper.h"
#include "DBCStores.h"
#include "GossipDef.h"
#include "GridDefines.h"
#include "IVMapMgr.h"
#include "LastMovementValue.h"
#include "MotionMaster.h"
#include "MoveSpline.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
// PositionInfo/PositionMap: the guide clears a standing "stay" anchor on accept
#include "PositionValue.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotTextMgr.h"
#include "QuestDef.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "TravelMgr.h"
#include "WaypointMovementGenerator.h"
#include "G3D/Vector2.h"
#include <cmath>
#include <cstdlib>

void TellRpgStatusAction::WhisperStatusChange(Player* owner, std::string const& statusName)
{
    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        RPG_STATUS_CHANGED_KEY, RPG_STATUS_CHANGED_DEFAULT,
        {{"%status", statusName}});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
}

bool TellRpgStatusAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    if (text.empty())
    {
        std::string out = botAI->rpgInfo.ToString();
        bot->Whisper(out.c_str(), LANG_UNIVERSAL, owner);
        return true;
    }

    Player* master = botAI->GetMaster();
    bool isMaster = master && master->GetGUID() == owner->GetGUID();
    bool isGM = owner->GetSession() && owner->GetSession()->GetSecurity() >= SEC_GAMEMASTER;
    if (!isMaster && !isGM)
    {
        std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
            "rpg_debug_permission_error",
            "Only your master or a GM can change my rpg status.", {});
        bot->Whisper(msg, LANG_UNIVERSAL, owner);
        return false;
    }

    std::string name = text;
    uint32 questId = 0;
    static std::string const doQuestPrefix = "do quest ";
    size_t doQuestPos = text.find(doQuestPrefix);
    if (doQuestPos != std::string::npos)
    {
        name = "do quest";
        std::string idStr = text.substr(doQuestPos + doQuestPrefix.length());
        try
        {
            questId = static_cast<uint32>(std::stoul(idStr));
        }
        catch (std::exception const&)
        {
            questId = 0;
        }
    }

    NewRpgStatus status = NewRpgInfo::StatusFromString(name);
    NewRpgInfo& info = botAI->rpgInfo;

    if (status == RPG_IDLE)
    {
        info.ChangeToIdle();
        WhisperStatusChange(owner, "IDLE");
        return true;
    }
    else if (status == RPG_REST)
    {
        info.ChangeToRest();
        bot->SetStandState(UNIT_STAND_STATE_SIT);
        WhisperStatusChange(owner, "REST");
        return true;
    }
    else if (status == RPG_WANDER_RANDOM)
    {
        info.ChangeToWanderRandom();
        WhisperStatusChange(owner, "WANDER_RANDOM");
        return true;
    }
    else if (status == RPG_WANDER_NPC)
    {
        info.ChangeToWanderNpc();
        WhisperStatusChange(owner, "WANDER_NPC");
        return true;
    }
    else if (status == RPG_GO_GRIND)
    {
        WorldPosition pos = SelectRandomGrindPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_grind_pos_error", "No grind position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoGrind(pos);
        WhisperStatusChange(owner, "GO_GRIND");
        return true;
    }
    else if (status == RPG_GO_CAMP)
    {
        WorldPosition pos = SelectRandomCampPos(bot);
        if (pos == WorldPosition())
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_camp_pos_error", "No camp position available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToGoCamp(pos);
        WhisperStatusChange(owner, "GO_CAMP");
        return true;
    }
    else if (status == RPG_TRAVEL_FLIGHT)
    {
        uint32 flightMasterEntry = 0;
        WorldPosition flightMasterPos;
        std::vector<uint32> path;
        if (!SelectRandomFlightTaxiNode(flightMasterEntry, flightMasterPos, path))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_flight_path_error", "No flight path available.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToTravelFlight(flightMasterEntry, flightMasterPos, std::move(path));
        WhisperStatusChange(owner, "TRAVEL_FLIGHT");
        return true;
    }
    else if (status == RPG_OUTDOOR_PVP)
    {
        info.ChangeToOutdoorPvp();
        WhisperStatusChange(owner, "OUTDOOR_PVP");
        return true;
    }
    else if (status == RPG_DO_QUEST)
    {
        if (!questId)
        {
            for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
            {
                uint32 qid = bot->GetQuestSlotQuestId(slot);
                if (!qid)
                    continue;
                std::vector<POIInfo> poi;
                if (GetQuestPOIPosAndObjectiveIdx(qid, poi, true))
                {
                    questId = qid;
                    break;
                }
            }
        }
        if (!questId)
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_no_quest_error", "No quest available; use 'do quest <id>'.", {});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatus questStatus = bot->GetQuestStatus(questId);
        if (!quest || (questStatus != QUEST_STATUS_INCOMPLETE && questStatus != QUEST_STATUS_COMPLETE))
        {
            std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
                "rpg_invalid_quest_error", "Invalid quest %quest_id",
                {{"%quest_id", std::to_string(questId)}});
            bot->Whisper(msg, LANG_UNIVERSAL, owner);
            return false;
        }
        info.ChangeToDoQuest(questId, quest);
        WhisperStatusChange(owner, "DO_QUEST " + std::to_string(questId));
        return true;
    }

    std::string msg = PlayerbotTextMgr::instance().GetBotTextOrDefault(
        "rpg_unknown_status_error",
        "Unknown rpg status. Options: idle, rest, wander random, wander npc, "
        "go grind, go camp, do quest [<id>], travel flight, outdoor pvp.", {});
    bot->Whisper(msg, LANG_UNIVERSAL, owner);
    return false;
}

bool StartRpgDoQuestAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(owner);
    uint32 questId = ch.extractQuestId(text);
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        botAI->rpgInfo.ChangeToDoQuest(questId, quest);
        bot->Whisper("퀘스트를 시작합니다: " + std::to_string(questId), LANG_UNIVERSAL, owner);
        return true;
    }
    bot->Whisper("유효하지 않은 퀘스트: " + text, LANG_UNIVERSAL, owner);
    return false;
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus status = info.GetStatus();
    switch (status)
    {
        case RPG_IDLE:
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP});

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_GRIND -> WANDER_RANDOM
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderRandom();
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderNpc();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // WANDER_RANDOM -> IDLE
            if (info.HasStatusPersisted(statusWanderRandomDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (info.HasStatusPersisted(statusWanderNpcDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // DO_QUEST -> IDLE
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            auto& data = std::get<NewRpgInfo::TravelFlight>(info.data);
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_REST:
        {
            // REST -> IDLE
            if (info.HasStatusPersisted(statusRestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        // Small nudge so the next tick's MoveFarTo starts from a
        // slightly different position. Kept small so it doesn't look
        // like the bot is abandoning its destination.
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return MoveRandomNear();
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo)
    {
        // No npc can be found, switch to IDLE
        ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract();
        if (npcOrGo.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);
            return true;
        }

        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) < npcStayTime)
            return false;

        // has reached the npc for more than `npcStayTime`, select the next target
        data.npcOrGo = ObjectGuid();
        data.lastReach = 0;
    }
    else
    {
        if (MoveWorldObjectTo(data.npcOrGo))
            return true;
        // NPC pathing failed (random offset in a wall, mmap hiccup, etc).
        // Take a small random step so the next tick retries from a
        // different spot instead of staring at the NPC from afar.
        return MoveRandomNear(15.0f);
    }

    return true;
}

bool NewRpgDoQuestAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    uint32 questId = data.questId;
    uint8 questStatus = bot->GetQuestStatus(questId);
    switch (questStatus)
    {
        case QUEST_STATUS_INCOMPLETE:
            return DoIncompleteQuest(data);
        case QUEST_STATUS_COMPLETE:
            return DoCompletedQuest(data);
        default:
            break;
    }
    info.ChangeToIdle();
    return true;
}

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    if (data.pos != WorldPosition())
    {
        /// @TODO: extract to a new function
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has completed
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        bool completed = true;
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] < quest->RequiredNpcOrGoCount[currentObjective])
                completed = false;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] <
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                completed = false;
        }
        // the current objective is completed, clear and find a new objective later
        if (completed)
        {
            data.lastReachPOI = 0;
            data.pos = WorldPosition();
            data.objectiveIdx = 0;
        }
    }
    if (data.pos == WorldPosition())
    {
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        {
            // can't find a poi pos to go, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        uint32 rndIdx = urand(0, poiInfo.size() - 1);
        G3D::Vector2 nearestPoi = poiInfo[rndIdx].pos;
        int32 objectiveIdx = poiInfo[rndIdx].objectiveIdx;

        float dx = nearestPoi.x, dy = nearestPoi.y;

        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = objectiveIdx;
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        // Long-range sampler couldn't land a candidate — nudge the
        // bot a short distance so the next tick retries from a
        // different position instead of sitting idle.
        return MoveRandomNear(10.0f);
    }
    // Now we are near the quest objective
    // kill mobs and looting quest should be done automatically by grind strategy

    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        bool hasProgression = false;
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has progression
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        QuestStatusData const& q_status = bot->getQuestStatusMap().at(questId);
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] != 0 && quest->RequiredNpcOrGoCount[currentObjective])
                hasProgression = true;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] != 0 &&
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                hasProgression = true;
        }
        if (!hasProgression)
        {
            // we has reach the poi for more than 5 mins but no progession
            // may not be able to complete this quest, marked as abandoned
            /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
            botAI->lowPriorityQuest.insert(questId);
            botAI->rpgStatistic.questAbandoned++;
            LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // clear and select another poi later
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        return true;
    }

    // At the POI: keep the bot actively placed but avoid large
    // random 20yd hops that look like pacing back and forth. A small
    // ~8yd wander reads as the bot looking around while grind/loot
    // strategies do their work.
    return MoveRandomNear(8.0f);
}

bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    Quest const* quest = data.quest;

    if (data.objectiveIdx != -1)
    {
        // if quest is completed, back to poi with -1 idx to reward
        BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
        botAI->rpgStatistic.questCompleted++;
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            // can't find a poi pos to reward, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }
        assert(poiInfo.size() > 0);
        // now we get the place to get rewarded
        float dx = poiInfo[0].pos.x, dy = poiInfo[0].pos.y;
        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = -1;
    }

    if (data.pos == WorldPosition())
        return false;

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    // Now we are near the qoi of reward
    // the quest should be rewarded by SearchQuestGiverAndAcceptOrReward
    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        // e.g. Can not reward quest to gameobjects
        /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
        botAI->lowPriorityQuest.insert(questId);
        botAI->rpgStatistic.questAbandoned++;
        LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    return false;
}

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    if (bot->IsInFlight())
    {
        data.inFlight = true;
        ContinueCrossMapTaxi();
        return false;
    }

    if (bot->GetDistance(data.flightMasterPos) > INTERACTION_DISTANCE)
        return MoveFarTo(data.flightMasterPos);

    Creature* flightMaster = bot->FindNearestCreature(data.flightMasterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
    {
        info.ChangeToIdle();
        return true;
    }
    if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
        return MoveFarTo(flightMaster);

    std::vector<uint32> nodes = data.path;

    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();

    bot->GetSession()->SendLearnNewTaxiNode(flightMaster);

    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes.empty() ? 0 : nodes.front(), nodes.empty() ? 0 : nodes.back());
        info.ChangeToIdle();
        return true;
    }
    return true;
}

// WOW Legends "The Guide". Toggle lives in mod-wowlegends
// (wowlegends_botflags.cpp); plain extern, no cross-module header.
extern bool WlBotGuideEnabled();

bool WlGuideAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    LOG_DEBUG("playerbots", "[wl guide] {}: start cmd, owner={}, strategy={}, param='{}'",
        bot->GetName(), owner ? owner->GetName() : "NONE",
        botAI->HasStrategy("guide", BOT_STATE_NON_COMBAT) ? "yes" : "MISSING",
        event.getParam().substr(0, 48));
    if (!owner)
        return false;

    std::string const text = event.getParam();
    // "stop" comes BEFORE the enable check: disabling the toggle mid-escort
    // must never wedge an active label the player can no longer cancel
    // (a stale label would also keep pitch-in suppressed forever)
    if (text == "stop")
    {
        context->GetValue<std::string>("wl guide label")->Set("");
        context->GetValue<WorldPosition>("wl guide pos")->Set(WorldPosition());
        bot->Whisper("알겠어요. 같이 다닐게요.", LANG_UNIVERSAL, owner);
        return true;
    }

    // the raw "$wl guide" command is player-reachable: honor the conf
    // toggle here too (covers enable-without-relog half-states as well)
    if (!WlBotGuideEnabled())
    {
        bot->Whisper("지금은 길을 안내할 수 없어요.", LANG_UNIVERSAL, owner);
        return false;
    }

    // bare "$wl guide": a status check, not a malformed order (whitespace
    // counts as bare - a trailing double space must not read as an order)
    if (text.find_first_not_of(' ') == std::string::npos)
    {
        std::string const active = context->GetValue<std::string>("wl guide label")->Get();
        if (!active.empty())
        {
            WorldPosition const cur = context->GetValue<WorldPosition>("wl guide pos")->Get();
            uint32 const paces = uint32(bot->GetExactDist(
                cur.GetPositionX(), cur.GetPositionY(), cur.GetPositionZ()));
            bot->Whisper("안내할 목적지: " + active + " - about "
                + std::to_string(paces) + " paces to go.", LANG_UNIVERSAL, owner);
        }
        else
            bot->Whisper("어디로 갈까요? 지역, 여관주인, 상급자 또는 퀘스트를 알려 주세요.",
                LANG_UNIVERSAL, owner);
        return true;
    }

    uint32 mapId = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    uint32 zTol = 0;
    std::string label;
    std::istringstream in(text);
    if (!(in >> mapId >> x >> y >> z >> zTol))
    {
        bot->Whisper("거기로 가는 길을 모르겠어요.", LANG_UNIVERSAL, owner);
        return false;
    }
    // the raw command is player-typed: nan/inf parse fine via strtod, and a
    // non-finite destination would poison every distance check downstream
    // (arrival never triggers) while feeding PathGenerator garbage
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)
        || !Acore::IsValidMapCoord(x, y, z))
    {
        bot->Whisper("거기로 가는 길을 모르겠어요.", LANG_UNIVERSAL, owner);
        return false;
    }

    std::getline(in, label);
    while (!label.empty() && label.front() == ' ')
        label.erase(0, 1);

    // the label is spoken in world chat: bridge labels are server data, but
    // the raw command is player-typed - strip link markup, cap the length
    label.erase(std::remove(label.begin(), label.end(), '|'), label.end());
    if (label.size() > 48)
        label.resize(48);
    if (label.empty())
        label = "our destination";

    if (mapId != bot->GetMapId())
    {
        bot->Whisper("여기서 가는 길을 찾지 못했어요.", LANG_UNIVERSAL, owner);
        return false;
    }

    context->GetValue<WorldPosition>("wl guide pos")->Set(WorldPosition(mapId, x, y, z));
    context->GetValue<uint32>("wl guide ztol")->Set(zTol);
    context->GetValue<std::string>("wl guide label")->Set(label);
    // every ACCEPTED escort bumps the sequence: the move action must treat a
    // repeat of the same destination as a brand-new trip, not resume stale
    // bookkeeping from the previous one (which instantly trips its watchdog)
    context->GetValue<uint32>("wl guide seq")->Set(
        context->GetValue<uint32>("wl guide seq")->Get() + 1);

    // WL FIX (field report 2026-07-20): an escort and a "stay" order are
    // mutually exclusive, but only ONE direction was enforced - "stay" cancels
    // the guide (ChatShortcutActions.cpp), while a guide did NOT cancel "stay".
    // A stayed bot therefore ACCEPTED escorts it could never walk: the stay
    // strategy's "return to stay position" rides at ACTION_MOVE = 30.0f
    // (StayStrategy.cpp) and outranks "wl guide move" at 1.8f
    // (GrindingStrategy.cpp) on every tick, so the route was planned, one step
    // was taken, and the bot was dragged straight back to its anchor.
    // NOTE: this runs inside the bot's OWN action execution = in-tick, which is
    // the only safe place to touch strategies (out-of-tick Engine init is the
    // known #2474 crash class). Clear the anchor too, so nothing drags it back.
    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT)
        || botAI->HasStrategy("stay", BOT_STATE_COMBAT))
    {
        botAI->ChangeStrategy("-stay", BOT_STATE_NON_COMBAT);
        botAI->ChangeStrategy("-stay", BOT_STATE_COMBAT);
        LOG_DEBUG("playerbots", "[wl guide] {}: cleared a standing 'stay' so the escort can move",
            bot->GetName());
    }
    // the anchor itself must go too: ReturnToStayPositionTrigger fires off the
    // stored position, so leaving it set would keep dragging the bot back even
    // with the strategy gone (same reset FollowChatShortcutAction performs)
    {
        PositionMap& posMap = context->GetValue<PositionMap&>("position")->Get();
        PositionInfo stayPos = posMap["stay"];
        stayPos.Reset();
        posMap["stay"] = stayPos;
        PositionInfo returnPos = posMap["return"];
        returnPos.Reset();
        posMap["return"] = returnPos;
    }

    LOG_DEBUG("playerbots", "[wl guide] {}: escort SET dest=({}, {:.1f}, {:.1f}, {:.1f}) label='{}'",
        bot->GetName(), mapId, x, y, z, label);
    botAI->Say("따라오세요 - " + label + "!");

    // a far destination deserves a word of it (Kneuma's ask): one line,
    // flavored per bot so two guides don't sound alike
    if (bot->GetExactDist(x, y, z) > 2500.0f)
    {
        static char const* lines[] =
        {
            "It's a long road ahead - stay close and I'll see you through.",
            "This is quite the journey - hope you packed water.",
            "A long walk, this one. Good company makes it shorter.",
        };
        botAI->Say(lines[bot->GetGUID().GetCounter() % 3]);
    }
    return true;
}

bool WlGuideMoveAction::isUseful()
{
    if (!WlBotGuideEnabled())
        return false;

    return !context->GetValue<std::string>("wl guide label")->Get().empty();
}

bool WlGuideMoveAction::Execute(Event /*event*/)
{
    std::string const label = context->GetValue<std::string>("wl guide label")->Get();
    WorldPosition const dest = context->GetValue<WorldPosition>("wl guide pos")->Get();

    // a new accepted escort (even to the SAME coords) invalidates all cached
    // trip state: watchdogs, the road route, and the LeadStep detour ring
    uint32 const seq = context->GetValue<uint32>("wl guide seq")->Get();
    if (seq != lastSeq)
    {
        lastSeq = seq;
        pwDest = WorldPosition();
        lastDest = WorldPosition();
        pwGiveLead = false;
        waitingMs = 0;
        replanTried = false;
        tripStartDist = 0.0f;
        milestoneMask = 0;
        comebackCount = 0;
        lastZoneId = bot->GetZoneId();
        lastRouteTickMs = 0;
        arcAtLastComeback = -1.0f;
        pwMoveFails = 0;
        pwHandedFinal = false;
        pwReplans = 0;
    }

    auto clearGuide = [&]()
    {
        context->GetValue<std::string>("wl guide label")->Set("");
        context->GetValue<WorldPosition>("wl guide pos")->Set(WorldPosition());
    };

    if (label.empty() || dest == WorldPosition())
    {
        clearGuide();
        return false;
    }

    // the escortee left, logged off or crossed a portal: guiding is over.
    // WL FIX (field report 2026-07-20): the "is my master a bot?" test used to
    // be GET_PLAYERBOT_AI(master), but ".playerbots bot self" attaches a
    // PlayerbotAI to a REAL player - so one selfbot toggle made this gate fire
    // for EVERY bot mastered by that player (present AND future), clearing the
    // escort on its first tick, before any movement or route planning. The bot
    // still said "Follow me!" (the accept path has no such check) and then
    // never moved. Test the SESSION instead - the same definition the AI-chat
    // bridge uses (wowlegends_aichat.cpp IsRealPlayer) - so a selfbotted human
    // stays a valid escortee and the two definitions can't disagree again.
    Player* master = botAI->GetMaster();
    bool const masterIsBotSession = master && master->GetSession() && master->GetSession()->IsBot();
    if (!master || masterIsBotSession || !bot->GetGroup()
        || bot->GetGroup() != master->GetGroup()
        || master->GetMapId() != bot->GetMapId()
        || dest.GetMapId() != bot->GetMapId())
    {
        clearGuide();
        return false;
    }

    // arrival. The height-tolerant case (close in 2D, big z offset) is for
    // QUEST destinations only, whose z comes from a surface probe - an
    // entrance must be truly reached (the tolerance once announced
    // "We're here" on the hill ABOVE the Wailing Caverns mouth).
    bool const zTolerant = context->GetValue<uint32>("wl guide ztol")->Get() != 0;
    float const dist2d = bot->GetExactDist2d(dest.GetPositionX(), dest.GetPositionY());
    float const dz = std::fabs(bot->GetPositionZ() - dest.GetPositionZ());
    if (bot->GetExactDist(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ()) < 10.0f
        || (zTolerant && dist2d < 10.0f && dz > 15.0f))
    {
        botAI->Say("도착했어요 - " + label + ".");
        clearGuide();
        return true;
    }

    // the escortee fell behind: hold and call out (throttled), like a real
    // guide would. 25y keeps the bot near enough to follow - and the
    // callout is a YELL: a /say from 40y away was never even visible to
    // the player (say radius ~25y), which read as silent abandonment.
    if (bot->GetDistance(master) > 25.0f)
    {
        // waiting is not being stuck - for EITHER watchdog (the corridor
        // one counted master-wait time as stall and exiled healthy trips
        // to navmesh stepping; review catch)
        lastProgressMs = getMSTime();
        pwProgressMs = getMSTime();
        if (!waitingMs)
            waitingMs = getMSTime();

        // the escortee is NOT closing in: the route led somewhere they
        // can't follow - come back down to them and re-plan (the failed
        // leg endpoint stays in memory, so the retry detours)
        if (GetMSTimeDiffToNow(waitingMs) > 20 * IN_MILLISECONDS)
        {
            // every comeback resets the lead watchdogs, so a master who
            // structurally cannot follow (a ledge, a wall) would yo-yo
            // forever - after four cycles, be honest and stand down
            if (++comebackCount >= 4)
            {
                botAI->Say("이 구간에서는 자꾸 떨어지네요. 여기로 안내하기는 어려울 것 같아요.");
                clearGuide();
                return true;
            }
            waitingMs = getMSTime();
            bot->Yell("잠시만요, " + master->GetName() + " - I'm coming back!", LANG_UNIVERSAL);
            // road-vs-lag discrimination (review catch: the old
            // unconditional giveLead exiled a whole trip from the road
            // because the player paused 20s to fight a zombie). A comeback
            // normally just RE-PLANS the road from wherever we regroup;
            // only when the PREVIOUS comeback also produced <30yd of arc
            // progress is the road leg between us genuinely unfollowable -
            // then LeadStep's detour memory takes the escort.
            bool const roadUnfollowable =
                arcAtLastComeback >= 0.0f && pwProgress - arcAtLastComeback < 30.0f;
            arcAtLastComeback = pwProgress;
            if (roadUnfollowable)
            {
                pwGiveLead = true;
                lastDest = WorldPosition(); // fresh LeadStep bookkeeping
            }
            pwDest = WorldPosition();
            pwRoute.clear();
            pwArc.clear();
            return MoveTo(master->GetMapId(), master->GetPositionX(),
                master->GetPositionY(), master->GetPositionZ(),
                false, false, false, true);
        }

        if (bot->isMoving())
            bot->StopMoving();

        time_t const nextCall = context->GetValue<time_t>("last said", "wl_guide")->Get();
        if (time(nullptr) >= nextCall)
        {
            context->GetValue<time_t>("last said", "wl_guide")->Set(time(nullptr) + 12);
            bot->Yell("이쪽이에요, " + master->GetName() + "!", LANG_UNIVERSAL);
        }
        return true;
    }

    waitingMs = 0;

    // zone-crossing callout: the hub-to-hub feel of a "zone gate" design
    // with zero gate data - the border tells us itself
    if (uint32 const zoneNow = bot->GetZoneId(); zoneNow != lastZoneId)
    {
        lastZoneId = zoneNow;
        if (AreaTableEntry const* z = sAreaTableStore.LookupEntry(zoneNow))
            if (z->area_name[0] && *z->area_name[0])
                botAI->Say(std::string("Entering ") + z->area_name[0] + " - onward!");
    }

    // trip narration: a quarter-milestone callout on longer walks, so a
    // cross-zone escort never feels like silent trudging. Short hops
    // (<300yd) skip it - narrating a stroll to the mailbox is noise.
    float const distNow = bot->GetExactDist(
        dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
    if (tripStartDist <= 0.0f)
        tripStartDist = distNow;
    if (tripStartDist > 300.0f)
    {
        float const progress = 1.0f - distNow / tripStartDist;
        struct { float at; uint8 bit; char const* line; } const marks[] =
        {
            { 0.25f, 1, "A quarter of the way - keep up!" },
            { 0.50f, 2, "Halfway there!" },
            { 0.75f, 4, "Nearly there now." },
        };
        for (auto const& m : marks)
        {
            if (progress >= m.at && !(milestoneMask & m.bit))
            {
                milestoneMask |= m.bit;
                botAI->Say(m.line);
                break;
            }
        }
    }

    // lead on - a Legend Roads route when the network covers this trip
    // (globally slope-capped, so never up a goat slope), live navmesh
    // stepping otherwise; no teleport either way
    bool stuck = false;
    if (!RouteStep(dest, stuck))
        LeadStep(dest, stuck);
    if (stuck)
    {
        // first wedge: don't apologize yet - re-plan the WHOLE trip from
        // where we stand (fresh road route, fresh detour memory). Wedges
        // are usually local: the world didn't change, our plan did.
        if (!replanTried)
        {
            replanTried = true;
            pwDest = WorldPosition();
            pwGiveLead = false;
            lastDest = WorldPosition();
            LOG_INFO("playerbots", "[wl guide] {}: wedged - full re-plan from current position",
                bot->GetName());
            botAI->Say("잠시만요. 방향을 확인할게요.");
            return true;
        }
        botAI->Say("지나갈 길을 찾지 못했어요. 미안해요.");
        clearGuide();
    }
    return true;
}

// Walk the precomputed pathway route toward dest. Returns true when this
// step handled the tick (moving or waiting on a committed leg); false hands
// the tick to LeadStep (no coverage, route consumed, handed off after a
// comeback, or the road plan stopped making progress). RouteStep keeps its
// OWN progress bookkeeping (pw*) and never touches LeadStep's members -
// LeadStep re-initializes itself via its dest != lastDest check.
bool WlGuideMoveAction::RouteStep(WorldPosition const& dest, bool& stuck)
{
    stuck = false;

    if (pwGiveLead || !WlBotPathwaysEnabled() || !WlPathwaysAvailable(bot->GetMapId()))
        return false;

    // watchdog fairness: ticks this action did not run (combat, another
    // action won, the wait branch) are not corridor time - a 50s fight must
    // not read as a 50s stall (review catch)
    if (lastRouteTickMs && GetMSTimeDiffToNow(lastRouteTickMs) > 3000)
        pwProgressMs = getMSTime();
    lastRouteTickMs = getMSTime();

    // backoff after a queue-full refusal: keep live-stepping for a moment
    // instead of re-requesting on every single tick (see the retry below)
    if (pwRetryMs && GetMSTimeDiffToNow(pwRetryMs) < 3000)
        return false;
    pwRetryMs = 0;

    if (pwDest != dest)
    {
        pwDest = dest;
        pwIdx = 0;
        pwRoute.clear();
        pwArc.clear();
        pwProgress = 0.0f;
        pwProgressMs = getMSTime();
        pwMoveFails = 0;
        pwHandedFinal = false;
        // planning runs OFF the map tick as a sliced world-tick job (long
        // mountain routes measured 160k+ pops on the climb-inflated graph -
        // the old 20ms in-tick deadline is what field-failed Durotar ->
        // Desolace). LeadStep leads for the ~second it takes.
        if (pwJobId)
            WlPathwaysRouteCancel(pwJobId);
        pwJobId = WlPathwaysRouteAsync(bot->GetMapId(),
            bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(),
            dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        if (!pwJobId)
        {
            // WL FIX (2026-07-20): a queue-full refusal is TRANSIENT (the pool
            // is 8 deep and drains in ~1s), but pwDest was already committed
            // above - so the request was never retried and that escort stayed
            // off the road network for the WHOLE trip. Re-arm with a short
            // backoff: clearing pwDest makes the next tick re-request, and the
            // backoff stops a waiting bot hammering the job mutex every tick.
            // (Pathways-off / no-graph never reach here - RouteStep returns
            // early above - so a 0 here really does mean "try again shortly".)
            pwDest = WorldPosition();
            pwRetryMs = getMSTime();
            LOG_INFO("playerbots", "[wl guide] {}: route queue full - live stepping, retrying shortly",
                bot->GetName());
        }
    }

    if (pwJobId)
    {
        char const* why = "";
        int const r = WlPathwaysRoutePoll(pwJobId, pwRoute, &why);
        if (r == 0)
            return false; // still planning - LeadStep leads meanwhile
        pwJobId = 0;
        if (r < 0)
        {
            // a GC'd unpolled job (long combat starved this action) is NOT
            // "no route" - re-request next tick; the finished answer sits in
            // the warm cache and resolves instantly (review catch). Never
            // done for structural failures (would hot-loop the 15s cache).
            if (std::strcmp(why, "job expired") == 0)
            {
                pwDest = WorldPosition();
                return false;
            }
            LOG_INFO("playerbots", "[wl guide] {}: no road route ({}) - live navmesh stepping",
                bot->GetName(), why);
        }
        else
        {
            LOG_INFO("playerbots", "[wl guide] {}: road route planned, {} nodes, {:.0f} yds",
                bot->GetName(), uint32(pwRoute.size()), bot->GetDistance(dest));
            // prefix arc lengths (2D): pwArc[i] = corridor length up to node i
            pwArc.reserve(pwRoute.size());
            pwArc.push_back(0.0f);
            for (std::size_t i = 1; i < pwRoute.size(); ++i)
            {
                float const dx = pwRoute[i][0] - pwRoute[i - 1][0];
                float const dy = pwRoute[i][1] - pwRoute[i - 1][1];
                pwArc.push_back(pwArc[i - 1] + std::sqrt(dx * dx + dy * dy));
            }
        }
    }

    if (pwRoute.size() < 2)
        return false;

    // let a committed leg finish (same guards as LeadStep)
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return true;
    {
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && lastMove.lastMoveToMapId == bot->GetMapId())
        {
            float const remaining = bot->GetExactDist(
                lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    // ---- corridor following (field-proven; replaced touch-every-node) ----
    // Project the bot onto the corridor polyline in a window around the last
    // known segment; progress is the MONOTONIC arc length of the projection.
    // No node ever needs to be touched: a node sampled atop a gravestone
    // cannot wedge the trip (that exact failure was logged: leg 3/65 bounced
    // 45s in the Deathknell graveyard), grid zigzags get corner-cut by the
    // lookahead, and walking backward along the route is impossible.
    float const bx = bot->GetPositionX();
    float const by = bot->GetPositionY();
    std::size_t const segFrom = pwIdx > 2 ? pwIdx - 2 : 0;
    std::size_t const segTo = std::min(pwRoute.size() - 1, std::size_t(pwIdx) + 12);
    float bestD2 = std::numeric_limits<float>::max();
    float bestArc = pwProgress;
    std::size_t bestSeg = pwIdx;
    float bestZ = pwRoute[std::min(std::size_t(pwIdx), pwRoute.size() - 1)][2];
    for (std::size_t i = segFrom; i < segTo; ++i)
    {
        float const ax = pwRoute[i][0], ay = pwRoute[i][1];
        float const cx = pwRoute[i + 1][0], cy = pwRoute[i + 1][1];
        float const vx = cx - ax, vy = cy - ay;
        float const len2 = vx * vx + vy * vy;
        float t = len2 > 0.01f ? ((bx - ax) * vx + (by - ay) * vy) / len2 : 0.0f;
        t = std::min(1.0f, std::max(0.0f, t));
        float const px = ax + vx * t, py = ay + vy * t;
        float const d2 = (bx - px) * (bx - px) + (by - py) * (by - py);
        if (d2 < bestD2)
        {
            bestD2 = d2;
            bestSeg = i;
            bestArc = pwArc[i] + std::sqrt(len2) * t;
            bestZ = pwRoute[i][2] + (pwRoute[i + 1][2] - pwRoute[i][2]) * t;
        }
    }

    // far off the corridor: re-plan from where we stand. One generous 2D
    // threshold (the entry walk alone may span the 180yd search radius) -
    // and a Z check, because the projection is 2D and stacked geometry
    // (bridge decks, city tiers) would otherwise accrue phantom progress
    // on the wrong layer (review catch; 35yd > the 30yd z-filter the entry
    // node search uses, so a legit entry can never instant-replan-loop)
    float const offCorridor = std::sqrt(bestD2);
    // first projection after adoption gets extra slack: the bot kept moving
    // under LeadStep while the route computed, so it may sit past the 180yd
    // entry radius the moment the corridor snaps in (review catch)
    float const offMax = pwProgress < 1.0f ? 230.0f : 190.0f;
    if (offCorridor > offMax || std::fabs(bot->GetPositionZ() - bestZ) > 35.0f)
    {
        // three consecutive no-progress replans = layer-locked (a cave under
        // the road: the fresh plan resolves the same surface entry we cannot
        // reach, and each replan resets the stall watchdog - field-logged as
        // 7 plans in 2 minutes). Hand to LeadStep: its navmesh knows the
        // cave and walks back OUT.
        if (++pwReplans >= 3)
        {
            LOG_INFO("playerbots", "[wl guide] {}: layer-locked after {} replans (cave?) - live stepping",
                bot->GetName(), pwReplans);
            pwGiveLead = true;
            lastDest = WorldPosition();
            pwRoute.clear();
            pwArc.clear();
            return false;
        }
        pwDest = WorldPosition();
        return false;
    }

    // monotonic progress + the wedge watchdog: the corridor arc must grow
    // (>=5yd per 45s) - crow-fly to dest is NOT the metric (a planned road
    // detour legitimately walks away from dest for minutes)
    if (bestArc > pwProgress + 5.0f)
    {
        pwProgress = bestArc;
        pwIdx = uint32(bestSeg);
        pwProgressMs = getMSTime();
        pwReplans = 0; // real progress clears the layer-lock strikes
    }
    else if (GetMSTimeDiffToNow(pwProgressMs) > 45 * IN_MILLISECONDS)
    {
        LOG_INFO("playerbots", "[wl guide] {}: corridor progress stalled at {:.0f}/{:.0f} yds - live stepping",
            bot->GetName(), pwProgress, pwArc.back());
        pwGiveLead = true;
        lastDest = WorldPosition(); // fresh LeadStep bookkeeping
        pwRoute.clear();
        pwArc.clear();
        return false;
    }

    // route consumed when the remaining corridor is short - LeadStep walks
    // the final stretch to the true destination (the exit node may sit up
    // to 180yd from it). One-shot LeadStep re-init: it may have run briefly
    // mid-trip and its stale bookkeeping must not instantly cry "stuck"
    if (pwArc.back() - std::max(pwProgress, bestArc) < 25.0f)
    {
        if (!pwHandedFinal)
        {
            pwHandedFinal = true;
            lastDest = WorldPosition();
        }
        return false;
    }

    // aim at the LOOKAHEAD point: ahead ALONG the corridor. The navmesh walk
    // to it dodges local obstacles; passing NEAR the corridor advances
    // progress automatically on the next tick.
    float const fromArc = std::max(pwProgress, bestArc);
    auto aimAt = [&](float arc, float& ox, float& oy, float& oz)
    {
        std::size_t seg = bestSeg;
        while (seg + 2 < pwRoute.size() && pwArc[seg + 1] < arc)
            ++seg;
        float const segLen = std::max(0.01f, pwArc[seg + 1] - pwArc[seg]);
        float const st = std::min(1.0f, std::max(0.0f, (arc - pwArc[seg]) / segLen));
        ox = pwRoute[seg][0] + (pwRoute[seg + 1][0] - pwRoute[seg][0]) * st;
        oy = pwRoute[seg][1] + (pwRoute[seg + 1][1] - pwRoute[seg][1]) * st;
        oz = pwRoute[seg][2] + (pwRoute[seg + 1][2] - pwRoute[seg][2]) * st;
    };
    float aimArc = std::min(fromArc + 45.0f, pwArc.back());
    float tx, ty, tz;
    aimAt(aimArc, tx, ty, tz);
    // curvature clamp (review catch): across a hairpin the chord to the aim
    // point is much shorter than the arc - a full-length shortcut would cut
    // the switchback the road exists to avoid (and the projection would then
    // skip it irreversibly). Shorten the lookahead so the bot rounds the bend.
    {
        float const arcAdv = aimArc - fromArc;
        float const chord = std::sqrt((tx - bx) * (tx - bx) + (ty - by) * (ty - by));
        if (arcAdv > 20.0f && chord < 0.7f * arcAdv)
        {
            aimArc = std::min(fromArc + std::max(15.0f, arcAdv * 0.5f), pwArc.back());
            aimAt(aimArc, tx, ty, tz);
        }
    }

    LOG_DEBUG("playerbots", "[wl guide] {}: corridor {:.0f}/{:.0f} yds (off {:.0f}), aim ({:.1f}, {:.1f})",
        bot->GetName(), pwProgress, pwArc.back(), offCorridor, tx, ty);
    if (!MoveTo(bot->GetMapId(), tx, ty, tz, false, false, false, true))
    {
        // aim point on an unreachable poly: a few strikes, then the live
        // navmesh takes the escort rather than a frozen guide (review catch)
        if (++pwMoveFails >= 3)
        {
            LOG_INFO("playerbots", "[wl guide] {}: aim point unreachable x3 - live stepping",
                bot->GetName());
            pwGiveLead = true;
            lastDest = WorldPosition();
            pwRoute.clear();
            pwArc.clear();
        }
        return false;
    }
    pwMoveFails = 0;
    return true;
}

bool WlGuideMoveAction::LeadStep(WorldPosition const& dest, bool& stuck)
{
    stuck = false;

    if (dest != lastDest)
    {
        lastDest = dest;
        recentCount = 0;
        recentHead = 0;
        bestDist = 0.0f;
        lastProgressMs = getMSTime();
    }

    // let a committed leg finish (same guards MoveFarTo uses) - reissuing
    // mid-spline from a new position is exactly what causes oscillation
    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return true;
    {
        LastMovement& lastMove = AI_VALUE(LastMovement&, "last movement");
        if (bot->isMoving() && lastMove.lastMoveToMapId == bot->GetMapId())
        {
            float const remaining = bot->GetExactDist(
                lastMove.lastMoveToX, lastMove.lastMoveToY, lastMove.lastMoveToZ);
            if (remaining > 10.0f)
                return true;
        }
    }

    float const disToDest = bot->GetDistance(dest);

    // progress bookkeeping: no meaningful gain while actively leading for
    // 45s means the way is genuinely blocked - stop promising
    if (bestDist == 0.0f || disToDest + 5.0f < bestDist)
    {
        bestDist = disToDest;
        lastProgressMs = getMSTime();
    }
    else if (GetMSTimeDiffToNow(lastProgressMs) > 45 * IN_MILLISECONDS)
    {
        stuck = true;
        return false;
    }

    uint32 const typeOk = PATHFIND_NORMAL | PATHFIND_INCOMPLETE | PATHFIND_FARFROMPOLY;

    auto nearRecent = [&](float ex, float ey, float ez)
    {
        for (uint8 i = 0; i < recentCount; ++i)
        {
            float const dx = recentEnds[i][0] - ex;
            float const dy = recentEnds[i][1] - ey;
            float const dz = recentEnds[i][2] - ez;
            if (dx * dx + dy * dy + dz * dz < 15.0f * 15.0f)
                return true;
        }
        return false;
    };
    auto tooSteep = [&](float ex, float ey, float ez)
    {
        // never lead up a face the player can't comfortably walk: reject by
        // GRADE (>35%) or big absolute climbs; small rises (ramps, stairs)
        // always pass
        float const rise = ez - bot->GetPositionZ();
        if (rise <= 8.0f)
            return false;
        float const run = std::max(1.0f, bot->GetExactDist2d(ex, ey));
        return rise > 20.0f || rise / run > 0.35f;
    };
    auto rememberAndGo = [&](float ex, float ey, float ez)
    {
        recentEnds[recentHead][0] = ex;
        recentEnds[recentHead][1] = ey;
        recentEnds[recentHead][2] = ez;
        recentHead = (recentHead + 1) % 4;
        if (recentCount < 4)
            ++recentCount;
        LOG_DEBUG("playerbots", "[wl guide] {}: leg to ({:.1f}, {:.1f}, {:.1f}), {:.0f} yds to dest",
            bot->GetName(), ex, ey, ez, disToDest);
        return MoveTo(bot->GetMapId(), ex, ey, ez, false, false, false, true);
    };

    // close enough for one exact leg
    if (disToDest < pathFinderDis)
        return MoveTo(dest.GetMapId(), dest.GetPositionX(), dest.GetPositionY(),
            dest.GetPositionZ(), false, false, false, true);

    // primary: the real navmesh route toward the true destination
    {
        PathGenerator path(bot);
        path.CalculatePath(dest.GetPositionX(), dest.GetPositionY(), dest.GetPositionZ());
        if (!(path.GetPathType() & (~typeOk)))
        {
            G3D::Vector3 const& end = path.GetActualEndPosition();
            float const endToDest = dest.GetExactDist(end.x, end.y, end.z);
            if (endToDest + 5.0f < disToDest && !nearRecent(end.x, end.y, end.z)
                && !tooSteep(end.x, end.y, end.z))
                return rememberAndGo(end.x, end.y, end.z);
        }
    }

    // deterministic fan toward the destination, SCORED: progress toward the
    // destination minus a climb penalty. Roads in this game are the
    // flattest corridors, so preferring flat progress approximates
    // road-following without any road data; the endpoint memory above
    // stops any A-B-A ping-pong.
    float const baseAngle = bot->GetAngle(&dest);
    float const offsets[] = { 0.0f, 0.5f, -0.5f, 1.05f, -1.05f, 1.57f, -1.57f };
    bool haveBest = false;
    float bestScore = 0.0f;
    float bx = 0.0f, by = 0.0f, bz = 0.0f;
    for (float off : offsets)
    {
        float const angle = baseAngle + off;
        float const sx = bot->GetPositionX() + std::cos(angle) * pathFinderDis;
        float const sy = bot->GetPositionY() + std::sin(angle) * pathFinderDis;
        float const sz = bot->GetPositionZ() + 0.5f;
        PathGenerator path(bot);
        path.CalculatePath(sx, sy, sz);
        if (path.GetPathType() & (~typeOk))
            continue;

        G3D::Vector3 const& end = path.GetActualEndPosition();
        if (bot->GetExactDist(end.x, end.y, end.z) < 25.0f)
            continue;                        // barely moves: not a real leg
        if (nearRecent(end.x, end.y, end.z) || tooSteep(end.x, end.y, end.z))
            continue;

        float const progress = disToDest - dest.GetExactDist(end.x, end.y, end.z);
        float const climb = std::max(0.0f, end.z - bot->GetPositionZ());
        float const score = progress - 2.0f * climb;
        if (!haveBest || score > bestScore)
        {
            haveBest = true;
            bestScore = score;
            bx = end.x;
            by = end.y;
            bz = end.z;
        }
    }
    if (haveBest)
        return rememberAndGo(bx, by, bz);

    stuck = true;
    return false;
}

void NewRpgTravelFlightAction::ContinueCrossMapTaxi()
{
    if (bot->IsBeingTeleported())
        return;

    if (!bot->movespline->Finalized())
        return;

    MotionMaster* mm = bot->GetMotionMaster();
    if (!mm || mm->GetCurrentMovementGeneratorType() != FLIGHT_MOTION_TYPE)
        return;

    // Check if we are at our destination.
    uint32 nextDest = bot->m_taxi.GetTaxiDestination();
    if (!nextDest)
        return;

    // Confirm next node needs different map.
    TaxiNodesEntry const* nextNode = sTaxiNodesStore.LookupEntry(nextDest);
    if (!nextNode || nextNode->map_id == bot->GetMapId())
        return;

    FlightPathMovementGenerator* flight = dynamic_cast<FlightPathMovementGenerator*>(mm->top());
    if (!flight)
        return;

    LOG_DEBUG("playerbots", "[New RPG] {} continuing taxi across map boundary (next node {} on map {})",
              bot->GetName(), nextDest, nextNode->map_id);

    flight->SetCurrentNodeAfterTeleport();

    if (flight->HasArrived())
        return;

    TaxiPathNodeEntry const* node = flight->GetPath()[flight->GetCurrentNode()];
    flight->SkipCurrentNode();

    bot->TeleportTo(nextNode->map_id, node->x, node->y, node->z, bot->GetOrientation(), TELE_TO_NOT_LEAVE_TAXI);
}
