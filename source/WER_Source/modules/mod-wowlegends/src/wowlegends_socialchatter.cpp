/*
 * WOW Legends local extension: bounded Korean player-style guild/group chatter.
 * Released under GNU AGPL v3 or later, matching mod-wowlegends.
 */
#include "ScriptMgr.h"
#include "Chat.h"
#include "Configuration/Config.h"
#include "DBCStores.h"
#include "Group.h"
#include "Guild.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Random.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "WowLegendsKoreanSpeech.h"
#include "WowLegendsSocialPolicy.h"
#include <algorithm>
#include <map>
#include <string>
#include <vector>

bool WlAiSocialGenerate(uint32 requestId, ObjectGuid botA, ObjectGuid botB,
    std::string const& botName, std::string const& prompt);
bool WlSpeechAllow(Player* who, int priority, uint64 beat);

namespace
{
    bool enabled = false;
    bool guildEnabled = false;
    bool groupEnabled = false;
    uint32 guildInterval = 180;
    uint32 groupInterval = 90;
    uint64 nowMs = 0;
    uint32 tickMs = 0;
    uint32 sequence = 0;
    std::map<std::string, uint64> nextAllowed;
    std::map<ObjectGuid, uint64> botNext;
    std::map<std::string, std::string> lastExchange;

    struct Exchange
    {
        uint32 id;
        ObjectGuid a, b, witness, group;
        uint32 guildId = 0;
        uint32 chatType = 0;
        std::string key;
        uint64 expires;
        uint64 nextLine = 0;
        bool combatA, combatB;
        std::array<std::string, 2> lines;
        unsigned line = 0;
    };
    std::map<uint32, Exchange> pending;

    bool Real(Player* player)
    {
        return player && player->IsInWorld() && player->GetSession() &&
            !player->GetSession()->IsBot() && !player->isAFK();
    }

    bool Bot(Player* player)
    {
        return player && player->IsInWorld() && player->GetSession() &&
            player->GetSession()->IsBot() && player->IsAlive() && !player->IsBeingTeleported();
    }

    std::string Key(Player* witness, bool guild)
    {
        if (guild)
            return witness->GetGuildId() ? "guild:" + std::to_string(witness->GetGuildId()) : "";
        Group* group = witness->GetGroup();
        return group ? "group:" + group->GetGUID().ToString() : "";
    }

    bool StillValid(Exchange const& exchange)
    {
        if (!enabled || nowMs >= exchange.expires)
            return false;
        Player* witness = ObjectAccessor::FindPlayer(exchange.witness);
        Player* a = ObjectAccessor::FindPlayer(exchange.a);
        Player* b = ObjectAccessor::FindPlayer(exchange.b);
        if (!Real(witness) || !Bot(a) || !Bot(b))
            return false;
        if (exchange.guildId)
            return guildEnabled && witness->GetGuildId() == exchange.guildId &&
                a->GetGuildId() == exchange.guildId && b->GetGuildId() == exchange.guildId &&
                !a->IsInCombat() && !b->IsInCombat();
        Group* group = witness->GetGroup();
        return groupEnabled && group && group->GetGUID() == exchange.group &&
            a->GetGroup() == group && b->GetGroup() == group &&
            a->GetMap() == witness->GetMap() && b->GetMap() == witness->GetMap() &&
            a->IsInCombat() == exchange.combatA && b->IsInCombat() == exchange.combatB;
    }

