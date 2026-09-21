#ifndef WOW_LEGENDS_QUEST_RADAR_TEXT_H
#define WOW_LEGENDS_QUEST_RADAR_TEXT_H

#include <cstddef>
#include <string>

// Local port: maintain the upstream 60-byte title budget without splitting
// a valid UTF-8 code point. DB locale strings are expected to be valid UTF-8.
inline std::string QuestRadar_WireTitle(std::string text, std::size_t maxBytes = 60)
{
    for (char& value : text)
        if (value == '\t' || value == '\r' || value == '\n' || value == '\0')
            value = ' ';
    if (text.size() > maxBytes)
    {
        std::size_t end = maxBytes;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80)
            --end;
        text.resize(end);
    }
    return text;
}

#endif
