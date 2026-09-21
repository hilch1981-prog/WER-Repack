#ifndef AZEROTHCORE_MODULES_WORLDCHAT_H
#define AZEROTHCORE_MODULES_WORLDCHAT_H

/*
 * Enhanced World Chat / World Chat 2.x
 *
 * A modern global chat module for AzerothCore.
 *
 * Features:
 * - Cross-faction world chat
 * - Persistent player World Chat mode
 * - Saved player preferences
 * - Flood protection
 * - Duplicate message detection
 * - URL / advertising filter
 * - Trade and group message auto-tagging
 * - WoW hyperlink preservation
 * - Ignore-list support
 * - Public API helpers for other modules
 */

#include <array>
#include <ctime>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "Channel.h"
#include "Chat.h"
#include "CommandScript.h"
#include "PlayerScript.h"
#include "SharedDefines.h"
#include "WorldScript.h"

namespace WC
{
    namespace ChatColor
    {
        constexpr std::string_view WORLD    = "|cff55ccff";
        constexpr std::string_view ANNOUNCE = "|cffffcc00";
        constexpr std::string_view TRADE    = "|cffffb347";
        constexpr std::string_view GROUP    = "|cffaad372";
        constexpr std::string_view TEXT     = "|cffcccccc";
        constexpr std::string_view GREEN    = "|cff00cc00";
        constexpr std::string_view RED      = "|cffff0000";
        constexpr std::string_view YELLOW   = "|cffffff00";
        constexpr std::string_view GREY     = "|cff999999";
    }

    constexpr std::array<std::string_view, MAX_CLASSES> ClassColor =
    {
        "",
        "|cffC79C6E", // Warrior
        "|cffF58CBA", // Paladin
        "|cffABD473", // Hunter
        "|cffFFF569", // Rogue
        "|cffFFFFFF", // Priest
        "|cffC41F3B", // Death Knight
        "|cff0070DE", // Shaman
        "|cff40C7EB", // Mage
        "|cff8787ED", // Warlock
        "",           // Monk unused in 3.3.5a
        "|cffFF7D0A", // Druid
    };

    constexpr std::string_view GMIcon = "|TINTERFACE/CHATFRAME/UI-CHATICON-BLIZZ:13:26:0:-2|t";

    struct PlayerState
    {
        bool receiveEnabled = true;
        bool worldMode = false;

        time_t muteUntil = 0;
        std::deque<time_t> recentMessages;

        std::string lastNormalizedMessage;
        time_t lastDuplicateTime = 0;
        uint32 duplicateCount = 0;
    };

    struct Config
    {
        bool enabled = true;
        bool crossFaction = true;
        bool announce = false;
        bool loginState = true;
        bool forceOnLogin = true;
        bool autoJoinChannel = true;
        bool ignoreSupport = true;

        uint8 minimumLevel = 10;

        uint32 floodWindowSeconds = 10;
        uint32 floodWarnCount = 3;
        uint32 floodMuteCount = 5;
        uint32 floodMuteSeconds = 60;

        bool duplicateDetection = true;
        uint32 duplicateWindowSeconds = 30;
        uint32 duplicateMaxRepeats = 2;

        bool urlFilter = true;
        std::vector<std::string> blockedUrlTerms;

        bool tradeDetection = true;
        bool groupDetection = true;

        std::string channelName = "world";
        std::string tag = "[World]";
        std::string tradeTag = "[Trade]";
        std::string groupTag = "[Group]";

        std::unordered_map<uint32, PlayerState> playerStates;
    };

    enum class MessageChannel
    {
        World,
        Announcement,
        Trade,
        Group,
        Horde,
        Alliance,
        GM
    };

    class WorldChat_Config : public WorldScript
    {
    public:
        WorldChat_Config() : WorldScript("WorldChat_Config", { WORLDHOOK_ON_BEFORE_CONFIG_LOAD }) {}
        void OnBeforeConfigLoad(bool reload) override;
    };

    class WorldChat_Announce : public PlayerScript
    {
    public:
        WorldChat_Announce()
            : PlayerScript("WorldChat_Announce",
            {
                PLAYERHOOK_ON_LOGIN,
                PLAYERHOOK_ON_MAP_CHANGED,
                PLAYERHOOK_CAN_PLAYER_USE_CHAT,
                PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT
            })
        {
        }

        void OnPlayerLogin(Player* player) override;
        void OnPlayerMapChanged(Player* player) override;
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg) override;
        bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg, Channel* channel) override;
    };

    class WorldChat : public CommandScript
    {
    public:
        WorldChat() : CommandScript("WorldChat") {}

        static bool HandleWorldChatCommand(ChatHandler* handler, const char* msg);
        static bool HandleWorldChatOnCommand(ChatHandler* handler);
        static bool HandleWorldChatOffCommand(ChatHandler* handler);
        static bool HandleWorldChatStatusCommand(ChatHandler* handler);
        static bool HandleWorldChatAnnounceCommand(ChatHandler* handler, const char* msg);
        static bool HandleWorldChatHordeCommand(ChatHandler* handler, const char* msg);
        static bool HandleWorldChatAllianceCommand(ChatHandler* handler, const char* msg);

        Acore::ChatCommands::ChatCommandTable GetCommands() const override;
    };

    // Player-originated World Chat messages.
    void SendWorldMessage(Player const& sender, std::string const& msg, int team);
    void SendWorldAnnouncement(std::string const& msg);

    // Backwards-friendly API helpers for other modules.
    void Broadcast(std::string const& msg);
    void BroadcastFaction(std::string const& msg, int team);
    void BroadcastTrade(std::string const& msg);
    void BroadcastGroup(std::string const& msg);

    // Extended API helpers for other modules.
    void Broadcast(MessageChannel channel, std::string const& msg);
    void SendToPlayer(Player* player, MessageChannel channel, std::string const& msg);
    void SendToFaction(uint32 team, MessageChannel channel, std::string const& msg);
    void SendToGMs(MessageChannel channel, std::string const& msg);
}

#endif // AZEROTHCORE_MODULES_WORLDCHAT_H
