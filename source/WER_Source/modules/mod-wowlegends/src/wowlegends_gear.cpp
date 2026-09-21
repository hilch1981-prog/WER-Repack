/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - custom .gear command
 *
 * Gears the target through the same GEAR-ONLY factory path the bots' own
 * $autogear order uses: spec-aware item selection + ammo + enchants + gems +
 * repair, and nothing else. Old equipped items go to the target's bags when
 * there is room. Adds `.gear undress` on top.
 *
 *   .gear level    -> uncommon (green) leveling gear
 *   .gear rare     -> rare (blue)
 *   .gear epic     -> epic (purple)
 *   .gear max      -> best available (epic) for the current level
 *   .gear undress  -> move all equipped items to bags
 *
 * Targets the selected player, or yourself if none is selected.
 *
 * NEVER route this through `.playerbots bot initself=` / PlayerbotFactory::
 * Randomize(): that is a full "rebuild as a fresh bot" pass that also wipes
 * quest history, bag contents, skills, spells and the hunter pet. v1.4.0
 * shipped that way and cost a player his quest log (bug report 2026-07-18).
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Player.h"
#include "Item.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"

using namespace Acore::ChatCommands;

class WowLegendsGearCommand : public CommandScript
{
public:
    WowLegendsGearCommand() : CommandScript("WowLegendsGearCommand") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable gearCommandTable =
        {
            { "level",   HandleGearLevelCommand,   SEC_GAMEMASTER, Console::No },
            { "rare",    HandleGearRareCommand,    SEC_GAMEMASTER, Console::No },
            { "epic",    HandleGearEpicCommand,    SEC_GAMEMASTER, Console::No },
            { "max",     HandleGearMaxCommand,     SEC_GAMEMASTER, Console::No },
            { "undress", HandleGearUndressCommand, SEC_GAMEMASTER, Console::No },
        };

        static ChatCommandTable baseTable =
        {
            { "gear", gearCommandTable },
        };

        return baseTable;
    }

    // Gear-only: equipment + ammo + enchants/gems + repair. Displaced items are
    // stored to the target's bags by the equip path when there is room.
    static bool GearDirect(ChatHandler* handler, uint32 quality, char const* label)
    {
        Player* target = handler->getSelectedPlayerOrSelf();
        if (!target)
        {
            handler->SendSysMessage("선택한 플레이어가 없습니다.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (target->IsInCombat())
        {
            handler->SendSysMessage("전투 중에는 장비를 바꿀 수 없습니다.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        PlayerbotFactory factory(target, target->GetLevel(), quality);
        factory.InitEquipment(false);
        factory.InitAmmo();
        if (target->GetLevel() >= sPlayerbotAIConfig.minEnchantingBotLevel)
            factory.ApplyEnchantAndGemsNew();
        target->DurabilityRepairAll(false, 1.0f, false);

        handler->PSendSysMessage("{}에게 특성에 맞는 장비 {}개를 지급했습니다 (마법부여·보석·수리 포함). 교체한 장비는 빈 가방 칸에 넣었습니다. 퀘스트·가방·숙련·소환수는 유지합니다.",
            target->GetName(), label);
        return true;
    }

    static bool HandleGearLevelCommand(ChatHandler* handler) { return GearDirect(handler, ITEM_QUALITY_UNCOMMON, "uncommon"); }
    static bool HandleGearRareCommand(ChatHandler* handler)  { return GearDirect(handler, ITEM_QUALITY_RARE, "rare"); }
    static bool HandleGearEpicCommand(ChatHandler* handler)  { return GearDirect(handler, ITEM_QUALITY_EPIC, "epic"); }
    static bool HandleGearMaxCommand(ChatHandler* handler)   { return GearDirect(handler, ITEM_QUALITY_EPIC, "best available"); }

    static bool HandleGearUndressCommand(ChatHandler* handler)
    {
        Player* target = handler->getSelectedPlayerOrSelf();
        if (!target)
        {
            handler->SendSysMessage("선택한 플레이어가 없습니다.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 moved = 0;
        uint32 failed = 0;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            Item* item = target->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
                continue;

            ItemPosCountVec dest;
            InventoryResult msg = target->CanStoreItem(NULL_BAG, NULL_SLOT, dest, item, false);
            if (msg == EQUIP_ERR_OK)
            {
                target->RemoveItem(INVENTORY_SLOT_BAG_0, slot, true);
                target->StoreItem(dest, item, true);
                ++moved;
            }
            else
                ++failed;
        }

        if (failed)
            handler->PSendSysMessage("장비 {}개를 가방으로 옮겼습니다. 가방이 가득 차 {}개는 착용 중입니다.", moved, failed);
        else
            handler->PSendSysMessage("장비 {}개를 가방으로 옮겼습니다.", moved);
        return true;
    }
};

void AddWowLegendsGearScripts()
{
    new WowLegendsGearCommand();
}
