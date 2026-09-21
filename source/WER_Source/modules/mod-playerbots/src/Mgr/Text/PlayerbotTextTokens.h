#ifndef PLAYERBOT_TEXT_TOKENS_H
#define PLAYERBOT_TEXT_TOKENS_H

#include <map>
#include <string>

namespace PlayerbotTextTokens
{
inline bool IsTokenStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

inline bool IsTokenChar(char c)
{
    return IsTokenStart(c) || (c >= '0' && c <= '9');
}

// Substitute complete template tokens once. Never interpret tokens in player names/link values.
inline bool Render(std::string const& source, std::map<std::string, std::string> const& values,
                   std::string& output)
{
    output.clear();
    for (std::size_t i = 0; i < source.size();)
    {
        if (source[i] != '%' || i + 1 == source.size() || !IsTokenStart(source[i + 1]))
        {
            output += source[i++];
            continue;
        }

        std::size_t end = i + 2;
        while (end < source.size() && IsTokenChar(source[end]))
            ++end;
        auto const value = values.find(source.substr(i, end - i));
        if (value == values.end())
        {
            output.clear();
            return false;
        }

        output += value->second;
        i = end;
    }
    return true;
}
}

#endif
