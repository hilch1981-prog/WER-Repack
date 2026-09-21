/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - No hearthstone cooldown (community request, Boethiah)
 *
 * Removes the 30-minute hearthstone cooldown so a solo/small-group player can
 * hearth, do a thing, and hearth back without the wait that only exists to pace
 * a raiding server. Pure convenience: it changes no combat, no rewards and no
 * world state, so a server running it is still playing the same game.
 *
 * WHY THE HOOK IS THE TELEPORT AND NOT THE CAST (verified in Spell.cpp,
 * 2026-07-25): inside Spell::_cast() the cooldown is written first -
 * SendSpellCooldown() at
 * :3913 - and the teleport effect only runs afterwards at handle_immediate()
 * :3969. So OnPlayerSpellCast fires BEFORE the cooldown exists and clearing it
 * there is a silent no-op; the teleport hook fires after, when there is
 * something to remove. FindCurrentSpellBySpellId() keeps us to actual
 * hearthstone casts, so ordinary teleports (portals, summons, .tele) are
 * untouched.
 *
 * SendClearCooldown() matters as much as RemoveSpellCooldown(): the first drops
 * the server's cooldown, the second tells the client to un-grey the button. The
 * 3.3.5 client keeps its own Spell.dbc copy of the 30-minute timer, so without
 * the explicit clear packet the item can look unusable while the server would
 * happily allow it.
 *
 * Applies to real players and bots alike - a bot that can hearth on request
 * behaves like the player standing next to it, and it retires the
 * "My hearthstone is still cooling down." refusal in the AI-chat order bridge.
 * Independent of The Pilgrim's Way, which forbids the hearthstone item itself
 * via OnPlayerCanUseItem and so still holds with this enabled.
 *
 * Default OFF - the owner opts in. Toggle
 * WowLegends.NoHearthstoneCooldown.Enabled (applies on worldserver restart,
 * like every other module conf value).
 */

#include "ScriptMgr.h"
#include "Player.h"
#include "Configuration/Config.h"
#include <atomic>

namespace
{
    std::atomic<bool> g_enabled{false};

    // Hearthstone (item 6948 casts this; its item cooldown is -1 = "use the
    // spell's own", so this single id carries the whole 30-minute timer).
    constexpr uint32 SPELL_HEARTHSTONE = 8690;
    // "There's No Place Like Home" - the same go-home effect on the
    // hearthstone-equivalent items, kept in step so those behave identically.
    constexpr uint32 SPELL_NO_PLACE_LIKE_HOME = 39937;

    void ClearHearthCooldown(Player* player, uint32 spellId)
    {
        player->RemoveSpellCooldown(spellId, true);
        player->SendClearCooldown(spellId, player);
    }
}

class WowLegendsHearthstonePlayer : public PlayerScript
{
public:
    WowLegendsHearthstonePlayer() : PlayerScript("WowLegendsHearthstonePlayer",
        { PLAYERHOOK_ON_BEFORE_TELEPORT })
    {
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 /*mapid*/, float /*x*/,
        float /*y*/, float /*z*/, float /*orientation*/, uint32 /*options*/,
        Unit* /*target*/) override
    {
        // hot path: every teleport in the world passes through here, so the
        // toggle and the "was this actually a hearthstone?" test come first
        if (!g_enabled.load() || !player)
            return true;

        if (player->FindCurrentSpellBySpellId(SPELL_HEARTHSTONE))
            ClearHearthCooldown(player, SPELL_HEARTHSTONE);
        else if (player->FindCurrentSpellBySpellId(SPELL_NO_PLACE_LIKE_HOME))
            ClearHearthCooldown(player, SPELL_NO_PLACE_LIKE_HOME);

        return true;   // never block the teleport itself
    }
};

class WowLegendsHearthstoneWorld : public WorldScript
{
public:
    WowLegendsHearthstoneWorld() : WorldScript("WowLegendsHearthstoneWorld",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>(
            "WowLegends.NoHearthstoneCooldown.Enabled", false);
    }
};

void AddWowLegendsHearthstoneScripts()
{
    new WowLegendsHearthstonePlayer();
    new WowLegendsHearthstoneWorld();
}
