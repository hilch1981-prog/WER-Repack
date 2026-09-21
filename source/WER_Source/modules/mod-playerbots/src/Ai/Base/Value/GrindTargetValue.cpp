/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "GrindTargetValue.h"

#include <cmath>

#include "NewRpgInfo.h"
#include "Playerbots.h"
#include "ReputationMgr.h"
#include "ServerFacade.h"
#include "SharedDefines.h"

Unit* GrindTargetValue::Calculate()
{
    uint32 memberCount = 1;
    Group* group = bot->GetGroup();
    if (group)
        memberCount = group->GetMembersCount();

    Unit* target = nullptr;
    uint32 assistCount = 0;
    while (!target && assistCount < memberCount)
    {
        target = FindTargetForGrinding(assistCount++);
    }

    return target;
}

Unit* GrindTargetValue::FindTargetForGrinding(uint32 assistCount)
{
    Group* group = bot->GetGroup();
    Player* master = GetMaster();

    if (master && (master == bot || master->GetMapId() != bot->GetMapId() || master->IsBeingTeleported() ||
                   !GET_PLAYERBOT_AI(master)))
        master = nullptr;

    GuidVector attackers = context->GetValue<GuidVector>("attackers")->Get();
    for (ObjectGuid const guid : attackers)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;

        return unit;
    }

    GuidVector targets = *context->GetValue<GuidVector>("possible targets");
    if (targets.empty())
        return nullptr;

    float distance = 0;
    Unit* result = nullptr;
    std::unordered_map<uint32, bool> needForQuestMap;

    for (ObjectGuid const guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit)
            continue;

        if (!unit->IsInWorld() || unit->IsDuringRemoveFromWorld())
            continue;

        if (unit->ToCreature() && !unit->ToCreature()->GetCreatureTemplate()->lootid &&
            bot->GetReactionTo(unit) >= REP_NEUTRAL)
            continue;

        if (!bot->IsHostileTo(unit) && unit->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
            continue;

        if (!bot->isHonorOrXPTarget(unit))
            continue;

        if (abs(bot->GetPositionZ() - unit->GetPositionZ()) > INTERACTION_DISTANCE)
            continue;

        if (!bot->InBattleground() && GetTargetingPlayerCount(unit) > assistCount)
            continue;

        // if (!bot->InBattleground() && master && master->GetDistance(unit) >= sPlayerbotAIConfig.grindDistance &&
        // !sRandomPlayerbotMgr.IsRandomBot(bot)) continue;

        // Bots in bot-groups no have a more limited range to look for grind target
        if (!bot->InBattleground() && master && botAI->HasStrategy("follow", BotState::BOT_STATE_NON_COMBAT) &&
            ServerFacade::instance().GetDistance2d(master, unit) > sPlayerbotAIConfig.lootDistance)
        {
            if (botAI->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT))
                botAI->TellMaster(chat->FormatWorldobject(unit) + " ignored (far from master).");
            continue;
        }

        if (!bot->InBattleground() && (int)unit->GetLevel() - (int)bot->GetLevel() > 4 && !unit->GetGUID().IsPlayer())
            continue;

        if (Creature* creature = unit->ToCreature())
            if (CreatureTemplate const* CreatureTemplate = creature->GetCreatureTemplate())
                if (CreatureTemplate->rank > CREATURE_ELITE_NORMAL && !AI_VALUE(bool, "can fight elite"))
                    continue;

        if (!bot->IsWithinLOSInMap(unit))
        {
            continue;
        }

        bool inactiveGrindStatus = botAI->rpgInfo.GetStatus() != RPG_WANDER_RANDOM && botAI->rpgInfo.GetStatus() != RPG_IDLE;

        float aggroRange = 30.0f;
        if (unit->ToCreature())
            aggroRange = std::min(30.0f, unit->ToCreature()->GetAggroRange(bot) + 10.0f);
        bool outOfAggro = unit->ToCreature() && bot->GetDistance(unit) > aggroRange;
        if (inactiveGrindStatus && outOfAggro)
        {
            if (needForQuestMap.find(unit->GetEntry()) == needForQuestMap.end())
                needForQuestMap[unit->GetEntry()] = needForQuest(unit);

            if (!needForQuestMap[unit->GetEntry()])
                continue;
        }

        if (group)
        {
            Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
            for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
            {
                Player* member = ObjectAccessor::FindPlayer(itr->guid);
                if (!member || !member->IsAlive())
                    continue;

                float d = member->GetDistance(unit);
                if (!result || d < distance)
                {
                    distance = d;
                    result = unit;
                }
            }
        }
        else
        {
            float newdistance = bot->GetDistance(unit);
            if (!result || (newdistance < distance))
            {
                distance = newdistance;
                result = unit;
            }
        }
    }

    return result;
}