    std::string Facts(Player* bot)
    {
        std::string result = bot->GetName() + ", " + std::to_string(bot->GetLevel()) + "레벨, ";
        result += PlayerbotAI::IsTank(bot) ? "탱커" : PlayerbotAI::IsHeal(bot) ? "힐러" : "공격 담당";
        if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(bot->GetZoneId()))
            if (zone->area_name[LOCALE_koKR] && *zone->area_name[LOCALE_koKR])
                result += std::string(", 지역 ") + zone->area_name[LOCALE_koKR];
        result += bot->IsInCombat() ? ", 현재 전투 중" : ", 현재 비전투";
        if (Group* group = bot->GetGroup())
            result += group->GetLeaderGUID() == bot->GetGUID() ? ", 실제 그룹장" : ", 그룹원";
        return result;
    }

    bool Begin(Player* witness, bool guild)
    {
        std::string key = Key(witness, guild);
        if (key.empty() || pending.size() >= 32)
            return false;
        auto due = nextAllowed.find(key);
        if (due == nextAllowed.end())
        {
            nextAllowed[key] = nowMs + 30000;
            return false;
        }
        if (nowMs < due->second)
            return false;
        for (auto const& entry : pending)
            if (entry.second.key == key)
                return false;
        due->second = nowMs + 15000; // failed selection/request backoff
        std::vector<Player*> bots;
        for (auto const& entry : ObjectAccessor::GetPlayers())
        {
            Player* bot = entry.second;
            if (!Bot(bot))
                continue;
            auto botDue = botNext.find(bot->GetGUID());
            if (botDue != botNext.end() && nowMs < botDue->second)
                continue;
            if (guild)
            {
                if (bot->GetGuildId() != witness->GetGuildId() || bot->IsInCombat())
                    continue;
            }
            else if (bot->GetGroup() != witness->GetGroup() || bot->GetMap() != witness->GetMap())
                continue;
            bots.push_back(bot);
        }
        if (bots.size() < 2)
            return false;
        for (size_t i = bots.size(); i > 1; --i)
            std::swap(bots[i - 1], bots[urand(0, uint32(i - 1))]);
        Player* a = bots[0];
        Player* b = bots[1];
        if (!WlSpeechAllow(a, 0, 0))
            return false;
        Group* group = witness->GetGroup();
        bool combat = !guild && (a->IsInCombat() || b->IsInCombat());
        Exchange exchange{};
        if (++sequence == 0)
            ++sequence;
        exchange.id = sequence;
        exchange.a = a->GetGUID();
        exchange.b = b->GetGUID();
        exchange.witness = witness->GetGUID();
        exchange.group = !guild && group ? group->GetGUID() : ObjectGuid::Empty;
        exchange.guildId = guild ? witness->GetGuildId() : 0;
        exchange.chatType = guild ? CHAT_MSG_GUILD : group->isRaidGroup() ? CHAT_MSG_RAID : CHAT_MSG_PARTY;
        exchange.key = key;
        exchange.expires = nowMs + (combat ? 20000 : 60000);
        exchange.combatA = a->IsInCombat();
        exchange.combatB = b->IsInCombat();
        std::string prompt = "실제 와우 유저 두 명의 짧은 한국어 대화. 세계관 인물처럼 연기하지 마세요. ";
        prompt += guild ? "길드 채팅입니다. " : group->isRaidGroup() ? "공격대 채팅입니다. " : "파티 채팅입니다. ";
        prompt += "A: " + Facts(a) + "\nB: " + Facts(b);
        auto previous = lastExchange.find(key);
        if (previous != lastExchange.end())
            prompt += "\n이 채널의 이전 봇 대화(내용을 반복하지 마세요): " + previous->second;
        prompt += "\nA가 상황에 맞는 말을 먼저 하고 B가 그 말에 짧게 답하세요. 각각 한 문장만. "
            "확인되지 않은 보스 전술, 디버프, 시약 수량, 재사용 대기시간, 행동 성공을 지어내지 마세요. "
            "그룹원이 공대장인 척하지 마세요. 접속 인사나 농담, 운영자 호출은 하지 마세요. "
            "전투 중에는 현재 역할에 관련된 짧은 소통, 비전투에는 다음 진행을 함께 확인하는 말로 하세요. "
            "다음 두 줄 형식만 출력: A: 한국어 문장\nB: 한국어 문장";
        if (!WlAiSocialGenerate(exchange.id, exchange.a, exchange.b, a->GetName(), prompt))
            return false;
        pending.emplace(exchange.id, exchange);
        botNext[exchange.a] = nowMs + 60000;
        botNext[exchange.b] = nowMs + 60000;
        uint32 interval = guild ? guildInterval : groupInterval;
        due->second = nowMs + uint64(urand(interval * 3 / 4, interval * 5 / 4)) * 1000;
        return true;
    }
}

void WlSocialPlayerSpoke(Player* player, uint32 chatType)
{
    if (!enabled || !Real(player))
        return;
    std::string key = Key(player, chatType == CHAT_MSG_GUILD);
    if (key.empty())
        return;
    nextAllowed[key] = std::max(nextAllowed[key], nowMs + 30000);
    for (auto it = pending.begin(); it != pending.end();)
        if (it->second.key == key)
            it = pending.erase(it);
        else
            ++it;
}

