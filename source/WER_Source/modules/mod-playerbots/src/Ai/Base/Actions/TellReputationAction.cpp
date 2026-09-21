/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TellReputationAction.h"
#include "Event.h"
#include "PlayerbotAI.h"
#include "ReputationMgr.h"
#include "SharedDefines.h"
#include "World.h"
#include <algorithm>

std::string TellReputationAction::BuildReputationLine(FactionEntry const* entry)
{
    ReputationMgr& repMgr = bot->GetReputationMgr();
    ReputationRank rank = repMgr.GetRank(entry);
    int32 reputation = repMgr.GetReputation(entry->ID);

    std::ostringstream out;
    LocaleConstant const locale = sWorld->GetDefaultDbcLocale();
    bool const korean = locale == LOCALE_koKR;
    char const* name = entry->name[locale];
    out << (name && *name ? name : entry->name[LOCALE_enUS]) << ": |cff";

    switch (rank)
    {
        case REP_HATED:
            out << (korean ? "cc2222매우 적대적" : "cc2222hated");
            break;
        case REP_HOSTILE:
            out << (korean ? "ff0000적대적" : "ff0000hostile");
            break;
        case REP_UNFRIENDLY:
            out << (korean ? "ee6622약간 적대적" : "ee6622unfriendly");
            break;
        case REP_NEUTRAL:
            out << (korean ? "ffff00중립적" : "ffff00neutral");
            break;
        case REP_FRIENDLY:
            out << (korean ? "00ff00약간 우호적" : "00ff00friendly");
            break;
        case REP_HONORED:
            out << (korean ? "00ff88우호적" : "00ff88honored");
            break;
        case REP_REVERED:
            out << (korean ? "00ffcc매우 우호적" : "00ffccrevered");
            break;
        case REP_EXALTED:
            out << (korean ? "00ffff확고한 동맹" : "00ffffexalted");
            break;
        default:
            out << (korean ? "808080알 수 없음" : "808080unknown");
            break;
    }

    out << "|cffffffff";

    int32 base = ReputationMgr::Reputation_Cap + 1;
    for (int32 i = MAX_REPUTATION_RANK - 1; i >= rank; --i)
        base -= ReputationMgr::PointsInRank[i];

    out << " (" << (reputation - base) << "/" << ReputationMgr::PointsInRank[rank] << ")";
    return out.str();
}

bool TellReputationAction::Execute(Event event)
{
    std::string const param = event.getParam();
    if (param == "all")
    {
        ReputationMgr& repMgr = bot->GetReputationMgr();
        std::vector<std::string> lines;

        FactionStateList const& stateList = repMgr.GetStateList();
        lines.reserve(stateList.size());

        for (auto const& itr : stateList)
        {
            FactionState const& faction = itr.second;
            if (!(faction.Flags & FACTION_FLAG_VISIBLE))
                continue;

            if (faction.Flags & (FACTION_FLAG_HIDDEN | FACTION_FLAG_INVISIBLE_FORCED) &&
                !(faction.Flags & FACTION_FLAG_SPECIAL))
                continue;

            FactionEntry const* entry = sFactionStore.LookupEntry(faction.ID);
            if (!entry)
                continue;

            lines.push_back(BuildReputationLine(entry));
        }

        std::sort(lines.begin(), lines.end());

        botAI->TellMaster("=== 평판 ===");
        for (auto const& line : lines)
            botAI->TellMaster(line);

        return true;
    }

    Player* master = GetMaster();
    if (!master)
        return false;

    ObjectGuid selection = master->GetTarget();
    if (selection.IsEmpty())
        return false;

    Unit* unit = ObjectAccessor::GetUnit(*master, selection);
    if (!unit)
        return false;

    FactionTemplateEntry const* factionTemplate = unit->GetFactionTemplateEntry();

    FactionEntry const* entry = sFactionStore.LookupEntry(factionTemplate->faction);
    if (!entry)
        return false;

    botAI->TellMaster(BuildReputationLine(entry));

    return true;
}
