/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "ChooseTargetActions.h"
#include "ChooseRpgTargetAction.h"
#include "Event.h"
#include "LootObjectStack.h"
#include "NewRpgStrategy.h"
#include "Playerbots.h"
#include "PossibleRpgTargetsValue.h"
#include "PvpTriggers.h"
#include "RtiTargetValue.h"
#include "ServerFacade.h"

bool AttackEnemyPlayerAction::isUseful()
{
    if (PlayerHasFlag::IsCapturingFlag(bot))
        return false;

    return !sPlayerbotAIConfig.IsPvpProhibited(bot->GetZoneId(), bot->GetAreaId());
}

bool AttackEnemyFlagCarrierAction::isUseful()
{
    Unit* target = context->GetValue<Unit*>("enemy flag carrier")->Get();
    return target && ServerFacade::instance().IsDistanceLessOrEqualThan(ServerFacade::instance().GetDistance2d(bot, target), 100.0f) &&
           PlayerHasFlag::IsCapturingFlag(bot);
}

bool AggressiveTargetAction::isUseful()
{
    if (bot->IsInCombat())
        return false;

    return true;
}

bool DropTargetAction::Execute(Event /*event*/)
{
    Unit* target = context->GetValue<Unit*>("current target")->Get();
    if (target && target->isDead())
    {
        ObjectGuid guid = target->GetGUID();
        if (guid)
            context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);
    }

    // ObjectGuid pullTarget = context->GetValue<ObjectGuid>("pull target")->Get();
    // GuidVector possible = botAI->GetAiObjectContext()->GetValue<GuidVector>("possible targets no los")->Get();

    // if (pullTarget && find(possible.begin(), possible.end(), pullTarget) == possible.end())
    // {
    //     context->GetValue<ObjectGuid>("pull target")->Set(ObjectGuid::Empty);
    // }

    context->GetValue<Unit*>("current target")->Set(nullptr);

    bot->SetTarget(ObjectGuid::Empty);
    bot->SetSelection(ObjectGuid());
    botAI->ChangeEngine(BOT_STATE_NON_COMBAT);
    if (bot->getClass() == CLASS_HUNTER) // Check for Hunter Class
    {
        Spell const* spell = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL); // Get the current spell being cast by the bot
        if (spell && spell->m_spellInfo->Id == 75) //Check spell is not nullptr before accessing m_spellInfo
            bot->InterruptSpell(CURRENT_AUTOREPEAT_SPELL); // Interrupt Auto Shot
    }
    bot->AttackStop();

    // if (Pet* pet = bot->GetPet())
    // {
    //     if (CreatureAI* creatureAI = ((Creature*)pet)->AI())
    //     {
    //         pet->SetReactState(REACT_PASSIVE);
    //         pet->GetCharmInfo()->SetCommandState(COMMAND_FOLLOW);
    //         pet->GetCharmInfo()->SetIsCommandFollow(true);
    //         pet->AttackStop();
    //         pet->GetCharmInfo()->IsReturning();
    //         pet->GetMotionMaster()->MoveFollow(bot, PET_FOLLOW_DIST, pet->GetFollowAngle());
    //     }
    // }

    return true;
}

bool AttackAnythingAction::Execute(Event event)
{
    bool result = AttackAction::Execute(event);
    if (result)
    {
        if (Unit* grindTarget = GetTarget())
        {
            context->GetValue<ObjectGuid>("pull target")->Set(grindTarget->GetGUID());
            bot->GetMotionMaster()->Clear();
            // bot->StopMoving();
        }
    }

    return result;
}

bool AttackAnythingAction::isUseful()
{
    if (!bot || !botAI)  // Prevents invalid accesses
        return false;

    if (!botAI->AllowActivity(GRIND_ACTIVITY))  // Bot cannot be active
        return false;

    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        return false;

    if (bot->IsInCombat())
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsInWorld())  // Checks if the target is valid and in the world
        return false;

    std::string const name = std::string(target->GetName());
    if (!name.empty() &&
        (name.find("Dummy") != std::string::npos ||
         name.find("Charge Target") != std::string::npos ||
         name.find("Melee Target") != std::string::npos ||
         name.find("Ranged Target") != std::string::npos))
    {
        return false;
    }

    return true;
}

