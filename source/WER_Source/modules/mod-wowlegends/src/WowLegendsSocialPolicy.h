#ifndef WOW_LEGENDS_SOCIAL_POLICY_H
#define WOW_LEGENDS_SOCIAL_POLICY_H

#include <array>
#include <sstream>
#include <string>

namespace WowLegends
{
    // Exactly one A/B exchange, never recursively continued by chat hooks.
    inline std::array<std::string, 2> ParseSocialPair(std::string const& text)
    {
        std::array<std::string, 2> result;
        std::istringstream input(text);
        std::string line;
        unsigned count = 0;
        while (std::getline(input, line))
        {
            auto first = line.find_first_not_of(" \t\r");
            if (first == std::string::npos)
                continue;
            line = line.substr(first, line.find_last_not_of(" \t\r") - first + 1);
            if (count >= 2 || line.size() < 3 || line[0] != "AB"[count] || line[1] != ':')
                return {};
            auto start = line.find_first_not_of(" \t", 2);
            if (start == std::string::npos || line.size() - start > 230)
                return {};
            result[count++] = line.substr(start);
        }
        if (count != 2 || result[0] == result[1])
            return {};
        return result;
    }
}
#endif
