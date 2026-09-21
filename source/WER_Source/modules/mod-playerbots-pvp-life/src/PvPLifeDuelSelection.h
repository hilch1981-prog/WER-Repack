/* PvP Life local extension; GPL-2.0-or-later, matching the module. */
#ifndef PVP_LIFE_DUEL_SELECTION_H
#define PVP_LIFE_DUEL_SELECTION_H
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace PvPLife
{
    // Pools are already shuffled. Prefer underrepresented level bands, but only
    // reserve complete compatible pairs; never change a character's level.
    template<class Candidate>
    std::pair<std::vector<Candidate>, std::vector<Candidate>> SelectDiverseDuelPairs(
        std::vector<Candidate> const& attackers, std::vector<Candidate> const& defenders,
        std::size_t maxPairs, unsigned maxDifference)
    {
        std::pair<std::vector<Candidate>, std::vector<Candidate>> result;
        std::unordered_set<std::uint32_t> used;
        std::array<unsigned, 3> bands{};
        while (result.first.size() < maxPairs)
        {
            Candidate const* bestA = nullptr;
            Candidate const* bestB = nullptr;
            unsigned bestBand = 0;
            unsigned bestScore = std::numeric_limits<unsigned>::max();
            for (auto const& a : attackers)
            {
                if (used.count(a.GuidLow))
                    continue;
                for (auto const& b : defenders)
                {
                    if (a.GuidLow == b.GuidLow || a.Team != b.Team || used.count(b.GuidLow))
                        continue;
                    unsigned difference = a.Level > b.Level ? a.Level - b.Level : b.Level - a.Level;
                    if (difference > maxDifference)
                        continue;
                    unsigned level = (unsigned(a.Level) + unsigned(b.Level)) / 2;
                    unsigned band = level < 30 ? 0 : level < 50 ? 1 : 2;
                    unsigned score = bands[band] * 256 + difference;
                    if (score < bestScore)
                    {
                        bestA = &a;
                        bestB = &b;
                        bestBand = band;
                        bestScore = score;
                    }
                }
            }
            if (!bestA)
                break;
            used.insert(bestA->GuidLow);
            used.insert(bestB->GuidLow);
            ++bands[bestBand];
            result.first.push_back(*bestA);
            result.second.push_back(*bestB);
        }
        return result;
    }
}
#endif
