/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - "Speech Governor": a single arbitration gate that every bot
 * CHATTER source clears before it speaks. With several chat features live at
 * once (AI replies, recognition greetings, ambient one-liners, two-bot
 * scenes, the voice-card chatter) bots could talk over each other into a wall
 * of text. The governor bounds the CADENCE of bot speech per small area over a
 * short window, with priority tiers: low-value ambient filler is dropped first
 * when an area is already noisy, higher-value lines survive longer, and a
 * player's DIRECTED reply never even asks the governor (it bypasses this).
 *
 * Because the ambient sources ask BEFORE calling the LLM, a suppressed line
 * costs zero tokens - a direct win for free / Ollama hosts.
 *
 * Called only from the main thread in practice (chat hooks + world-update),
 * but mutex-guarded anyway. State is a bounded sliding window; nothing stored.
 */

#include "Player.h"
#include "Timer.h"

#include <cmath>
#include <deque>
#include <mutex>

extern bool WlSpeechGovernorEnabled();

namespace
{
    constexpr uint32 WINDOW_MS = 9000;   // how far back "recently" reaches
    constexpr float  CELL_YD   = 55.0f;  // ~"one earful" of a player
    constexpr size_t HARD_CAP  = 4096;   // absolute memory backstop

    struct Emission
    {
        uint32 ms;
        uint64 cell;
        uint64 beat;
    };

    std::mutex g_mtx;
    std::deque<Emission> g_recent;   // time-ordered (push_back = newest)

    uint64 CellKey(uint32 mapId, float x, float y)
    {
        // world coords are +-17066 -> cell index +-~310; a +1024 offset keeps
        // both axes non-negative and inside 11 bits (0..2047), so the three
        // fields pack into DISJOINT bit ranges (no XOR overlap / collisions).
        uint64 const cx = uint64(int32(std::floor(x / CELL_YD)) + 1024) & 0x7FF;
        uint64 const cy = uint64(int32(std::floor(y / CELL_YD)) + 1024) & 0x7FF;
        return (uint64(mapId) << 22) | (cx << 11) | cy;
    }

    // how many recent lines an area tolerates before THIS priority is held.
    // higher priority = higher ceiling = harder to suppress.
    int Ceiling(int prio)
    {
        switch (prio)
        {
            case 0:  return 2;   // ambient filler - dropped first
            case 1:  return 3;   // narration
            case 2:  return 3;   // greeting
            default: return 4;   // group reply / everything higher
        }
    }
}

// prio: 0 ambient, 1 narration, 2 greeting, 3 group reply. `beat` (optional,
// 0 = none) also dedupes the SAME moment in an area (e.g. two bots about to
// narrate one shared kill). Returns true = go ahead (the emission is recorded),
// false = stay quiet this time. Disabled or null => always true (no-op).
bool WlSpeechAllow(Player* who, int prio, uint64 beat)
{
    if (!WlSpeechGovernorEnabled() || !who)
        return true;

    uint32 const now = getMSTime();
    uint64 const cell = CellKey(who->GetMapId(),
        who->GetPositionX(), who->GetPositionY());

    std::lock_guard<std::mutex> lock(g_mtx);

    // drop expired entries from the front (deque is time-ordered)
    while (!g_recent.empty() && now - g_recent.front().ms > WINDOW_MS)
        g_recent.pop_front();

    int count = 0;
    for (Emission const& e : g_recent)
    {
        if (e.cell != cell)
            continue;
        if (beat && e.beat == beat)
            return false;            // this exact beat was already voiced here
        ++count;
    }
    if (count >= Ceiling(prio))
        return false;                // the area is already noisy enough

    g_recent.push_back({ now, cell, beat });
    if (g_recent.size() > HARD_CAP)
        g_recent.pop_front();
    return true;
}