bool AttackAnythingAction::isPossible() { return GetTarget() && AttackAction::isPossible(); }

bool WlPitchInAttackAction::Execute(Event event)
{
    // unlike AttackAnythingAction, do NOT set "pull target": targets are
    // already leashed to the master, and the value is never cleared on alt
    // bots, which would suppress PossibleAddsTrigger forever after
    bool const result = AttackAction::Execute(event);
    if (result)
        bot->GetMotionMaster()->Clear();

    return result;
}

bool WlPitchInAttackAction::isUseful()
{
    if (!bot || !botAI)
        return false;

    if (bot->IsInCombat())
        return false;

    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        return false;

    if (!botAI->AllowActivity(GRIND_ACTIVITY))
        return false;

    Unit* target = GetTarget();
    if (!target || !target->IsInWorld())
        return false;

    std::string const name = std::string(target->GetName());
    if (!name.empty() &&
        (name.find("Dummy") != std::string::npos ||
         name.find("Charge Target") != std::string::npos ||
         name.find("Melee Target") != std::string::npos ||
         name.find("Ranged Target") != std::string::npos))
    {
        return false;
    }

    return true;
}

bool DpsAssistAction::isUseful()
{
    if (PlayerHasFlag::IsCapturingFlag(bot))
        return false;

    return true;
}

bool AttackRtiTargetAction::Execute(Event /*event*/)
{
    Unit* rtiTarget = AI_VALUE(Unit*, "rti target");

    // Fallback: if the "rti target" value did not resolve a valid unit yet,
    // try to resolve the raid icon directly from the group.
    if (!rtiTarget)
    {
        if (Group* group = bot->GetGroup())
        {
            std::string const rti = AI_VALUE(std::string, "rti");
            int32 const index = RtiTargetValue::GetRtiIndex(rti);
            if (index >= 0)
            {
                ObjectGuid const guid = group->GetTargetIcon(index);
                if (!guid.IsEmpty())
                    rtiTarget = botAI->GetUnit(guid);
            }
        }
    }

    if (rtiTarget && rtiTarget->IsInWorld() && rtiTarget->GetMapId() == bot->GetMapId())
    {
        botAI->GetAiObjectContext()->GetValue<GuidVector>("prioritized targets")->Set({rtiTarget->GetGUID()});
        bool result = Attack(botAI->GetUnit(rtiTarget->GetGUID()));
        if (result)
        {
            context->GetValue<ObjectGuid>("pull target")->Set(rtiTarget->GetGUID());
            return true;
        }
    }
    else
        botAI->TellError("I dont see my rti attack target");

    return false;
}

bool AttackRtiTargetAction::isUseful()
{
    if (botAI->ContainsStrategy(STRATEGY_TYPE_HEAL))
        return false;

    return true;
}

// WOW Legends "idle life". Toggle lives in mod-wowlegends
// (wowlegends_botflags.cpp); plain extern keeps this module header-free.
extern bool WlBotIdleLifeEnabled();

// the ONLY state in which idle-life behaviors may run: a settled real-player
// master in the open world. Any movement, mount, combat or teleport flips
// this false the same tick and follow/combat win again. Shared (extern) with
// FollowActions.cpp, which releases nearby/seated bots from their formation
// slot in exactly this state so idle moves aren't yanked back.
bool WlIdleLifeMasterSettled(PlayerbotAI* botAI, Player* bot)
{
    Player* master = botAI->GetMaster();
    if (!master || master == bot || GET_PLAYERBOT_AI(master))
        return false;

    if (!bot->GetGroup() || bot->GetGroup() != master->GetGroup())
        return false;

    if (!master->IsAlive() || master->IsBeingTeleported() || master->GetMapId() != bot->GetMapId())
        return false;

    if (master->IsMounted() || master->isMoving() || master->IsInCombat())
        return false;

    if (bot->IsMounted() || bot->IsInCombat())
        return false;

    if (!bot->GetMap() || bot->GetMap()->Instanceable())
        return false;

    return true;
}

