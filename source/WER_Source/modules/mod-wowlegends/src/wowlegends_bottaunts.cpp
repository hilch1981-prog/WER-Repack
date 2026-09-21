/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - bot faction combat taunts
 *
 * When WowLegends.BotTaunts.Enabled is on, an AI playerbot shouts a faction
 * taunt at an opposite-faction player the moment it ENGAGES them, chosen by the
 * target's faction and emitted in LANG_UNIVERSAL so the target can actually
 * read it. Throttled per bot so it stays flavour, not spam.
 *
 * We fire from the combat-enter hook (a direct, reliable Say/Yell) rather than
 * the playerbot action engine: a low-priority "say" action is always outranked
 * by the bot's combat actions and never gets its turn, so a hook is both simpler
 * and actually works. 100% mod-wowlegends; no mod-playerbots edits. The toggle
 * itself lives in wowlegends_botflags.cpp (WlBotTauntsEnabled()).
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "WorldSession.h"
#include "SharedDefines.h"
#include "Random.h"
#include "Log.h"
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Defined in wowlegends_botflags.cpp.
extern bool WlBotTauntsEnabled();

namespace
{
    // Faction-targeted taunt lines. A leading "/y " makes it a yell; <target>
    // is replaced with the enemy's name. Gruff faction banter only - no real
    // slurs. (Picked by the TARGET's faction, so a bot uses these vs an enemy.)
    std::vector<std::string> const g_tauntAlliance = {
        "Die, Alliance scum!",
        "Crawl back to Stormwind, <target>!",
        "The Horde will bury you, <target>!",
        "You picked the wrong fight, Alliance dog!",
        "Lok'tar ogar, <target> - victory or death, and it won't be yours!",
        "Your king can't save you now, <target>!",
        "Another Alliance whelp for the pyre!",
        "/y For the Horde! Down with the Alliance!",
    };
    std::vector<std::string> const g_tauntHorde = {
        "Die, Horde scum!",
        "Back to Orgrimmar in a box, <target>!",
        "For the Alliance - and against filth like you, <target>!",
        "You savages don't belong here, <target>!",
        "By the Light, I'll end you, <target>!",
        "Run home to your warchief, <target>!",
        "Another Horde brute who bit off more than he could chew!",
        "/y For the Alliance! The Horde falls today!",
    };

    // Per-bot taunt cooldown (guid -> earliest next taunt time). World-thread.
    std::mutex g_tauntCdMutex;
    std::unordered_map<uint32, time_t> g_tauntCd;

    bool IsBotPlayer(Player* p)
    {
        return p && p->GetSession() && p->GetSession()->IsBot();
    }
}

class WowLegendsBotTauntPlayer : public PlayerScript
{
public:
    WowLegendsBotTauntPlayer() : PlayerScript("WowLegendsBotTauntPlayer",
        { PLAYERHOOK_ON_PLAYER_ENTER_COMBAT }) { }

    void OnPlayerEnterCombat(Player* player, Unit* enemy) override
    {
        if (!WlBotTauntsEnabled() || !player || !enemy)
            return;
        if (!IsBotPlayer(player))                       // only bots taunt
            return;
        Player* foe = enemy->ToPlayer();
        if (!foe || foe == player)                      // only against players
            return;
        if (player->GetTeamId() == foe->GetTeamId())    // opposite faction only
            return;

        uint32 const guid = player->GetGUID().GetCounter();
        time_t const now = time(nullptr);
        {
            std::lock_guard<std::mutex> lock(g_tauntCdMutex);
            auto it = g_tauntCd.find(guid);
            if (it != g_tauntCd.end() && it->second > now)
                return;
            g_tauntCd[guid] = now + 30 + urand(0, 30);  // ~30-60s between taunts
        }

        bool const foeAlliance = foe->GetTeamId() == TEAM_ALLIANCE;
        std::vector<std::string> const& lines = foeAlliance ? g_tauntAlliance : g_tauntHorde;
        std::string line = lines[urand(0, uint32(lines.size() - 1))];

        std::size_t const pos = line.find("<target>");
        if (pos != std::string::npos)
            line.replace(pos, 8, foe->GetName());

        if (line.rfind("/y ", 0) == 0)
            player->Yell(line.substr(3), LANG_UNIVERSAL);
        else
            player->Say(line, LANG_UNIVERSAL);

        LOG_DEBUG("server", "[bottaunt] {} taunts {} ({})",
            player->GetName(), foe->GetName(), foeAlliance ? "alliance" : "horde");
    }
};

void AddWowLegendsBotTauntScripts()
{
    new WowLegendsBotTauntPlayer();
}
