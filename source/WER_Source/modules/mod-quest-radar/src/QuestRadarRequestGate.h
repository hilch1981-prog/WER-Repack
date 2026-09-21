#ifndef WOW_LEGENDS_QUEST_RADAR_REQUEST_GATE_H
#define WOW_LEGENDS_QUEST_RADAR_REQUEST_GATE_H
#include <chrono>
#include <cstdint>
#include <unordered_map>

// Used on the core's serialized player chat/logout hooks, not a worker thread.
class QuestRadarRequestGate
{
public:
    using Clock = std::chrono::steady_clock;
    bool Accept(std::uint64_t player, Clock::time_point now = Clock::now())
    {
        auto found = last_.find(player);
        if (found != last_.end() && now - found->second < std::chrono::seconds(3))
            return false;
        last_[player] = now;
        return true;
    }
    void Remove(std::uint64_t player) { last_.erase(player); }
private:
    std::unordered_map<std::uint64_t, Clock::time_point> last_;
};
#endif
