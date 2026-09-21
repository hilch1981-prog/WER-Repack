/* WOW Legends local extension; AGPL-3.0-or-later. */
#include "ScriptMgr.h"
#include "Configuration/Config.h"
#include "AiObjectContext.h"
#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "Creature.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <algorithm>
#include <ctime>
#include <map>
#include <vector>

namespace
{
    bool enabled = false;
    uint32 tickMs = 0;
    uint64 nowMs = 0;
    std::map<ObjectGuid, uint64> nextAttempt;
    constexpr uint32 Reserve = 10000; // 1 gold, never create spending money.
    constexpr uint32 MaxBuy = 100000; // 10 gold per purchase, including the stack.

    bool Eligible(Player* bot, PlayerbotAI* ai)
    {
        return ai && !ai->GetMaster() && bot->IsInWorld() && bot->GetSession()->IsBot() &&
            sRandomPlayerbotMgr.IsRandomBot(bot) && bot->IsAlive() && !bot->GetGroup() &&
            !bot->IsInCombat() && !bot->IsBeingTeleported() && !bot->IsInFlight() &&
            !bot->InBattleground() && !bot->InArena();
    }

    // Only normal auction mail, only at a real mailbox. Never delete mail here.
    bool CollectAuctionMail(Player* bot, PlayerbotAI* ai)
    {
        Mail* candidate = nullptr;
        for (Mail* mail : bot->GetMails())
            if (mail && mail->messageType == MAIL_AUCTION && mail->state != MAIL_STATE_DELETED &&
                !mail->COD && mail->deliver_time <= std::time(nullptr) && (mail->money || mail->HasItems()))
            {
                candidate = mail;
                break;
            }
        if (!candidate)
            return false;
        for (ObjectGuid guid : ai->GetAiObjectContext()->GetValue<GuidVector>("nearest game objects")->Get())
        {
            if (!bot->GetGameObjectIfCanInteractWith(guid, GAMEOBJECT_TYPE_MAILBOX))
                continue;
            uint32 id = candidate->messageID;
            uint32 itemGuid = candidate->items.empty() ? 0 : candidate->items.front().item_guid;
            bool money = candidate->money != 0;
            WorldPacket packet;
            packet << guid << id;
            if (money)
                bot->GetSession()->HandleMailTakeMoney(packet);
            else
            {
                packet << itemGuid;
                bot->GetSession()->HandleMailTakeItem(packet);
            }
            LOG_DEBUG("module", "봇 경매 우편 수령 시도: 봇={} 우편={} 종류={}",
                bot->GetGUID().ToString(), id, money ? "골드" : "아이템");
            return true;
        }
        return false;
    }

    bool Sell(Player* bot, PlayerbotAI* ai, Creature* npc, AuctionHouseObject* house)
    {
        uint32 owned = 0;
        for (auto const& entry : house->GetAuctions())
            if (entry.second->owner == bot->GetGUID())
                ++owned;
        if (owned >= 5)
            return false;
        auto* houseEntry = AuctionHouseMgr::GetAuctionHouseEntryFromFactionTemplate(npc->GetFaction());
        if (!houseEntry)
            return false;
        std::vector<Item*> inventory;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                inventory.push_back(item);
        for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
            if (Bag* bag = bot->GetBagByPos(slot))
                for (uint32 pos = 0; pos < bag->GetBagSize(); ++pos)
                    if (Item* item = bag->GetItemByPos(pos))
                        inventory.push_back(item);
        for (Item* item : inventory)
        {
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto || proto->Class != ITEM_CLASS_TRADE_GOODS || proto->Quality > ITEM_QUALITY_UNCOMMON ||
                !proto->SellPrice || item->IsSoulBound() || !item->CanBeTraded() ||
                ItemUsageValue::IsItemUsefulForQuest(bot, proto))
                continue;
            std::string qualifier = std::to_string(proto->ItemId) + "," + std::to_string(item->GetItemRandomPropertyId());
            if (ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", qualifier)->Get() != ITEM_USAGE_AH)
                continue;
            uint32 count = item->GetCount();
            uint64 price = uint64(proto->SellPrice) * count * 2;
            if (!count || count > 20 || price > MaxBuy)
                continue;
            uint32 deposit = AuctionHouseMgr::GetAuctionDeposit(houseEntry, 12 * HOUR, item, count);
            if (uint64(bot->GetMoney()) < uint64(Reserve) + deposit)
                continue;
            ObjectGuid itemGuid = item->GetGUID();
            WorldPacket packet;
            packet << npc->GetGUID() << uint32(1) << itemGuid << count;
            packet << uint32(price) << uint32(price) << uint32(12 * 60);
            bot->GetSession()->HandleAuctionSellItem(packet);
            bool listed = false;
            for (auto const& entry : house->GetAuctions())
                if (entry.second->owner == bot->GetGUID() && entry.second->item_guid == itemGuid)
                    listed = true;
            LOG_INFO("module", "봇 경매 등록 결과: 봇={} 아이템={} 수량={} 가격={} 등록={}",
                bot->GetGUID().ToString(), proto->ItemId, count, price, listed);
            return true; // Do not dereference the transferred item again.
        }
        return false;
    }

