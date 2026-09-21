/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_GRINDTARGETVALUE_H
#define PLAYERBOTS_GRINDTARGETVALUE_H

#include "TargetValue.h"

#include <unordered_map>

class PlayerbotAI;
class Unit;

class GrindTargetValue : public TargetValue
{
public:
    GrindTargetValue(PlayerbotAI* botAI, std::string const name = "grind target") : TargetValue(botAI, name) {}

    Unit* Calculate() override;

protected:
    uint32 GetTargetingPlayerCount(Unit* unit);
    Unit* FindTargetForGrinding(uint32 assistCount);
    bool needForQuest(Unit* target);
    bool needForQuest(Player* who, Unit* target);
};

// WOW Legends "pitch in": target chooser for player-owned grouped bots that
// proactively attack mobs the bot's OWN quest log still needs (kill credit or
// quest loot), leashed to the master so the party never scatters. Dormant
// unless grouped under a real-player master in the open world.

// per-filter rejection counters for the "debug grind" narration
struct WlPitchInScanStats
{
    uint32 seen = 0;
    uint32 playerOwned = 0;
    uint32 role = 0;
    uint32 invalid = 0;
    uint32 zdiff = 0;
    uint32 leash = 0;
    uint32 level = 0;
    uint32 elite = 0;
    uint32 busy = 0;
    uint32 quest = 0;
    uint32 los = 0;

    std::string Line() const;
};

class WlPitchInTargetValue : public GrindTargetValue
{
public:
    WlPitchInTargetValue(PlayerbotAI* botAI) : GrindTargetValue(botAI, "pitch in target") {}

    Unit* Calculate() override;

private:
    Unit* FindQuestMobNearMaster(Player* master, GuidVector const& targets,
        std::unordered_map<uint32, bool>& needForQuestMap, uint32 assistCount, bool& anyBusyRejected,
        WlPitchInScanStats* stats);
};

#endif
