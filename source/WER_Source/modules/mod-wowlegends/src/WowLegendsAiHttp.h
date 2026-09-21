// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <string_view>

namespace WowLegendsAiHttp
{
// Keep provider errors out of success parsers without exposing response bodies.
inline bool IsSuccess(std::string_view response)
{
    auto end = response.find("\r\n");
    if (end == std::string_view::npos || end < 13)
        return false;
    if (response.substr(0, 9) != "HTTP/1.1 " && response.substr(0, 9) != "HTTP/1.0 ")
        return false;
    if (response[12] != ' ')
        return false;
    for (std::size_t i = 9; i < 12; ++i)
        if (response[i] < '0' || response[i] > '9')
            return false;
    unsigned status = (response[9] - '0') * 100 + (response[10] - '0') * 10 + response[11] - '0';
    return status >= 200 && status < 300;
}

constexpr std::size_t MaxResponseBytes = 8 * 1024 * 1024;
inline bool CanAppend(std::size_t current, std::size_t added)
{
    return current <= MaxResponseBytes && added <= MaxResponseBytes - current;
}
}
