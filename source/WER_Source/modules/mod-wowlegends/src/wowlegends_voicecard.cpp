/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - "Voice Cards": a deterministic, per-bot personality derived
 * purely from the bot's GUID. Stateless - no DB, no writes - so the SAME bot
 * always has the SAME voice: two orcs sound like different people, and a bot
 * you come to know keeps its character forever (even across restarts, since it
 * is re-derived, not stored).
 *
 * Layered ON TOP of the race/faction persona. It shapes two things:
 *   - the LLM prompt (a temperament + a speech habit), so replies and ambient
 *     lines carry a consistent individual voice;
 *   - how readily the bot pipes up in AMBIENT chatter (talkativeness), which
 *     makes the personality visible even with the LLM switched off.
 *
 * It NEVER gates a directed reply - a quiet bot still answers a whisper;
 * talkativeness only stretches/shrinks the ambient cooldown.
 */

#include "Define.h"   // AzerothCore uint32 typedef

#include <string>

extern bool WlVoiceCardsEnabled();

namespace
{
    // integer avalanche hash (fmix-style) so adjacent GUIDs get unrelated cards
    uint32 VcHash(uint32 x)
    {
        x ^= x >> 16;
        x *= 0x7feb352dU;
        x ^= x >> 15;
        x *= 0x846ca68bU;
        x ^= x >> 16;
        return x;
    }

    // Kept generic (NOT race-specific) so it composes with the race/faction
    // flavour rather than fighting it: a gruff orc and a gruff dwarf differ
    // from their cheerful kin, and from each other via race flavour.
    char const* const TEMPERAMENTS[] = {
        "gruff and blunt",
        "warm and quick to laugh",
        "calm and hard to rattle",
        "proud, and never shy about it",
        "wary, always watching the treeline",
        "dry, with a sardonic streak",
        "earnest and eager",
        "world-weary, like you've seen it all before",
    };
    constexpr uint32 TEMPERAMENT_COUNT = 8;

    char const* const HABITS[] = {
        "You swear by your weapon and your own skill.",
        "You lace your talk with short, gruff exclamations.",
        "You tend to answer a question with a question.",
        "You keep it short - a few blunt words and done.",
        "You like the sound of your own plans and over-explain them.",
        "You keep circling back to home and the old days.",
        "You keep score of favours and debts, and forget nothing.",
        "You're a touch superstitious - omens, luck, old sayings.",
    };
    constexpr uint32 HABIT_COUNT = 8;
}

// The stable personality snippet appended to a bot's persona/prompt. Empty
// when disabled. Uses independent slices of the hash for the two axes so they
// vary independently. It instructs the model to LET the character show, never
// announce it (no "as a gruff orc, I...").
std::string WlVoiceCardPersona(uint32 guidLow)
{
    if (!WlVoiceCardsEnabled())
        return "";

    uint32 const h = VcHash(guidLow);
    char const* temper = TEMPERAMENTS[(h >> 2) % TEMPERAMENT_COUNT];
    char const* habit = HABITS[(h >> 13) % HABIT_COUNT];

    return std::string(" Beyond your people's ways you have your own"
        " character: you are ") + temper + ". " + habit
        + " Let this character colour how you speak - but never announce it,"
        " just let it show.";
}

// Ambient-chatter cooldown multiplier: chatty bots < 1 (speak up more often),
// quiet bots > 1 (speak less), ~half are normal. 1.0 when disabled so the
// pacing is untouched. Never gates directed replies - only ambient frequency.
float WlVoiceCardTalkFactor(uint32 guidLow)
{
    if (!WlVoiceCardsEnabled())
        return 1.0f;

    // separate seed (xor a constant) so talkativeness is independent of the
    // temperament/habit axes above
    uint32 const t = VcHash(guidLow ^ 0x9e3779b9U) % 4; // 0..3
    if (t == 0)
        return 1.8f;   // quiet   (~25%)
    if (t == 3)
        return 0.55f;  // chatty  (~25%)
    return 1.0f;       // normal  (~50%)
}
