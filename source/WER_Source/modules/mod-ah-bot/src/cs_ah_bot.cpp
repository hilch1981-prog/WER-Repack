/*
 * Copyright (C) 2008-2012 TrinityCore <http://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* ScriptData
Name: ah_bot_commandscript
%Complete: 100
Comment: All ah_bot related commands
Category: commandscripts
EndScriptData */

#include "ScriptMgr.h"
#include "Chat.h"
#include "AuctionHouseBot.h"
#include "Config.h"

#if AC_COMPILER == AC_COMPILER_GNU
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

using namespace Acore::ChatCommands;

class ah_bot_commandscript : public CommandScript
{
private:
    static ItemQualities stringToItemQualities(const char* name, int length)
    {
        // 
        // Translates a string into ItemQualities enum
        // 

        if (strncmp(name, "grey", length) == 0)
        {
            return ITEM_QUALITY_POOR;
        }

        if (strncmp(name, "white", length) == 0)
        {
            return ITEM_QUALITY_NORMAL;
        }

        if (strncmp(name, "green", length) == 0)
        {
            return ITEM_QUALITY_UNCOMMON;
        }

        if (strncmp(name, "blue", length) == 0)
        {
            return ITEM_QUALITY_RARE;
        }

        if (strncmp(name, "purple", length) == 0)
        {
            return ITEM_QUALITY_EPIC;
        }

        if (strncmp(name, "orange", length) == 0)
        {
            return ITEM_QUALITY_LEGENDARY;
        }

        if (strncmp(name, "yellow", length) == 0)
        {
            return ITEM_QUALITY_ARTIFACT;
        }

        return static_cast<ItemQualities>(-1); // Invalid
    }

public:
    ah_bot_commandscript() : CommandScript("ah_bot_commandscript")
    {

    }

    std::vector<ChatCommand> GetCommands() const override
    {
        static std::vector<ChatCommand> commandTable =
        {
            { "ahbotoptions", HandleAHBotOptionsCommand, SEC_GAMEMASTER, Console::Yes }
        };

        return commandTable;
    }

