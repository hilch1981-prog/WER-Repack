/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - instant "on next swing" abilities
 *
 * Suggested by Boethiah (#suggestions "QoL Rapidfire", 2026-07-18).
 *
 * In 3.3.5 a handful of melee abilities are "on next swing": pressing Heroic
 * Strike does not hit anything, it arms your NEXT white attack to hit harder.
 * Nothing happens when you press the button, which reads as lag or a dead
 * keybind to anyone who did not grow up with it. This makes them behave like
 * every other instant ability.
 *
 * Covered: Heroic Strike, Cleave (Warrior) · Raptor Strike (Hunter) · Rune
 * Strike (Death Knight). Maul is deliberately EXCLUDED - see the block comment
 * on the whitelist below, it would silently break Omen of Clarity.
 *
 * How: AzerothCore decides "is this an on-next-swing spell" in exactly one
 * place - Spell::IsNextMeleeSwingSpell() (Spell.cpp:8104) - and it tests a
 * single DBC attribute bit. Clear that bit while spells are loading and the
 * ability falls through the ordinary instant-cast path. No hooks in combat, no
 * per-swing cost: this runs once at startup and then never again.
 *
 * Deliberate choices:
 *
 *   - WHITELIST, not "every spell that has the bit". Some creature abilities
 *     use on-next-swing too, and silently rewriting boss melee is not what
 *     anyone asked for. Only the five player abilities below are touched.
 *   - Matched by SPELL CHAIN, so every rank is covered without listing ~40
 *     ids - GetFirstSpellInChain() is safe here because LoadSpellRanks()
 *     (World.cpp:411) runs before LoadSpellInfoCustomAttributes() (:420),
 *     which is what fires this hook.
 *   - The attribute check comes FIRST and is the safety net: if one of these
 *     ids turns out not to be an on-next-swing spell after all, this is a
 *     no-op for it rather than a silent behaviour change.
 *   - Both bits are cleared. Only 0x4 is read by the server, but 0x400 is the
 *     same flag as far as the client is concerned and leaving a half-cleared
 *     pair behind for the next person to puzzle over helps nobody.
 *
 * Ships DISABLED. This changes how five abilities work for every player AND
 * every bot's melee rotation on the realm - firmly in "world-changing, owner
 * opts in" territory. (Upstream's standalone module defaults it ON; we do not.)
 *
 * ⚠️ Takes effect at STARTUP ONLY. Spell attributes are read once while the
 * world loads, so `.reload config` will re-read the toggle and change nothing.
 * A worldserver restart is required either way.
 */

#include "ScriptMgr.h"
#include "Configuration/Config.h"
#include "Log.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include <atomic>

namespace
{
    std::atomic<bool> g_enabled{false};

    // Rank 1 of each affected ability. Every higher rank is reached through
    // the spell chain, so this list does not grow with expansions.
    constexpr uint32 SPELL_HEROIC_STRIKE = 78;      // Warrior
    constexpr uint32 SPELL_CLEAVE        = 845;     // Warrior
    constexpr uint32 SPELL_RAPTOR_STRIKE = 2973;    // Hunter
    constexpr uint32 SPELL_RUNE_STRIKE   = 56815;   // Death Knight

    // 🛑 MAUL (6807) IS DELIBERATELY NOT HERE. Do not "fix" this by adding it.
    //
    // Omen of Clarity reads the same DBC bits to decide whether a melee-damage
    // -class spell may proc it (spell_druid.cpp:260):
    //
    //     if (typeMask & PROC_FLAG_DONE_SPELL_MELEE_DMG_CLASS)
    //         return HasAttribute(ON_NEXT_SWING) ||
    //                HasAttribute(ON_NEXT_SWING_NO_DAMAGE);
    //
    // Maul is the only druid ability that clause is there for. Clear its bits
    // and the check returns false, so Omen of Clarity silently stops proccing
    // from Maul - a talent quietly broken by an unrelated convenience toggle,
    // which is exactly the kind of hidden coupling nobody can debug later.
    //
    // If we ever want Maul included, the honest fix is one line in that core
    // script - `|| sSpellMgr->GetFirstSpellInChain(spellInfo->Id) == 6807` -
    // which is behaviour-preserving whether this toggle is on or off. That is
    // a core patch, so it is Kneuma's call, not a silent addition here.

    constexpr uint32 ON_NEXT_SWING_BITS =
        SPELL_ATTR0_ON_NEXT_SWING_NO_DAMAGE | SPELL_ATTR0_ON_NEXT_SWING;

    bool IsWhitelisted(uint32 baseSpellId)
    {
        switch (baseSpellId)
        {
            case SPELL_HEROIC_STRIKE:
            case SPELL_CLEAVE:
            case SPELL_RAPTOR_STRIKE:
            case SPELL_RUNE_STRIKE:
                return true;
            default:
                return false;
        }
    }

    // Counted so startup says exactly what was changed. An owner who flips
    // this on and sees "0 abilities" knows something is wrong with their
    // client data rather than wondering whether the setting took.
    std::atomic<uint32> g_converted{0};
}

class WowLegendsInstantSwingGlobal : public GlobalScript
{
public:
    WowLegendsInstantSwingGlobal()
        : GlobalScript("WowLegendsInstantSwingGlobal",
            { GLOBALHOOK_ON_LOAD_SPELL_CUSTOM_ATTR })
    {
    }

    void OnLoadSpellCustomAttr(SpellInfo* spellInfo) override
    {
        if (!g_enabled.load() || !spellInfo)
            return;

        if (!(spellInfo->Attributes & ON_NEXT_SWING_BITS))
            return;

        if (!IsWhitelisted(sSpellMgr->GetFirstSpellInChain(spellInfo->Id)))
            return;

        spellInfo->Attributes &= ~ON_NEXT_SWING_BITS;
        ++g_converted;
    }
};

class WowLegendsInstantSwingWorld : public WorldScript
{
public:
    WowLegendsInstantSwingWorld()
        : WorldScript("WowLegendsInstantSwingWorld",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        // Read before spells load: LoadConfigSettings() (World.cpp:318) fires
        // this hook, and LoadSpellInfoCustomAttributes() is at :420.
        g_enabled = sConfigMgr->GetOption<bool>(
            "WowLegends.InstantSwing.Enabled", false);
    }

    void OnStartup() override
    {
        if (!g_enabled.load())
            return;

        LOG_INFO("server.loading",
            "[WER] Instant swing: {} ability ranks no longer wait for "
            "the next melee swing.", g_converted.load());
    }
};

void AddWowLegendsInstantSwingScripts()
{
    new WowLegendsInstantSwingGlobal();
    new WowLegendsInstantSwingWorld();
}