// WOW Legends Warband Camp: is this bot parked at a camp, and where is the
// fire? Lives in mod-wowlegends (registry filled by `.camp alts` / the
// auto-gather), plain extern as usual.
extern bool WlWarbandCampParked(uint32 lowGuid, float& cx, float& cy, float& cz);

bool WlIdleStrollAction::isUseful()
{
    if (!WlBotIdleLifeEnabled())
        return false;

    // Camp-parked alts stroll around the CAMP, with no master, no group and
    // no follow required - the whole point is that they live there while the
    // owner plays someone else. `$follow` (re-adding the follow strategy) is
    // the natural exit and must win, so parked mode stands down the moment
    // follow is back on.
    float cx, cy, cz;
    if (WlWarbandCampParked(bot->GetGUID().GetCounter(), cx, cy, cz) &&
        !botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
    {
        if (!bot->IsAlive() || bot->IsInCombat() || bot->IsMounted() ||
            bot->IsSitState())
            return false;
        // Summoned away / dragged off: don't march it back across the zone,
        // just stop being camp scenery.
        if (bot->GetDistance(cx, cy, cz) > 60.0f)
            return false;
        return true;
    }

    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        return false;

    Player* master = botAI->GetMaster();
    if (!master || master->IsSitState())   // the campfire circle owns sitting
        return false;

    // a straggler catches up at run speed via follow first; idle-life only
    // animates bots that are already home
    if (bot->GetDistance(master) > 12.0f)
        return false;

    return WlIdleLifeMasterSettled(botAI, bot);
}

bool WlIdleStrollAction::Execute(Event /*event*/)
{
    // The anchor the legs orbit: the CAMP FIRE for a parked alt, the MASTER
    // for an ordinary grouped companion. Parked alts have no master to lean
    // on, so this must be resolved before the master null-check.
    float ax, ay, az;
    bool const parked =
        WlWarbandCampParked(bot->GetGUID().GetCounter(), ax, ay, az) &&
        !botAI->HasStrategy("follow", BOT_STATE_NON_COMBAT);

    Player* master = botAI->GetMaster();
    if (!parked)
    {
        if (!master)
            return false;
        ax = master->GetPositionX();
        ay = master->GetPositionY();
        az = master->GetPositionZ();
    }

    // fetched once per stroll (~20s): known hostiles around the bot, used to
    // refuse legs that would end inside anything's aggro bubble
    GuidVector hostiles = *context->GetValue<GuidVector>("possible targets");

    Map* map = bot->GetMap();
    for (int i = 0; i < 3; ++i)
    {
        // a short leg anchored to the anchor point, so bots drift around it
        // instead of wandering off. Camp legs reach a little further - a
        // camp is a place, a master is a person.
        float const angle = (float)rand_norm() * 2 * static_cast<float>(M_PI);
        float const dist = parked ? frand(3.0f, 12.0f) : frand(3.0f, 9.0f);
        float x = ax + dist * cos(angle);
        float y = ay + dist * sin(angle);
        float z = az;

        if (!map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                                  bot->GetPositionZ(), x, y, z))
            continue;
        if (map->IsInWater(bot->GetPhaseMask(), x, y, z, bot->GetCollisionHeight()))
            continue;

        // never amble INTO something: reject a leg that ends inside a
        // hostile's real aggro range when it also brings us closer to it
        bool nearHostile = false;
        for (ObjectGuid const guid : hostiles)
        {
            Unit* u = botAI->GetUnit(guid);
            if (!u || !u->IsAlive())
                continue;

            float aggro = 13.0f;
            if (Creature* c = u->ToCreature())
                aggro = c->GetAggroRange(bot) + 2.0f;

            float const dLeg = u->GetDistance(x, y, z);
            if (dLeg < aggro && dLeg < u->GetDistance(bot))
            {
                nearHostile = true;
                break;
            }
        }
        if (nearHostile)
            continue;

        // amble, don't sprint: walk mode bakes into the launched spline
        bot->m_movementInfo.AddMovementFlag(MOVEMENTFLAG_WALKING);
        if (MoveTo(bot->GetMapId(), x, y, z, false, false, false, true))
            return true;
    }

    // no leg launched: don't leave the walk flag armed for the next spline
    bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_WALKING);
    return false;
}

