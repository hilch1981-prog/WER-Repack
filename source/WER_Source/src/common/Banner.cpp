/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "Banner.h"
#include "GitRevision.h"
#include "StringFormat.h"

void Acore::Banner::Show(std::string_view applicationName, void(*log)(std::string_view text), void(*logExtraInfo)())
{
    log(Acore::StringFormat("WER REPACK VER1.1.0 ({}) - AzerothCore 기반", applicationName));
    log("============================================================");
    log("  와우 리치왕 버전 3.3.5a (빌드 12340)");
    log("  한국 에뮬레이터 연구소 제작 - WER REPACK VER1.1.0");
    log("  제작일: 2026년 9월 21일 (2026-09-21)");
    log("============================================================");
    log("  종료: Ctrl+C (월드 서버 -> 로그인 서버 -> MySQL 순서)\n");

    if (logExtraInfo)
    {
        logExtraInfo();
    }

    log(" ");
}