bool GrindTargetValue::needForQuest(Unit* target) { return needForQuest(bot, target); }

// WL: parameterized so pitch-in can also ask "does the MASTER still need
// this mob" (a low-level bot often cannot even hold the synced quest)
bool GrindTargetValue::needForQuest(Player* who, Unit* target)
{
    QuestStatusMap& questMap = who->getQuestStatusMap();
    for (auto& quest : questMap)
    {
        Quest const* questTemplate = sObjectMgr->GetQuestTemplate(quest.first);
        if (!questTemplate)
            continue;

        uint32 questId = questTemplate->GetQuestId();
        if (!questId)
            continue;

        QuestStatus status = who->GetQuestStatus(questId);

        if (status == QUEST_STATUS_INCOMPLETE)
        {
            QuestStatusData const* questStatus = &who->getQuestStatusMap()[questId];

            if (questTemplate->GetQuestLevel() > who->GetLevel() + 5)
                continue;

            for (int j = 0; j < QUEST_OBJECTIVES_COUNT; j++)
            {
                int32 entry = questTemplate->RequiredNpcOrGo[j];

                if (entry && entry > 0)
                {
                    int required = questTemplate->RequiredNpcOrGoCount[j];
                    int available = questStatus->CreatureOrGOCount[j];

                    if (required && available < required && target->GetEntry() == uint32(entry))
                        return true;
                }
            }
        }
    }

    if (CreatureTemplate const* data = sObjectMgr->GetCreatureTemplate(target->GetEntry()))
    {
        if (uint32 lootId = data->lootid)
        {
            if (LootTemplates_Creature.HaveQuestLootForPlayer(lootId, who))
            {
                return true;
            }
        }
    }

    return false;
}

uint32 GrindTargetValue::GetTargetingPlayerCount(Unit* unit)
{
    Group* group = bot->GetGroup();
    if (!group)
        return 0;

    uint32 count = 0;
    Group::MemberSlotList const& groupSlot = group->GetMemberSlots();
    for (Group::member_citerator itr = groupSlot.begin(); itr != groupSlot.end(); itr++)
    {
        Player* member = ObjectAccessor::FindPlayer(itr->guid);
        if (!member || !member->IsAlive() || member == bot)
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(member);
        if ((botAI && *botAI->GetAiObjectContext()->GetValue<Unit*>("current target") == unit) ||
            (!botAI && member->GetTarget() == unit->GetGUID()))
            ++count;
    }

    return count;
}

// WOW Legends "pitch in" (v1.4.0 The Living Party). Toggles live in
// mod-wowlegends (wowlegends_botflags.cpp); plain externs keep this module
// free of cross-module headers.
extern bool WlBotPitchInEnabled();
extern bool WlBotGuideEnabled();
extern float WlBotPitchInRadius();

