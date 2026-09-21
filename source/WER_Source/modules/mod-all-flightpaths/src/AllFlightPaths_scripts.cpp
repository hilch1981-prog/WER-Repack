/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Copyright (C) Aldrynth / VenomekPL
 *
 * Grant all faction-appropriate taxi nodes on login (no discover-as-you-go).
 */

#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "Log.h"
#include "Player.h"
#include "WorldSession.h"
#include "ScriptMgr.h"

namespace
{
    void GrantFactionTaxiNodes(Player* player)
    {
        if (!player)
            return;

        TaxiMask const& factionMask = (player->GetTeamId(true) == TEAM_ALLIANCE)
            ? sAllianceTaxiNodesMask
            : sHordeTaxiNodesMask;

        for (uint32 nodeIdx = 1; nodeIdx < sTaxiNodesStore.GetNumRows(); ++nodeIdx)
        {
            if (!sTaxiNodesStore.LookupEntry(nodeIdx))
                continue;

            uint32 field = (nodeIdx - 1) / 32;
            if (field >= TaxiMaskSize)
                continue;

            uint32 submask = 1u << ((nodeIdx - 1) % 32);
            if (factionMask[field] & submask)
                player->m_taxi.SetTaximaskNode(nodeIdx);
        }
    }
}

class AllFlightPaths_Player : public PlayerScript
{
public:
    AllFlightPaths_Player() : PlayerScript("AllFlightPaths_Player", {
        PLAYERHOOK_ON_LOGIN
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        // Local port: keep account progression unchanged until explicitly enabled.
        if (!player || !sConfigMgr->GetOption<bool>("AllFlightPaths.Enable", false))
            return;

        GrantFactionTaxiNodes(player);

        if (player->GetSession() && sConfigMgr->GetOption<bool>("AllFlightPaths.Announce", false))
            ChatHandler(player->GetSession()).SendSysMessage(
                player->GetSession()->GetSessionDbLocaleIndex() == LOCALE_koKR
                    ? "소속 진영의 모든 비행 경로가 개방되었습니다."
                    : "All flight paths for your faction are unlocked.");
    }
};

class AllFlightPaths_World : public WorldScript
{
public:
    AllFlightPaths_World() : WorldScript("AllFlightPaths_World") { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        if (!sConfigMgr->GetOption<bool>("AllFlightPaths.Enable", false))
            return;

        LOG_INFO("server.loading", "AllFlightPaths: module present (faction taxi nodes on login)");
    }
};

void AddAllFlightPathsScripts()
{
    new AllFlightPaths_Player();
    new AllFlightPaths_World();
}