    static bool HandleAHBotOptionsCommand(ChatHandler* handler, const char*args)
    {
        uint32 ahMapID = 0;
        char*  opt     = strtok((char*)args, " ");

        if (!opt)
        {
            handler->PSendSysMessage("명령어 형식이 올바르지 않습니다.");
            handler->PSendSysMessage("ahbotoptions help로 설정 목록을 확인하세요.");

            return false;
        }

        //
        // Commands which does not requires an AH
        //

        int l = strlen(opt);

        if (strncmp(opt, "buyer", l) == 0)
        {
            char* param1 = strtok(NULL, " ");

            if (!param1)
            {
                handler->PSendSysMessage("사용법: ahbotoptions buyer $state (0 끄기, 1 켜기)");
                return false;
            }

            for (AuctionHouseBot* bot: gBots)
            {
                bot->Commands(AHBotCommand::buyer, 0, 0, param1);
            }

            return true;
        }
        else if (strncmp(opt, "seller", l) == 0)
        {
            char* param1 = strtok(NULL, " ");

            if (!param1)
            {
                handler->PSendSysMessage("사용법: ahbotoptions seller $state (0 끄기, 1 켜기)");
                return false;
            }

            for (AuctionHouseBot* bot: gBots)
            {
                bot->Commands(AHBotCommand::seller, 0, 0, param1);
            }

            return true;
        }
        else if (strncmp(opt, "usemarketprice", l) == 0)
        {
            char* param1 = strtok(NULL, " ");

            if (!param1)
            {
                handler->PSendSysMessage("사용법: ahbotoptions useMarketPrice $state (0 끄기, 1 켜기)");
                return false;
            }

            for (AuctionHouseBot* bot : gBots)
            {
                bot->Commands(AHBotCommand::useMarketPrice, 0, 0, param1);
            }

            return true;
        }

        //
        // Retrieve the auction house type
        //

        char* ahMapIdStr = strtok(NULL, " ");

        if (ahMapIdStr)
        {
            ahMapID = uint32(strtoul(ahMapIdStr, NULL, 0));

            switch (ahMapID)
            {
                case 2:
                case 6:
                case 7:
                    break;
                default:
                    opt = NULL;
                    break;
            }
        }

        //
        // Syntax check
        //

        if (!opt)
        {
            handler->PSendSysMessage("명령어 오류: 경매장 ID는 2, 6, 7 중 하나여야 합니다.");
            return false;
        }

        //
        // Commands that do requires an AH id to be performed
        //

        if (strncmp(opt, "help", l) == 0)
        {
            handler->PSendSysMessage("경매장 봇 명령어:");
            handler->PSendSysMessage("buyer - 구매 기능 켜기/끄기");
            handler->PSendSysMessage("seller - 판매 기능 켜기/끄기");
            handler->PSendSysMessage("usemarketprice - 시세 기준 판매 켜기/끄기");
            handler->PSendSysMessage("ahexpire - 봇이 등록한 모든 경매 만료 처리");
            handler->PSendSysMessage("minitems - 최소 경매 수 설정");
            handler->PSendSysMessage("maxitems - 최대 경매 수 설정");
            handler->PSendSysMessage("percentages - 품질별 판매 비율 설정");
            handler->PSendSysMessage("minprice - 최소 판매 가격 설정");
            handler->PSendSysMessage("maxprice - 최대 판매 가격 설정");
            handler->PSendSysMessage("minbidprice - 구매 봇 최소 입찰가 설정");
            handler->PSendSysMessage("maxbidprice - 구매 봇 최대 입찰가 설정");
            handler->PSendSysMessage("maxstack - 최대 묶음 수량 설정");
            handler->PSendSysMessage("buyerprice - 구매 가격 정책 설정");
            handler->PSendSysMessage("bidinterval - 입찰 주기 설정");
            handler->PSendSysMessage("bidsperinterval - 주기당 입찰 수 설정");

            return true;
        }
        else if (strncmp(opt, "ahexpire", l) == 0)
        {
            if (!ahMapIdStr)
            {
                handler->PSendSysMessage("사용법: ahbotoptions ahexpire $ahMapID (2, 6 또는 7)");
                return false;
            }

            for (AuctionHouseBot* bot: gBots)
            {
                bot->Commands(AHBotCommand::ahexpire, ahMapID, 0, NULL);
            }
        }
        else if (strncmp(opt, "minitems", l) == 0)
        {
            char* param1 = strtok(NULL, " ");

            if (!ahMapIdStr || !param1)
            {
                handler->PSendSysMessage("사용법: ahbotoptions minitems $ahMapID (2, 6 또는 7) $minItems");
                return false;
            }

            for (AuctionHouseBot* bot : gBots)
            {
                bot->Commands(AHBotCommand::minitems, ahMapID, 0, param1);
            }
        }
        else if (strncmp(opt, "maxitems", l) == 0)
        {
            char* param1 = strtok(NULL, " ");

            if (!ahMapIdStr || !param1)
            {
                handler->PSendSysMessage("사용법: ahbotoptions maxitems $ahMapID (2, 6 또는 7) $maxItems");
                return false;
            }

            for (AuctionHouseBot* bot: gBots)
            {
                bot->Commands(AHBotCommand::maxitems, ahMapID, 0, param1);
            }
        }
        else if (strncmp(opt, "percentages", l) == 0)
        {
            char* param1  = strtok(NULL, " ");
            char* param2  = strtok(NULL, " ");
            char* param3  = strtok(NULL, " ");
            char* param4  = strtok(NULL, " ");
            char* param5  = strtok(NULL, " ");
            char* param6  = strtok(NULL, " ");
            char* param7  = strtok(NULL, " ");
            char* param8  = strtok(NULL, " ");
            char* param9  = strtok(NULL, " ");
            char* param10 = strtok(NULL, " ");
            char* param11 = strtok(NULL, " ");
            char* param12 = strtok(NULL, " ");
            char* param13 = strtok(NULL, " ");
            char* param14 = strtok(NULL, " ");

            if (!ahMapIdStr || !param14)
            {
                handler->PSendSysMessage("사용법: ahbotoptions percentages $ahMapID (2, 6 또는 7) $1 $2 $3 $4 $5 $6 $7 $8 $9 $10 $11 $12 $13 $14");
                handler->PSendSysMessage("거래 물품: 1 하급, 2 일반, 3 고급, 4 희귀, 5 영웅");
                handler->PSendSysMessage("거래 물품: 6 전설, 7 유물 / 일반 물품: 8 하급, 9 일반, 10 고급, 11 희귀");
                handler->PSendSysMessage("일반 물품: 12 영웅, 13 전설, 14 유물");

                return false;
            }

            uint32 greytg       = uint32(strtoul(param1 , NULL, 0));
            uint32 whitetg      = uint32(strtoul(param2 , NULL, 0));
            uint32 greentg      = uint32(strtoul(param3 , NULL, 0));
            uint32 bluetg       = uint32(strtoul(param4 , NULL, 0));
            uint32 purpletg     = uint32(strtoul(param5 , NULL, 0));
            uint32 orangetg     = uint32(strtoul(param6 , NULL, 0));
            uint32 yellowtg     = uint32(strtoul(param7 , NULL, 0));
            uint32 greyi        = uint32(strtoul(param8 , NULL, 0));
            uint32 whitei       = uint32(strtoul(param9 , NULL, 0));
            uint32 greeni       = uint32(strtoul(param10, NULL, 0));
            uint32 bluei        = uint32(strtoul(param11, NULL, 0));
            uint32 purplei      = uint32(strtoul(param12, NULL, 0));
            uint32 orangei      = uint32(strtoul(param13, NULL, 0));
            uint32 yellowi      = uint32(strtoul(param14, NULL, 0));

            uint32 totalPercent = greytg + whitetg + greentg + bluetg + purpletg + orangetg + yellowtg + greyi + whitei + greeni + bluei + purplei + orangei + yellowi;

            if (totalPercent == 0 || totalPercent != 100)
            {
                handler->PSendSysMessage("비율의 합계는 100%%여야 합니다.");

                return false;
            }

            char param[100] = { 0 };

            strcat(param, param1);
            strcat(param, " ");
            strcat(param, param2);
            strcat(param, " ");
            strcat(param, param3);
            strcat(param, " ");
            strcat(param, param4);
            strcat(param, " ");
            strcat(param, param5);
            strcat(param, " ");
            strcat(param, param6);
            strcat(param, " ");
            strcat(param, param7);
            strcat(param, " ");
            strcat(param, param8);
            strcat(param, " ");
            strcat(param, param9);
            strcat(param, " ");
            strcat(param, param10);
            strcat(param, " ");
            strcat(param, param11);
            strcat(param, " ");
            strcat(param, param12);
            strcat(param, " ");
            strcat(param, param13);
            strcat(param, " ");
            strcat(param, param14);

            for (AuctionHouseBot* bot: gBots)
            {
                bot->Commands(AHBotCommand::percentages, ahMapID, 0, param);
            }
        }
        else if (strncmp(opt, "minprice", l) == 0)
        {
            char* param1 = strtok(NULL, " ");
            char* param2 = strtok(NULL, " ");

            if (!ahMapIdStr || !param1 || !param2)
            {
                handler->PSendSysMessage("사용법: ahbotoptions minprice $ahMapID (2, 6 또는 7) $color (grey, white, green, blue, purple, orange 또는 yellow) $price");
                return false;
            }

            auto quality = stringToItemQualities(param1, l);

            if (quality != static_cast<ItemQualities>(-1))
            {
                for (AuctionHouseBot* bot: gBots)
                {
                    bot->Commands(AHBotCommand::minprice, ahMapID, quality, param2);
                }
            }
            else
            {
                handler->PSendSysMessage("사용법: ahbotoptions minprice $ahMapID (2, 6 또는 7) $color (grey, white, green, blue, purple, orange 또는 yellow) $price");
                return false;
            }
        }
        else if (strncmp(opt, "maxprice", l) == 0)
        {
            char* param1 = strtok(NULL, " ");
            char* param2 = strtok(NULL, " ");

            if (!ahMapIdStr || !param1 || !param2)
            {
                handler->PSendSysMessage("사용법: ahbotoptions maxprice $ahMapID (2, 6 또는 7) $color (grey, white, green, blue, purple, orange 또는 yellow) $price");
                return false;
            }

            auto quality = stringToItemQualities(param1, l);

            if (quality != static_cast<ItemQualities>(-1))
            {
                for (AuctionHouseBot* bot: gBots)
                {
                    bot->Commands(AHBotCommand::maxprice, ahMapID, quality, param2);
                }
            }
            else
            {
                handler->PSendSysMessage("사용법: ahbotoptions maxprice $ahMapID (2, 6 또는 7) $color (grey, white, green, blue, purple, orange 또는 yellow) $price");
                return false;
            }
        }
        else if (strncmp(opt, "minbidprice", l) == 0)
        {
            char* param1 = strtok(NULL, " ");
            char* param2 = strtok(NULL, " ");

            if (!ahMapIdStr || !param2 || !param2)
            {
                handler->PSendSysMessage("사용법: ahbotoptions minbidprice $ahMapID (2, 6 또는 7) $color (grey, white, green, blue, purple, orange 또는 yellow) $price");
                return false;
            }

            uint32 minBidPrice = uint32(strtoul(param2, NULL, 0));

            if (minBidPrice < 1 || minBidPrice > 100)
            {
                handler->PSendSysMessage("최소 입찰가 배율은 1~100이어야 합니다.");
                return false;
            }

            auto quality = stringToItemQualities(param1, l);

            if (quality != static_cast<ItemQualities>(-1))
            {
                for (AuctionHouseBot* bot: gBots)
                {
                    bot->Commands(AHBotCommand::minbidprice, ahMapID, quality, param2);
                }
            }
            else
            {
                handler->PSendSysMessage("사용법: ahbotoptions minbidprice $ahMapID (2, 6 또는 7) $color (grey, white, green, blue, purple, orange 또는 yellow) $price");
                return false;
            }
        }
        else if (strncmp(opt, "maxbidprice", l) == 0)
        {
            char* param1 = strtok(NULL, " ");
            char* param2 = strtok(NULL, " ");

            if (!ahMapIdStr || !param1 || !param2)
            {
                handler->PSendSysMessage("사용법: ahbotoptions maxbidprice $ahMapID (2, 6 또는 7) $color (grey, white, green, blue, purple, orange 또는 yellow) $price");
                return false;
            }

            uint32 maxBidPrice = uint32(strtoul(param2, NULL, 0));

            if (maxBidPrice < 1 || maxBidPrice > 100)
            {
                handler->PSendSysMessage("최대 입찰가 배율은 1~100이어야 합니다.");
                return false;
            }

            auto quality = stringToItemQualities(param1, l);

            if (quality != static_cast<ItemQualities>(-1))
            {
                for (AuctionHouseBot* bot: gBots)
                {
                    bot->Commands(AHBotCommand::maxbidprice, ahMapID, quality, param2);
                }
            }
            else
            {
                handler->PSendSysMessage("사용법: ahbotoptions max bidprice $ahMapID (2, 6 또는 7) $color (grey, white, green, blue, purple, orange 또는 yellow) $price");
                return false;
            }
        }
        else if (strncmp(opt, "maxstack",l) == 0)
        {
            char* param1 = strtok(NULL, " ");
            char* param2 = strtok(NULL, " ");

            if (!ahMapIdStr || !param1 || !param2)
            {
                handler->PSendSysMessage("사용법: ahbotoptions maxstack $ahMapID (2, 6 또는 7) $color (grey, white, green, blue, purple, orange 또는 yellow) $value");
                return false;
            }

            // uint32 maxStack = uint32(strtoul(param2, NULL, 0));
            // if (maxStack < 0)
            // {
            //     handler->PSendSysMessage("maxstack can't be a negative number.");
            //    return false;
            // }

            auto quality = stringToItemQualities(param1, l);

            if (quality != static_cast<ItemQualities>(-1))
            {
                for (AuctionHouseBot* bot: gBots)
                {
                    bot->Commands(AHBotCommand::maxstack, ahMapID, quality, param2);
                }
            }
            else
            {
                handler->PSendSysMessage("사용법: ahbotoptions maxstack $ahMapID (2, 6 또는 7) $color (grey, white, green, blue, purple, orange 또는 yellow) $value");
                return false;
            }
        }
        else if (strncmp(opt, "buyerprice", l) == 0)
        {
            char* param1 = strtok(NULL, " ");
            char* param2 = strtok(NULL, " ");

            if (!ahMapIdStr || !param1 || !param2)
            {
                handler->PSendSysMessage("사용법: ahbotoptions buyerprice $ahMapID (2, 6 또는 7) $color (grey, white, green, blue 또는 purple) $price");
                return false;
            }

            auto quality = stringToItemQualities(param1, l);

            if (quality != static_cast<ItemQualities>(-1))
            {
                for (AuctionHouseBot* bot: gBots)
                {
                    bot->Commands(AHBotCommand::buyerprice, ahMapID, quality, param2);
                }
            }
            else
            {
                handler->PSendSysMessage("사용법: ahbotoptions buyerprice $ahMapID (2, 6 또는 7) $color (grey, white, green, blue 또는 purple) $price");
                return false;
            }
        }
        else if (strncmp(opt, "bidinterval", l) == 0)
        {
            char* param1 = strtok(NULL, " ");

            if (!ahMapIdStr || !param1)
            {
                handler->PSendSysMessage("사용법: ahbotoptions bidinterval $ahMapID (2, 6 또는 7) $interval(분)");
                return false;
            }

            for (AuctionHouseBot* bot: gBots)
            {
                bot->Commands(AHBotCommand::bidinterval, ahMapID, 0, param1);
            }
        }
        else if (strncmp(opt, "bidsperinterval", l) == 0)
        {
            char* param1 = strtok(NULL, " ");

            if (!ahMapIdStr || !param1)
            {
                handler->PSendSysMessage("사용법: ahbotoptions bidsperinterval $ahMapID (2, 6 또는 7) $bids");
                return false;
            }

            for (AuctionHouseBot* bot: gBots)
            {
                bot->Commands(AHBotCommand::bidsperinterval, ahMapID, 0, param1);
            }
        }
        else
        {
            handler->PSendSysMessage("명령어 형식이 올바르지 않습니다.");
            handler->PSendSysMessage("ahbotoptions help로 설정 목록을 확인하세요.");
            return false;
        }

        handler->PSendSysMessage("완료했습니다.");
        return true;
    }
};

void AddAHBotCommandScripts()
{
    new ah_bot_commandscript();
}
