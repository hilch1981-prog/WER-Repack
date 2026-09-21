/* PvP Life local extension; GPL-2.0-or-later. */
#ifndef PVP_LIFE_DUEL_REQUEST_POLICY_H
#define PVP_LIFE_DUEL_REQUEST_POLICY_H
#include <algorithm>
#include <cstdint>

namespace PvPLife
{
    enum class DuelRequestAction { Cast, Wait, AcceptBot, Confirmed, Busy, TimedOut };

    inline DuelRequestAction EvaluateDuelRequest(bool sent, bool anyDuel, bool matchingPair,
        bool targetIsReal, bool bothStarted, std::uint32_t now, std::uint32_t deadline)
    {
        if (anyDuel)
        {
            if (!sent || !matchingPair)
                return DuelRequestAction::Busy;
            return targetIsReal || bothStarted ? DuelRequestAction::Confirmed : DuelRequestAction::AcceptBot;
        }
        if (!sent)
            return DuelRequestAction::Cast;
        return now < deadline ? DuelRequestAction::Wait : DuelRequestAction::TimedOut;
    }

    inline float DuelApproachDistance(float spellRange)
    {
        // Stay comfortably inside the actual spell range; never assume 30 yards.
        return std::max(0.0f, std::min(8.0f, spellRange - 1.0f));
    }

    constexpr std::uint32_t DuelFailureBackoff = 120;
    constexpr std::uint32_t DuelRequestTimeout = 5;
    constexpr std::uint8_t DuelApproachRetries = 12;
}
#endif