bool WlCampfireSpotAction::isUseful()
{
    if (!WlBotIdleLifeEnabled())
        return false;

    if (botAI->HasStrategy("stay", BOT_STATE_NON_COMBAT))
        return false;

    Player* master = botAI->GetMaster();
    if (!master || !master->IsSitState() || bot->IsSitState())
        return false;

    // stragglers come home via follow first
    if (bot->GetDistance(master) > 12.0f)
        return false;

    return WlIdleLifeMasterSettled(botAI, bot);
}

bool WlCampfireSpotAction::Execute(Event /*event*/)
{
    Player* master = botAI->GetMaster();
    if (!master)
        return false;

    // sit around a campfire when one is lit close to the master, else around
    // the master themself. Every campfire variant (player-summoned or world
    // doodad) is a spell-focus gameobject with focusId 4 (COOKING). The scan
    // is capped at 6.5y so every seat (radius 3) stays inside the core
    // posture mirror's 10y sit window around the master.
    WorldObject* anchor = master;
    float bestDist = 6.5f;
    GuidVector gos = *context->GetValue<GuidVector>("nearest game objects");
    for (ObjectGuid const guid : gos)
    {
        GameObject* go = botAI->GetGameObject(guid);
        if (!go || !go->isSpawned())
            continue;

        GameObjectTemplate const* info = go->GetGOInfo();
        if (!info || info->type != GAMEOBJECT_TYPE_SPELL_FOCUS
            || info->spellFocus.focusId != 4)
            continue;

        float const d = master->GetDistance(go);
        if (d < bestDist)
        {
            bestDist = d;
            anchor = go;
        }
    }

    // deterministic DISTINCT seats: the bot's join-order position in the
    // group spreads the circle (guid%N would stack two bots on one spot)
    uint32 seatIndex = 0;
    if (Group* group = bot->GetGroup())
    {
        uint32 index = 0;
        for (Group::MemberSlotList::const_iterator itr = group->GetMemberSlots().begin();
             itr != group->GetMemberSlots().end(); ++itr, ++index)
            if (itr->guid == bot->GetGUID())
            {
                seatIndex = index;
                break;
            }
    }
    float const seat = 2 * static_cast<float>(M_PI) * float(seatIndex % 8) / 8.0f;
    float const radius = anchor == master ? 2.5f : 3.0f;
    float x = anchor->GetPositionX() + radius * cos(seat);
    float y = anchor->GetPositionY() + radius * sin(seat);
    float z = anchor->GetPositionZ();

    if (bot->GetDistance(x, y, z) < 1.0f)
    {
        // at the seat: sit down ourselves - the core mirror only covers
        // bots within 10y of the master, and sitting also stops the
        // 2s trigger from re-firing
        if (!bot->isMoving() && !bot->IsSitState())
        {
            bot->SetStandState(UNIT_STAND_STATE_SIT);
            return true;
        }
        return false;
    }

    Map* map = bot->GetMap();
    if (!map->CheckCollisionAndGetValidCoords(bot, bot->GetPositionX(), bot->GetPositionY(),
                                              bot->GetPositionZ(), x, y, z))
        return false;

    bot->m_movementInfo.AddMovementFlag(MOVEMENTFLAG_WALKING);
    bool const moved = MoveTo(bot->GetMapId(), x, y, z, false, false, false, true);
    if (!moved)
        bot->m_movementInfo.RemoveMovementFlag(MOVEMENTFLAG_WALKING);
    return moved;
}

bool WlIdleEmoteAction::isUseful()
{
    if (!WlBotIdleLifeEnabled())
        return false;

    return WlIdleLifeMasterSettled(botAI, bot) && EmoteAction::isUseful();
}