Unit* WlPitchInTargetValue::Calculate()
{
    // WL: stand down while a mod-dungeon-clear run drives this bot's group.
    // Pitch-in tagging a quest mob mid-route is an EXTRA PULL that sabotages
    // the DC pull planner. The value is registered only when that module is
    // present and non-null only while a run is active - a plain read, safe
    // either way.
    if (UntypedValue* dcTank = context->GetUntypedValue("dungeon clear party tank"))
        if (Value<Player*>* v = dynamic_cast<Value<Player*>*>(dcTank))
            if (v->Get())
                return nullptr;

    // WL: stand down while THIS bot is guiding an escort. Pitch-in (3.8)
    // outranks the guide's move action (1.8), so a quest mob near the
    // escortee yanked the guide into combat every few steps - which read
    // as "the guide wanders in circles" on the road. Gated on the guide
    // toggle too: with guiding disabled a stale label must not suppress
    // pitch-in (the move driver that clears labels is dormant then).
    if (WlBotGuideEnabled()
        && !context->GetValue<std::string>("wl guide label")->Get().empty())
        return nullptr;

    // decision narration, one verdict per ~3s per bot: ALWAYS mirrored to
    // the debug log (visible at LogLevel debug), and whispered to the master
    // when the bot runs "nc +debug grind"
    bool dbg = false;
    {
        time_t const nextDbg = context->GetValue<time_t>("last said", "wl_pitchin_dbg")->Get();
        if (time(nullptr) >= nextDbg)
        {
            context->GetValue<time_t>("last said", "wl_pitchin_dbg")->Set(time(nullptr) + 3);
            dbg = true;
        }
    }
    bool const dbgWhisper = dbg && botAI->HasStrategy("debug grind", BotState::BOT_STATE_NON_COMBAT);
    auto bail = [&](char const* why) -> Unit*
    {
        if (dbg)
            LOG_DEBUG("playerbots", "[pitch in] {}: {}", bot->GetName(), why);
        if (dbgWhisper)
            botAI->TellMaster(std::string("pitch in: ") + why);
        return nullptr;
    };

    if (!WlBotPitchInEnabled())
        return bail("disabled by conf");

    Group* group = bot->GetGroup();
    if (!group)
        return bail("not in a group");

    Player* master = GetMaster();
    if (!master || master == bot || GET_PLAYERBOT_AI(master))
        return bail("no real-player master");

    if (master->GetGroup() != group)
        return bail("master not in my group");

    if (!master->IsAlive() || master->IsBeingTeleported() || master->GetMapId() != bot->GetMapId())
        return bail("master unavailable");

    // only when the party is actually settled: a mounted/moving/fighting
    // master means travel or an ongoing fight - never peel off behind him
    // (and a still-mounted bot must not open combat mounted)
    if (master->IsMounted() || master->isMoving() || master->IsInCombat() || bot->IsMounted())
        return bail("waiting: you are mounted/moving/fighting (or I am mounted)");

    // Open world only: pulling ahead of the party in dungeons/raids/BGs is
    // exactly what a helpful bot must NOT do.
    if (!bot->GetMap() || bot->GetMap()->Instanceable())
        return bail("not in the open world");

    GuidVector targets = *context->GetValue<GuidVector>("possible targets");
    if (targets.empty())
        return bail("no possible targets nearby");

    // one grid scan + one quest-need lookup per entry, shared by every
    // escalation pass below
    std::unordered_map<uint32, bool> needForQuestMap;
    uint32 const memberCount = group->GetMembersCount();
    WlPitchInScanStats stats;
    for (uint32 assistCount = 0; assistCount < memberCount; ++assistCount)
    {
        bool anyBusyRejected = false;
        if (Unit* target = FindQuestMobNearMaster(master, targets, needForQuestMap, assistCount,
                anyBusyRejected, (dbg && assistCount == 0) ? &stats : nullptr))
        {
            if (dbg)
                LOG_DEBUG("playerbots", "[pitch in] {}: -> {}", bot->GetName(), target->GetName());
            if (dbgWhisper)
                botAI->TellMaster("전투 지원 대상: -> " + target->GetName());
            return target;
        }

        // nothing was rejected for already being worked on, so a higher
        // assist count cannot change the outcome
        if (!anyBusyRejected)
            break;
    }

    if (dbg)
        LOG_DEBUG("playerbots", "[pitch in] {}: no pick ({})", bot->GetName(), stats.Line());
    if (dbgWhisper)
        botAI->TellMaster("전투 지원 대상을 찾지 못함 (" + stats.Line() + ")");
    return nullptr;
}