void WlSocialDeliver(uint32 requestId, ObjectGuid botA, ObjectGuid botB, std::string const& text)
{
    auto it = pending.find(requestId);
    if (it == pending.end() || it->second.a != botA || it->second.b != botB)
        return;
    if (!StillValid(it->second) || WowLegends::HasLatinSpeech(text, true))
    {
        pending.erase(it);
        return;
    }
    auto lines = WowLegends::ParseSocialPair(text);
    std::string const combined = lines[0] + "\n" + lines[1];
    auto previous = lastExchange.find(it->second.key);
    if (lines[0].empty() || (previous != lastExchange.end() && previous->second == combined))
    {
        pending.erase(it);
        return;
    }
    it->second.lines = std::move(lines);
    it->second.nextLine = nowMs + 1000;
}

class WowLegendsSocialChatterWorld : public WorldScript
{
public:
    WowLegendsSocialChatterWorld() : WorldScript("WowLegendsSocialChatterWorld",
        {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE}) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        enabled = sConfigMgr->GetOption<bool>("WowLegends.SocialChatter.Enabled", false) &&
            sConfigMgr->GetOption<bool>("WowLegends.AiChat.Enabled", false);
        guildEnabled = sConfigMgr->GetOption<bool>("WowLegends.AiChat.GuildChat", false);
        groupEnabled = sConfigMgr->GetOption<bool>("WowLegends.AiChat.PartyChat", false);
        guildInterval = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(
            "WowLegends.SocialChatter.GuildIntervalSeconds", 180), 60, 3600);
        groupInterval = std::clamp<uint32>(sConfigMgr->GetOption<uint32>(
            "WowLegends.SocialChatter.GroupIntervalSeconds", 90), 45, 3600);
        pending.clear();
        nextAllowed.clear();
        botNext.clear();
        lastExchange.clear();
    }

    void OnUpdate(uint32 diff) override
    {
        nowMs += diff;
        for (auto it = pending.begin(); it != pending.end();)
        {
            Exchange& exchange = it->second;
            if (!StillValid(exchange))
            {
                it = pending.erase(it);
                continue;
            }
            if (!exchange.lines[0].empty() && nowMs >= exchange.nextLine)
            {
                Player* bot = ObjectAccessor::FindPlayer(exchange.line ? exchange.b : exchange.a);
                if (exchange.guildId)
                {
                    if (Guild* guild = bot->GetGuild())
                        guild->BroadcastToGuild(bot->GetSession(), false, exchange.lines[exchange.line], LANG_UNIVERSAL);
                }
                else
                {
                    WorldPacket packet;
                    ChatHandler::BuildChatPacket(packet, ChatMsg(exchange.chatType), LANG_UNIVERSAL,
                        bot->GetGUID(), ObjectGuid::Empty, exchange.lines[exchange.line], 0, bot->GetName(), "");
                    bot->GetGroup()->BroadcastPacket(&packet, false);
                }
                if (++exchange.line == 2)
                {
                    lastExchange[exchange.key] = exchange.lines[0] + "\n" + exchange.lines[1];
                    it = pending.erase(it);
                    continue;
                }
                exchange.nextLine = nowMs + urand(4000, 7000);
            }
            ++it;
        }
        if (!enabled || (tickMs += diff) < 5000)
            return;
        tickMs = 0;
        for (auto it = nextAllowed.begin(); it != nextAllowed.end();)
            if (it->second + 3600000 < nowMs)
            {
                lastExchange.erase(it->first);
                it = nextAllowed.erase(it);
            }
            else
                ++it;
        for (auto it = botNext.begin(); it != botNext.end();)
            if (it->second < nowMs)
                it = botNext.erase(it);
            else
                ++it;
        std::vector<Player*> witnesses;
        for (auto const& entry : ObjectAccessor::GetPlayers())
            if (Real(entry.second))
                witnesses.push_back(entry.second);
        for (size_t i = witnesses.size(); i > 1; --i)
            std::swap(witnesses[i - 1], witnesses[urand(0, uint32(i - 1))]);
        for (Player* witness : witnesses)
        {
            bool guildFirst = urand(0, 1) != 0;
            for (bool guild : {guildFirst, !guildFirst})
                if ((guild ? guildEnabled : groupEnabled) && Begin(witness, guild))
                    return; // at most one paid generation per scheduler tick
        }
    }
};

void AddWowLegendsSocialChatterScripts()
{
    new WowLegendsSocialChatterWorld();
}