    bool Buy(Player* bot, PlayerbotAI* ai, Creature* npc, AuctionHouseObject* house)
    {
        // Include not-yet-delivered purchases: do not buy the same need repeatedly.
        for (Mail* mail : bot->GetMails())
            if (mail && mail->messageType == MAIL_AUCTION && mail->state != MAIL_STATE_DELETED && mail->HasItems())
                return false;
        uint32 selected = 0;
        uint32 price = MaxBuy + 1;
        for (auto const& entry : house->GetAuctions())
        {
            AuctionEntry const* auction = entry.second;
            if (auction->owner == bot->GetGUID() || !auction->buyout || auction->buyout >= price ||
                !auction->itemCount || auction->itemCount > 20 || auction->expire_time <= std::time(nullptr) ||
                uint64(auction->buyout) + Reserve > bot->GetMoney())
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
            if (!proto || proto->Class != ITEM_CLASS_CONSUMABLE || proto->Quality > ITEM_QUALITY_UNCOMMON ||
                (proto->SubClass != ITEM_SUBCLASS_FOOD && proto->SubClass != ITEM_SUBCLASS_POTION) ||
                !proto->BuyPrice || uint64(auction->buyout) > uint64(proto->BuyPrice) * auction->itemCount * 2 ||
                bot->CanUseItem(proto) != EQUIP_ERR_OK || bot->GetItemCount(proto->ItemId) >= 5)
                continue;
            if (ai->GetAiObjectContext()->GetValue<ItemUsage>("item usage", std::to_string(proto->ItemId))->Get() != ITEM_USAGE_USE)
                continue;
            selected = auction->Id;
            price = auction->buyout;
        }
        if (!selected)
            return false;
        uint32 before = bot->GetMoney();
        WorldPacket packet;
        packet << npc->GetGUID() << selected << price;
        bot->GetSession()->HandleAuctionPlaceBid(packet);
        LOG_INFO("module", "봇 경매 구매 결과: 봇={} 경매={} 가격={} 결제={}",
            bot->GetGUID().ToString(), selected, price, uint64(bot->GetMoney()) + price == before);
        return true;
    }
}

class WowLegendsBotAuctionWorld : public WorldScript
{
public:
    WowLegendsBotAuctionWorld() : WorldScript("WowLegendsBotAuctionWorld",
        {WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE}) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        enabled = sConfigMgr->GetOption<bool>("WowLegends.BotAuction.Enabled", false);
        // Keep cooldowns across reload; toggling config must not bypass the limits.
    }

    void OnUpdate(uint32 diff) override
    {
        nowMs += diff;
        if (!enabled)
            return;
        tickMs += diff;
        if (tickMs < 30000)
            return;
        tickMs = 0;
        for (auto it = nextAttempt.begin(); it != nextAttempt.end();)
            if (it->second <= nowMs)
                it = nextAttempt.erase(it);
            else
                ++it;
        std::vector<Player*> bots;
        for (auto const& entry : ObjectAccessor::GetPlayers())
        {
            Player* bot = entry.second;
            if (!bot || !bot->GetSession() || !bot->GetSession()->IsBot() || nextAttempt.count(bot->GetGUID()))
                continue;
            if (Eligible(bot, sPlayerbotsMgr.GetPlayerbotAI(bot)))
                bots.push_back(bot);
        }
        if (bots.empty())
            return;
        std::size_t start = urand(0, uint32(bots.size() - 1));
        // Bound spatial/item scans independently of the number of online bots.
        for (std::size_t i = 0; i < std::min<std::size_t>(bots.size(), 20); ++i)
        {
            Player* bot = bots[(start + i) % bots.size()];
            PlayerbotAI* ai = sPlayerbotsMgr.GetPlayerbotAI(bot);
            if (CollectAuctionMail(bot, ai))
            {
                nextAttempt[bot->GetGUID()] = nowMs + 30000;
                return;
            }
            for (ObjectGuid guid : ai->GetAiObjectContext()->GetValue<GuidVector>("nearest npcs")->Get())
            {
                Creature* npc = bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_AUCTIONEER);
                if (!npc)
                    continue;
                AuctionHouseObject* house = sAuctionMgr->GetAuctionsMap(npc->GetFaction());
                if (!house)
                    continue;
                nextAttempt[bot->GetGUID()] = nowMs + 15 * MINUTE * IN_MILLISECONDS;
                bool buyFirst = urand(0, 1) != 0;
                bool attempted = buyFirst ? (Buy(bot, ai, npc, house) || Sell(bot, ai, npc, house)) :
                    (Sell(bot, ai, npc, house) || Buy(bot, ai, npc, house));
                if (attempted)
                    return;
                break;
            }
        }
    }
};

void AddWowLegendsBotAuctionScripts()
{
    new WowLegendsBotAuctionWorld();
}
