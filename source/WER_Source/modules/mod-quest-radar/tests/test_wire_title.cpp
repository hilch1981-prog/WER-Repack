#include "../src/QuestRadarText.h"
#include "../src/QuestRadarRequestGate.h"
#include <cassert>
#include <iostream>
#include <vector>

int main()
{
    assert(QuestRadar_WireTitle("").empty());
    assert(QuestRadar_WireTitle("short") == "short");
    assert(QuestRadar_WireTitle(std::string(61, 'a')) == std::string(60, 'a'));
    assert(QuestRadar_WireTitle(std::string("a\tb\rc\nd\0e", 9)) == "a b c d e");
    // Cover each byte boundary in ASCII, Korean (3), accented (2), and emoji (4).
    std::vector<std::string> units = {"a", u8"한", u8"글", u8"é", u8"🙂", "Z"};
    std::string source;
    std::vector<std::size_t> boundaries = {0};
    for (int repeat = 0; repeat < 10; ++repeat)
        for (auto const& unit : units)
        {
            source += unit;
            boundaries.push_back(source.size());
        }
    for (std::size_t limit = 0; limit <= source.size() + 1; ++limit)
    {
        std::size_t expected = 0;
        for (std::size_t boundary : boundaries)
            if (boundary <= limit)
                expected = boundary;
        assert(QuestRadar_WireTitle(source, limit) == source.substr(0, expected));
    }
    QuestRadarRequestGate gate;
    auto now = QuestRadarRequestGate::Clock::time_point{};
    assert(gate.Accept(1, now));
    assert(!gate.Accept(1, now));
    assert(gate.Accept(2, now));
    assert(!gate.Accept(1, now + std::chrono::milliseconds(2999)));
    assert(gate.Accept(1, now + std::chrono::seconds(3)));
    gate.Remove(1);
    assert(gate.Accept(1, now + std::chrono::seconds(3)));
    gate.Remove(999);
    std::cout << "PASS: UTF-8 byte limits 0.." << source.size() + 1
              << ", control characters, empty/ASCII, per-player request throttle and logout reset\n";
}
