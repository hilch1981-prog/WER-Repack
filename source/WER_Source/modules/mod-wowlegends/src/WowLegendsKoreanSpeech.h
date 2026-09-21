#ifndef WOW_LEGENDS_KOREAN_SPEECH_H
#define WOW_LEGENDS_KOREAN_SPEECH_H

#include <string_view>

namespace WowLegends
{
// Check visible text, not hyperlink identifiers or color escape payloads.
inline bool HasLatinSpeech(std::string_view text, bool scene = false)
{
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text.substr(i, 2) == "|H")
        {
            auto end = text.find("|h", i + 2);
            if (end == std::string_view::npos)
                return true;
            i = end + 1;
            continue;
        }
        if (text.substr(i, 2) == "|c" && i + 10 <= text.size())
        {
            i += 9;
            continue;
        }
        if (text.substr(i, 2) == "|h" || text.substr(i, 2) == "|r")
        {
            ++i;
            continue;
        }
        // Living Chatter requires A:/B: speaker labels; these are not displayed.
        if (scene && (i == 0 || text[i - 1] == '\n') &&
            (text[i] == 'A' || text[i] == 'B') && i + 1 < text.size() && text[i + 1] == ':')
            continue;
        if ((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z'))
            return true;
    }
    return false;
}
}

#endif