std::string WlPitchInScanStats::Line() const
{
    return "seen=" + std::to_string(seen)
        + " owned=" + std::to_string(playerOwned)
        + " role=" + std::to_string(role)
        + " attack=" + std::to_string(invalid)
        + " z=" + std::to_string(zdiff)
        + " leash=" + std::to_string(leash)
        + " lvl=" + std::to_string(level)
        + " elite=" + std::to_string(elite)
        + " busy=" + std::to_string(busy)
        + " quest=" + std::to_string(quest)
        + " los=" + std::to_string(los);
}

Unit* WlPitchInTargetValue::FindQuestMobNearMaster(Player* master, GuidVector const& targets,
    std::unordered_map<uint32, bool>& needForQuestMap, uint32 assistCount, bool& anyBusyRejected,
    WlPitchInScanStats* stats)
{
    float const radius = WlBotPitchInRadius();
    float distance = 0;
    Unit* result = nullptr;
    auto count = [stats](uint32 WlPitchInScanStats::* field)
    {
        if (stats)
            ++(stats->*field);
    };

    for (ObjectGuid const guid : targets)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive() || !unit->IsInWorld() || unit->IsDuringRemoveFromWorld())
            continue;

        Creature* creature = unit->ToCreature();
        if (!creature)
            continue;

        count(&WlPitchInScanStats::seen);

        // never player-controlled aliases (pets, totems, guardians): they
        // give no quest credit and attacking one PvP-flags the bot
        if (creature->IsPet() || creature->IsTotem() || unit->IsControlledByPlayer() ||
            unit->GetCharmerOrOwnerGUID().IsPlayer())
        {
            count(&WlPitchInScanStats::playerOwned);
            continue;
        }

        // never mobs with a role (questgivers, vendors, trainers ...)
        if (!bot->IsHostileTo(unit) && unit->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
        {
            count(&WlPitchInScanStats::role);
            continue;
        }

        if (!bot->IsValidAttackTarget(unit))
        {
            count(&WlPitchInScanStats::invalid);
            continue;
        }

        // grind's INTERACTION_DISTANCE (5.5y) height gate rejects nearly the
        // whole field on rolling terrain (measured on the Mulgore mesas:
        // 11 of 13 mobs); 20y still refuses true cliff-over/under cases
        // while mmaps pathing + the leash + LOS handle the rest
        if (std::fabs(bot->GetPositionZ() - unit->GetPositionZ()) > 20.0f)
        {
            count(&WlPitchInScanStats::zdiff);
            continue;
        }

        // leash: only mobs around the master - help with the party's quest
        // field, never wander off across the zone
        if (ServerFacade::instance().GetDistance2d(master, unit) > radius)
        {
            count(&WlPitchInScanStats::leash);
            continue;
        }

        if ((int)unit->GetLevel() - (int)bot->GetLevel() > 4)
        {
            count(&WlPitchInScanStats::level);
            continue;
        }

        if (CreatureTemplate const* tmpl = creature->GetCreatureTemplate())
            if (tmpl->rank > CREATURE_ELITE_NORMAL && !AI_VALUE(bool, "can fight elite"))
            {
                count(&WlPitchInScanStats::elite);
                continue;
            }

        // cheap cull first: prefer mobs nobody in the party works on yet,
        // then join in (escalated by the caller)
        if (GetTargetingPlayerCount(unit) > assistCount)
        {
            count(&WlPitchInScanStats::busy);
            anyBusyRejected = true;
            continue;
        }

        // the decisive filter: a mob counts when the BOT's or the MASTER's
        // quest log still needs it (kill credit or quest loot) - low-level
        // bots often cannot even hold the synced quest, but helping with
        // the master's objectives is the whole point
        if (needForQuestMap.find(unit->GetEntry()) == needForQuestMap.end())
            needForQuestMap[unit->GetEntry()] =
                needForQuest(unit) || needForQuest(master, unit);

        if (!needForQuestMap[unit->GetEntry()])
        {
            count(&WlPitchInScanStats::quest);
            continue;
        }

        if (!bot->IsWithinLOSInMap(unit))
        {
            count(&WlPitchInScanStats::los);
            continue;
        }

        float const d = bot->GetDistance(unit);
        if (!result || d < distance)
        {
            distance = d;
            result = unit;
        }
    }

    return result;
}
