#include "ScriptMgr.h"
#include "Player.h"
#include "Configuration/Config.h"
#include "Creature.h"
#include "Guild.h"
#include "SpellAuraEffects.h"
#include "Chat.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "GuildMgr.h"
#include "Define.h"
#include "GossipDef.h"
#include "DataMap.h"
#include "GameObject.h"
#include "Transport.h"
#include "Maps/MapMgr.h"
#include "WorldSession.h"
#include "DatabaseEnv.h"
#include "guildhouse.h"
#include <array>
#include <unordered_map>

namespace
{
    // id -> text per LocaleConstant, preloaded once so UI strings don't hit the
    // database on the world thread for every message.
    std::unordered_map<uint32, std::array<std::string, TOTAL_LOCALES>> _guildHouseLocaleTexts;
} // namespace

void LoadGuildHouseLocales()
{
    _guildHouseLocaleTexts.clear();

    QueryResult result = WorldDatabase.Query("SELECT `Id`, `Locale`, `Text` FROM `mod_guildhouse_locale`");
    if (!result)
    {
        LOG_WARN("modules", "GUILDHOUSE: `mod_guildhouse_locale` is empty or missing; localized texts unavailable.");
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        uint32 id = fields[0].Get<uint32>();
        LocaleConstant locale = GetLocaleByName(fields[1].Get<std::string>());
        _guildHouseLocaleTexts[id][locale] = fields[2].Get<std::string>();
        ++count;
    } while (result->NextRow());

    LOG_INFO("modules", "GUILDHOUSE: Loaded {} localized text entries.", count);
}

std::string GetGuildHouseLocaleText(uint32 id, Player* player)
{
    if (!player || !player->GetSession())
        return {};

    auto it = _guildHouseLocaleTexts.find(id);
    if (it == _guildHouseLocaleTexts.end())
        return {};

    LocaleConstant locale = player->GetSession()->GetSessionDbLocaleIndex();
    if (locale < TOTAL_LOCALES && !it->second[locale].empty())
        return it->second[locale];

    return it->second[LOCALE_enUS]; // fall back to English
}

class GuildData : public DataMap::Base
{
public:
    GuildData() {}
    GuildData(uint32 phase, float posX, float posY, float posZ, float ori) : phase(phase), posX(posX), posY(posY), posZ(posZ), ori(ori) {}
    uint32 phase = 0;
    bool inGuildHouse = false;
    float posX;
    float posY;
    float posZ;
    float ori;
};

class GuildHelper : public GuildScript
{

public:
    GuildHelper() : GuildScript("GuildHelper") {}

    void OnCreate(Guild* /*guild*/, Player* leader, const std::string& /*name*/)
    {
        ChatHandler(leader->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_YOU_NOW_OWN_A_GUILD, leader).c_str());
    }

    void OnRemoveMember(Guild* /*guild*/, Player* player, bool /*isDisbanding*/, bool /*isKicked*/) override
    {
        if (!player || player->GetZoneId() != 876)
            return;
        player->RemoveRestState();
        player->SetPhaseMask(PHASEMASK_NORMAL, true);
        if (player->GetTeamId() == TEAM_ALLIANCE)
            player->TeleportTo(0, -8833.38f, 628.628f, 94.0066f, 1.0f);
        else
            player->TeleportTo(1, 1486.048f, -4415.14f, 24.1875f, 0.13f);
    }

    uint32 GetGuildPhase(Guild* guild)
    {
        return guild->GetId() + 10;
    }

    void OnDisband(Guild* guild)
    {

        if (RemoveGuildHouse(guild))
        {
            LOG_INFO("modules", "GUILDHOUSE: Deleting Guild House data due to disbanding of guild...");
        }
        else
        {
            LOG_INFO("modules", "GUILDHOUSE: Error deleting Guild House data during disbanding of guild!!");
        }
    }

    bool RemoveGuildHouse(Guild* guild)
    {
        if (!CharacterDatabase.Query("SELECT id FROM guild_house WHERE guild={}", guild->GetId()))
            return false;
        uint32 guildPhase = GetGuildPhase(guild);
        QueryResult CreatureResult;
        QueryResult GameobjResult;

        // Lets find all of the gameobjects to be removed
        GameobjResult = WorldDatabase.Query("SELECT `guid` FROM `gameobject` WHERE `map`=1 AND `phaseMask`={} AND position_x BETWEEN 16000 AND 16400 AND position_y BETWEEN 16000 AND 16500", guildPhase);
        // Lets find all of the creatures to be removed
        CreatureResult = WorldDatabase.Query("SELECT `guid` FROM `creature` WHERE `map`=1 AND `phaseMask`={} AND position_x BETWEEN 16000 AND 16400 AND position_y BETWEEN 16000 AND 16500", guildPhase);

        Map* map = sMapMgr->FindMap(1, 0);
        if (!map)
            return false;
        // Remove creatures from the deleted guild house map
        if (CreatureResult)
        {
            do
            {
                Field* fields = CreatureResult->Fetch();
                uint32 lowguid = fields[0].Get<int32>();
if (CreatureData const* cr_data = sObjectMgr->GetCreatureData(lowguid))
                {
                    map->LoadGrid(cr_data->posX, cr_data->posY);
                    if (Creature* creature = map->GetCreature(ObjectGuid::Create<HighGuid::Unit>(cr_data->id, lowguid)))
                    {
                        creature->CombatStop();
                        creature->DeleteFromDB();
                        creature->AddObjectToRemoveList();
                    }
                }
            } while (CreatureResult->NextRow());
        }

        // Remove gameobjects from the deleted guild house map
        if (GameobjResult)
        {
            do
            {
                Field *fields = GameobjResult->Fetch();
                uint32 lowguid = fields[0].Get<int32>();
if (GameObjectData const* go_data = sObjectMgr->GetGameObjectData(lowguid))
                {
                    map->LoadGrid(go_data->posX, go_data->posY);
                    if (GameObject* gobject = map->GetGameObject(ObjectGuid::Create<HighGuid::GameObject>(go_data->id, lowguid)))
                    {
                        gobject->SetRespawnTime(0);
                        gobject->Delete();
                        gobject->DeleteFromDB();
                        gobject->CleanupsBeforeDelete();
                        // delete gobject;
                    }
                }

            } while (GameobjResult->NextRow());
        }

        // Delete actual guild_house data from characters database
        CharacterDatabase.DirectExecute("DELETE FROM `guild_house` WHERE `guild`={}", guild->GetId());

        return true;
    }
};

class GuildHouseSeller : public CreatureScript
{

public:
    GuildHouseSeller() : CreatureScript("GuildHouseSeller") {}

    struct GuildHouseSellerAI : public ScriptedAI
    {
        GuildHouseSellerAI(Creature* creature) : ScriptedAI(creature) {}

        void UpdateAI(uint32 /*diff*/) override
        {
            me->SetFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP);
        }
    };

    CreatureAI * GetAI(Creature* creature) const override
    {
        return new GuildHouseSellerAI(creature);
    }

    bool OnGossipHello(Player* player, Creature* creature) override
    {
        ClearGossipMenuFor(player);
        if (!player->GetGuild())
        {
            ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_NOT_IN_GUILD, player).c_str());
            CloseGossipMenuFor(player);
            return false;
        }

        QueryResult has_gh = CharacterDatabase.Query("SELECT id, `guild` FROM `guild_house` WHERE guild = {}", player->GetGuildId());

        // Only show Teleport option if guild owns a guild house
        if (has_gh)
        {
            AddGossipItemFor(player, GOSSIP_ICON_TABARD,
                GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_TELEPORT_TO_HOUSE, player),
                GUILDHOUSE_GOSSIP_SENDER, 1);

            // Only show "Sell" option if they have a guild house & have permission to sell it
            Guild* guild = sGuildMgr->GetGuildById(player->GetGuildId());
            Guild::Member const* memberMe = guild->GetMember(player->GetGUID());
            if (memberMe && memberMe->IsRankNotLower(sConfigMgr->GetOption<int32>("GuildHouseSellRank", 0)))
            {
                AddGossipItemFor(player, GOSSIP_ICON_TABARD,
                    GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_SELL_HOUSE, player),
                    GUILDHOUSE_GOSSIP_SENDER, 3,
                    GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_SELL_HOUSE_CONFIRM, player), 0, false);
            }
        }
        else
        {
            // Only leader of the guild can buy guild house & only if they don't already have a guild house
            if (player->GetGuild()->GetLeaderGUID() == player->GetGUID())
            {
                AddGossipItemFor(player, GOSSIP_ICON_TABARD,
                    GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_BUY_HOUSE, player),
                    GUILDHOUSE_GOSSIP_SENDER, 2);
            }
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_CLOSE, player),
            GUILDHOUSE_GOSSIP_SENDER, 5);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        Guild* guild = player->GetGuild();
        if (sender != GUILDHOUSE_GOSSIP_SENDER || !guild)
        {
            CloseGossipMenuFor(player);
            return true;
        }
        if (action != 1 && action != 2 && action != 3 && action != 5 && action != 100)
            return true;
        if ((action == 2 || action == 100) && guild->GetLeaderGUID() != player->GetGUID())
            return true;
        if (action == 3)
        {
            Guild::Member const* member = guild->GetMember(player->GetGUID());
            if (!member || !member->IsRankNotLower(sConfigMgr->GetOption<int32>("GuildHouseSellRank", 0)))
                return true;
        }
        int32 price = sConfigMgr->GetOption<int32>("CostGuildHouse", 10000000);
        if (action == 100 && (price < 0 || !player->HasEnoughMoney(price) ||
            CharacterDatabase.Query("SELECT id FROM guild_house WHERE guild={}", guild->GetId())))
        {
            ChatHandler(player->GetSession()).SendSysMessage("길드하우스 보유 여부와 구매 금액을 확인해 주세요.");
            CloseGossipMenuFor(player);
            return true;
        }
        uint32 map = 1;
        float posX = 16222.972f;
        float posY = 16267.802f;
        float posZ = 13.136777f;
        float ori = 1.461173f;

        switch (action)
        {
        case 100: // GM Island
            map = 1;
            posX = 16222.972f;
            posY = 16267.802f;
            posZ = 13.136777f;
            ori = 1.461173f;
            break;
        case 5: // close
            CloseGossipMenuFor(player);
            break;
        case 4: // --- MORE TO COME ---
            BuyGuildHouse(player->GetGuild(), player, creature);
            break;
        case 3: // sell back guild house
        {
            QueryResult has_gh = CharacterDatabase.Query("SELECT id, `guild` FROM `guild_house` WHERE guild={}", player->GetGuildId());
            if (!has_gh)
            {
                ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GUILD_HAS_NO_HOUSE, player).c_str());
                CloseGossipMenuFor(player);
                return false;
            }

            // calculate total gold returned: 1) cost of guild house and cost of each purchase made
            if (RemoveGuildHouse(player))
            {
                ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_HOUSE_SOLD_SUCCESS, player).c_str());
                player->GetGuild()->BroadcastToGuild(player->GetSession(), false, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_BROADCAST_HOUSE_SOLD, player).c_str(), LANG_UNIVERSAL);
                player->ModifyMoney(+(sConfigMgr->GetOption<int32>("CostGuildHouse", 10000000) / 2));
                LOG_INFO("modules", "GUILDHOUSE: Successfully returned money and sold Guild House");
                CloseGossipMenuFor(player);
            }
            else
            {
                ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_HOUSE_SOLD_ERROR, player).c_str());
                CloseGossipMenuFor(player);
            }
            break;
        }
        case 2: // buy guild house
            BuyGuildHouse(player->GetGuild(), player, creature);
            break;
        case 1: // teleport to guild house
            CloseGossipMenuFor(player);
            TeleportToOwnedGuildHouse(player);
            break;
        }

        if (action == 100)
        {
            CharacterDatabase.DirectExecute("INSERT INTO `guild_house` (guild, phase, map, positionX, positionY, positionZ, orientation) VALUES ({}, {}, {}, {}, {}, {}, {})", player->GetGuildId(), GetGuildPhase(player), map, posX, posY, posZ, ori);
            if (!CharacterDatabase.Query("SELECT id FROM guild_house WHERE guild={}", player->GetGuildId()))
                return true;
            player->ModifyMoney(-(sConfigMgr->GetOption<int32>("CostGuildHouse", 10000000)));
            // Msg to purchaser and Msg Guild as purchaser
            ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_HOUSE_PURCHASED_SUCCESS, player).c_str());
            player->GetGuild()->BroadcastToGuild(player->GetSession(), false, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_BROADCAST_HOUSE_PURCHASED, player).c_str(), LANG_UNIVERSAL);
            player->GetGuild()->BroadcastToGuild(player->GetSession(), false, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_BROADCAST_USE_TELEPORT, player).c_str(), LANG_UNIVERSAL);
            LOG_INFO("modules", "GUILDHOUSE: GuildId: '{}' has purchased a guildhouse", player->GetGuildId());

            // Spawn a portal and the guild house butler automatically as part of purchase.
            SpawnStarterPortal(player);
            SpawnButlerNPC(player);
            CloseGossipMenuFor(player);
        }

        return true;
    }

    uint32 GetGuildPhase(Player* player)
    {
        return player->GetGuildId() + 10;
    }

    bool RemoveGuildHouse(Player* player)
    {

        uint32 guildPhase = GetGuildPhase(player);
        QueryResult CreatureResult;
        QueryResult GameobjResult;
        Map *map = sMapMgr->FindMap(1, 0);
        if (!map)
            return false;
        // Lets find all of the gameobjects to be removed
        GameobjResult = WorldDatabase.Query("SELECT `guid` FROM `gameobject` WHERE `map`=1 AND `phaseMask`={} AND position_x BETWEEN 16000 AND 16400 AND position_y BETWEEN 16000 AND 16500", guildPhase);
        // Lets find all of the creatures to be removed
        CreatureResult = WorldDatabase.Query("SELECT `guid` FROM `creature` WHERE `map`=1 AND `phaseMask`={} AND position_x BETWEEN 16000 AND 16400 AND position_y BETWEEN 16000 AND 16500", guildPhase);

        // Remove creatures from the deleted guild house map
        if (CreatureResult)
        {
            do
            {
                Field* fields = CreatureResult->Fetch();
                uint32 lowguid = fields[0].Get<uint32>();
                if (CreatureData const* cr_data = sObjectMgr->GetCreatureData(lowguid))
                {
                    map->LoadGrid(cr_data->posX, cr_data->posY);
                    if (Creature* creature = map->GetCreature(ObjectGuid::Create<HighGuid::Unit>(cr_data->id, lowguid)))
                    {
                        creature->CombatStop();
                        creature->DeleteFromDB();
                        creature->AddObjectToRemoveList();
                    }
                }
            } while (CreatureResult->NextRow());
        }

        // Remove gameobjects from the deleted guild house map
        if (GameobjResult)
        {
            do
            {
                Field* fields = GameobjResult->Fetch();
                uint32 lowguid = fields[0].Get<uint32>();
                if (GameObjectData const* go_data = sObjectMgr->GetGameObjectData(lowguid))
                {
                    map->LoadGrid(go_data->posX, go_data->posY);
                    if (GameObject* gobject = map->GetGameObject(ObjectGuid::Create<HighGuid::GameObject>(go_data->id, lowguid)))
                    {
                        gobject->SetRespawnTime(0);
                        gobject->Delete();
                        gobject->DeleteFromDB();
                        gobject->CleanupsBeforeDelete();
                        // delete gobject;
                    }
                }

            } while (GameobjResult->NextRow());
        }

        // Delete actual guild_house data from characters database
        CharacterDatabase.DirectExecute("DELETE FROM `guild_house` WHERE `guild`={}", player->GetGuildId());

        return true;
    }

    void SpawnStarterPortal(Player* player)
    {

        uint32 entry = 0;
        float posX;
        float posY;
        float posZ;
        float ori;

        Map* map = sMapMgr->FindMap(1, 0);

        if (!map)
            return;

        if (player->GetTeamId() == TEAM_ALLIANCE)
        {
            // Portal to Stormwind
            entry = GetGameObjectEntry(0);
        }
        else
        {
            // Portal to Orgrimmar
            entry = GetGameObjectEntry(4);
        }

        if (entry == 0)
        {
            LOG_INFO("modules", "Error with SpawnStarterPortal in GuildHouse Module!");
            return;
        }

        QueryResult result = WorldDatabase.Query("SELECT `posX`, `posY`, `posZ`, `orientation` FROM `guild_house_spawns` WHERE `entry`={}", entry);

        if (!result)
        {
            LOG_INFO("modules", "GUILDHOUSE: Unable to find data on portal for entry: {}", entry);
            return;
        }

        do
        {
            Field* fields = result->Fetch();
            posX = fields[0].Get<float>();
            posY = fields[1].Get<float>();
            posZ = fields[2].Get<float>();
            ori = fields[3].Get<float>();

        } while (result->NextRow());

        uint32 objectId = entry;
        if (!objectId)
        {
            LOG_INFO("modules", "GUILDHOUSE: objectId IS NULL, should be '{}'", entry);
            return;
        }

        const GameObjectTemplate* objectInfo = sObjectMgr->GetGameObjectTemplate(objectId);

        if (!objectInfo)
        {
            LOG_INFO("modules", "GUILDHOUSE: objectInfo is NULL!");
            return;
        }

        if (objectInfo->displayId && !sGameObjectDisplayInfoStore.LookupEntry(objectInfo->displayId))
        {
            LOG_INFO("modules", "GUILDHOUSE: Unable to find displayId??");
            return;
        }

        GameObject* object = sObjectMgr->IsGameObjectStaticTransport(objectInfo->entry) ? new StaticTransport() : new GameObject();
        ObjectGuid::LowType guidLow = player->GetMap()->GenerateLowGuid<HighGuid::GameObject>();

        if (!object->Create(guidLow, objectInfo->entry, map, GetGuildPhase(player), posX, posY, posZ, ori, G3D::Quat(), 0, GO_STATE_READY))
        {
            delete object;
            LOG_INFO("modules", "GUILDHOUSE: Unable to create object!!");
            return;
        }

        // fill the gameobject data and save to the db
        object->SaveToDB(sMapMgr->FindMap(1, 0)->GetId(), (1 << sMapMgr->FindMap(1, 0)->GetSpawnMode()), GetGuildPhase(player));
        guidLow = object->GetSpawnId();
        // delete the old object and do a clean load from DB with a fresh new GameObject instance.
        // this is required to avoid weird behavior and memory leaks
        delete object;

        object = sObjectMgr->IsGameObjectStaticTransport(objectInfo->entry) ? new StaticTransport() : new GameObject();
        // this will generate a new guid if the object is in an instance
        if (!object->LoadGameObjectFromDB(guidLow, sMapMgr->FindMap(1, 0), true))
        {
            delete object;
            return;
        }

        // TODO: is it really necessary to add both the real and DB table guid here ?
        sObjectMgr->AddGameobjectToGrid(guidLow, sObjectMgr->GetGameObjectData(guidLow));
        CloseGossipMenuFor(player);
    }

    void SpawnButlerNPC(Player* player)
    {
        uint32 entry = GetCreatureEntry(1);
        float posX = 16202.185547f;
        float posY = 16255.916992f;
        float posZ = 21.160221f;
        float ori = 6.195375f;

        Map* map = sMapMgr->FindMap(1, 0);
        if (!map)
            return;
        Creature *creature = new Creature();

        if (!creature->Create(map->GenerateLowGuid<HighGuid::Unit>(), map, player->GetPhaseMaskForSpawn(), entry, 0, posX, posY, posZ, ori))
        {
            delete creature;
            return;
        }
        creature->SaveToDB(map->GetId(), (1 << map->GetSpawnMode()), GetGuildPhase(player));
        uint32 lowguid = creature->GetSpawnId();

        creature->CleanupsBeforeDelete();
        delete creature;
        creature = new Creature();
        if (!creature->LoadCreatureFromDB(lowguid, map))
        {
            delete creature;
            return;
        }

        sObjectMgr->AddCreatureToGrid(lowguid, sObjectMgr->GetCreatureData(lowguid));
        return;
    }

    bool BuyGuildHouse(Guild* guild, Player* player, Creature* creature)
    {
        QueryResult result = CharacterDatabase.Query("SELECT `id`, `guild` FROM `guild_house` WHERE `guild`={}", guild->GetId());

        if (result)
        {
            ChatHandler(player->GetSession()).PSendSysMessage("%s", GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GUILD_ALREADY_HAS_HOUSE, player).c_str());
            CloseGossipMenuFor(player);
            return false;
        }

        ClearGossipMenuFor(player);
        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_GOSSIP_GM_ISLAND, player), GUILDHOUSE_GOSSIP_SENDER, 100, GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CONFIRM_BUY_GM_ISLAND, player), sConfigMgr->GetOption<int32>("CostGuildHouse", 10000000), false);
        // Removing this tease for now, as right now the phasing code is specific go GM Island, so it's not a simple thing to add new areas yet.
        // AddGossipItemFor(player, GOSSIP_ICON_CHAT, " ----- More to Come ----", GUILDHOUSE_GOSSIP_SENDER, 4);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, creature->GetGUID());
        return true;
    }

};

class GuildHousePlayerScript : public PlayerScript
{
public:
    GuildHousePlayerScript() : PlayerScript("GuildHousePlayerScript") {}

    void OnPlayerLogin(Player* player)
    {
        CheckPlayer(player);
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 /*newArea*/)
    {
        if (newZone == 876)
            CheckPlayer(player);
        else if (GuildData* data = player->CustomData.GetDefault<GuildData>("mod-guildhouse.phase"); data->inGuildHouse)
        {
            data->inGuildHouse = false;
            player->SetPhaseMask(GetNormalPhase(player), true);
        }
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 mapid, float x, float y, float z, float orientation, uint32 options, Unit* target)
    {
        (void)mapid;
        (void)x;
        (void)y;
        (void)z;
        (void)orientation;
        (void)options;
        (void)target;

        if (player->GetZoneId() == 876 && player->GetAreaId() == 876) // GM Island
        {
            // Remove the rested state when teleporting from the guild house
            player->RemoveRestState();
            if (mapid != 1 || x < 16000 || x > 16400 || y < 16000 || y > 16500)
                player->SetPhaseMask(GetNormalPhase(player), true);
        }

        return true;
    }

    uint32 GetNormalPhase(Player* player) const
    {
        if (player->IsGameMaster())
            return PHASEMASK_ANYWHERE;

        uint32 phase = player->GetPhaseByAuras();
        if (!phase)
            return PHASEMASK_NORMAL;
        else
            return phase;
    }

    void CheckPlayer(Player* player)
    {
        if (player->GetZoneId() != 876 || player->GetAreaId() != 876)
            return; // Do not reset quest/WOW/progression phases across the rest of the world.
        GuildData* guildData = player->CustomData.GetDefault<GuildData>("mod-guildhouse.phase");
        QueryResult result = CharacterDatabase.Query("SELECT `id`, `guild`, `phase`, `map`,`positionX`, `positionY`, `positionZ`, `orientation` FROM guild_house WHERE `guild` = {}", player->GetGuildId());

        if (result)
        {
            do
            {
                // commented out due to travis, but keeping for future expansion into other areas
                Field *fields = result->Fetch();
                // uint32 id = fields[0].Get<uint32>();        // fix for travis
                // uint32 guild = fields[1].Get<uint32>();     // fix for travis
                guildData->phase = fields[2].Get<uint32>();
                // uint32 map = fields[3].Get<uint32>();       // fix for travis
                // guildData->posX = fields[4].Get<float>();   // fix for travis
                // guildData->posY = fields[5].Get<float>();   // fix for travis
                // guildData->posZ = fields[6].Get<float>();   // fix for travis
                // guildData->ori = fields[7].Get<float>();   // fix for travis

            } while (result->NextRow());
        }

        if (player->GetZoneId() == 876 && player->GetAreaId() == 876) // GM Island
        {
            // Set the guild house as a rested area
            player->SetRestState(0);

            // If player is not in a guild he doesnt have a guild house teleport away
            // TODO: What if they are in a guild, but somehow are in the wrong phaseMask and seeing someone else's area?

            if (!result || !player->GetGuild())
            {
                ChatHandler(player->GetSession()).SendSysMessage("소속 길드에 길드하우스가 없어 대도시로 이동합니다.");
                teleportToDefault(player);
                return;
            }

            player->SetPhaseMask(guildData->phase, true);
            guildData->inGuildHouse = true;
        }
        else
            player->SetPhaseMask(GetNormalPhase(player), true);
    }

    void teleportToDefault(Player* player)
    {
        if (player->GetTeamId() == TEAM_ALLIANCE)
            player->TeleportTo(0, -8833.379883f, 628.627991f, 94.006599f, 1.0f);
        else
            player->TeleportTo(1, 1486.048340f, -4415.140625f, 24.187496f, 0.13f);
    }
};

using namespace Acore::ChatCommands;

class GuildHouseCommand : public CommandScript
{
public:
    GuildHouseCommand() : CommandScript("GuildHouseCommand") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable GuildHouseCommandTable =
        {
            {"teleport", HandleGuildHouseTeleCommand, SEC_PLAYER, Console::No},
            {"butler", HandleSpawnButlerCommand, SEC_PLAYER, Console::No},
        };

        static ChatCommandTable GuildHouseCommandBaseTable =
        {
            {"guildhouse", GuildHouseCommandTable},
            {"gh", GuildHouseCommandTable}
        };

        return GuildHouseCommandBaseTable;
    }

    static uint32 GetGuildPhase(Player* player)
    {
        return player->GetGuildId() + 10;
    }

    static bool HandleSpawnButlerCommand(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player)
            return false;
        Map* map = player->GetMap();

        if (!player->GetGuild() || (player->GetGuild()->GetLeaderGUID() != player->GetGUID()))
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_NEED_GUILDMASTER, player).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (player->GetAreaId() != 876)
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_NEED_IN_GUILDHOUSE, player).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (player->FindNearestCreature(GetCreatureEntry(1), VISIBLE_RANGE, true))
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_BUTLER_ALREADY_EXISTS, player).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        float posX = 16202.185547f;
        float posY = 16255.916992f;
        float posZ = 21.160221f;
        float ori = 6.195375f;

        Creature* creature = new Creature();
        if (!creature->Create(map->GenerateLowGuid<HighGuid::Unit>(), map, GetGuildPhase(player), GetCreatureEntry(1), 0, posX, posY, posZ, ori))
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_BUTLER_ALREADY_EXISTS, player).c_str());
            handler->SetSentErrorMessage(true);
            delete creature;
            return false;
        }
        creature->SaveToDB(player->GetMapId(), (1 << player->GetMap()->GetSpawnMode()), GetGuildPhase(player));
        uint32 lowguid = creature->GetSpawnId();

        creature->CleanupsBeforeDelete();
        delete creature;
        creature = new Creature();
        if (!creature->LoadCreatureFromDB(lowguid, player->GetMap()))
        {
            handler->SendSysMessage(GetGuildHouseLocaleText(GUILDHOUSE_TEXT_CMD_BUTLER_ADD_ERROR, player).c_str());
            handler->SetSentErrorMessage(true);
            delete creature;
            return false;
        }

        sObjectMgr->AddCreatureToGrid(lowguid, sObjectMgr->GetCreatureData(lowguid));
        return true;
    }

    static bool HandleGuildHouseTeleCommand(ChatHandler* handler)
    {
        return TeleportToOwnedGuildHouse(handler->GetPlayer());
    }

};

class GuildHouseGlobal : public GlobalScript
{
public:
    GuildHouseGlobal() : GlobalScript("GuildHouseGlobal") {}

    void OnBeforeWorldObjectSetPhaseMask(WorldObject const* worldObject, uint32 & /*oldPhaseMask*/, uint32 & /*newPhaseMask*/, bool &useCombinedPhases, bool & /*update*/) override
    {
        if (worldObject->GetZoneId() == 876)
            useCombinedPhases = false;
    }
};

class GuildHouseWorld : public WorldScript
{
public:
    GuildHouseWorld() : WorldScript("GuildHouseWorld") {}

    void OnStartup() override
    {
        LoadGuildHouseLocales();
    }
};

bool TeleportToOwnedGuildHouse(Player* player)
{
    if (!player || !player->GetSession())
        return false;
    if (!player->GetGuild())
    {
        ChatHandler(player->GetSession()).SendSysMessage("길드에 가입한 캐릭터만 길드하우스를 이용할 수 있습니다.");
        return false;
    }
    if (!player->IsAlive() || player->IsInCombat() || player->IsInFlight() || player->IsBeingTeleported() ||
        player->InBattleground() || player->GetTransport())
    {
        ChatHandler(player->GetSession()).SendSysMessage("전투·비행·전장·이동 중이거나 사망한 상태에서는 길드하우스로 이동할 수 없습니다.");
        return false;
    }
    QueryResult result = CharacterDatabase.Query("SELECT phase,map,positionX,positionY,positionZ,orientation FROM guild_house WHERE guild={}", player->GetGuildId());
    if (!result)
    {
        ChatHandler(player->GetSession()).SendSysMessage("길드하우스가 없습니다. 길드장이 비행 조련사의 길드하우스 메뉴에서 구매할 수 있습니다.");
        return false;
    }
    Field* fields = result->Fetch();
    uint32 phase = fields[0].Get<uint32>();
    uint32 map = fields[1].Get<uint32>();
    float x = fields[2].Get<float>(), y = fields[3].Get<float>(), z = fields[4].Get<float>(), o = fields[5].Get<float>();
    // This upstream module supports GM Island only. Never trust arbitrary DB destinations.
    if (phase != player->GetGuildId() + 10 || map != 1 || !(x >= 16000 && x <= 16400) ||
        !(y >= 16000 && y <= 16500) || !(z >= -100 && z <= 500))
    {
        ChatHandler(player->GetSession()).SendSysMessage("길드하우스 이동 정보가 올바르지 않습니다. 관리자에게 문의해 주세요.");
        return false;
    }
    return player->TeleportTo(map, x, y, z, o);
}

class GuildHouseFlightMaster : public AllCreatureScript
{
public:
    explicit GuildHouseFlightMaster(GuildHouseSeller* seller) : AllCreatureScript("GuildHouseFlightMaster"), _seller(seller) { }

    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        // Custom scripted taxi NPCs retain their own interaction contracts.
        if (!player->GetGuild() || !creature->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER) || creature->GetScriptId())
            return false;
        player->GetSession()->SendLearnNewTaxiNode(creature);
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);
        AddGossipItemFor(player, GOSSIP_ICON_TABARD, "길드하우스 이용", GUILDHOUSE_GOSSIP_SENDER, 51001);
        player->SendPreparedGossip(creature);
        return true;
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 sender, uint32 action) override
    {
        if (sender != GUILDHOUSE_GOSSIP_SENDER || !creature->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER))
            return false;
        if (!player->GetGuild())
        {
            CloseGossipMenuFor(player);
            ChatHandler(player->GetSession()).SendSysMessage("길드에 가입한 캐릭터만 길드하우스를 이용할 수 있습니다.");
            return true;
        }
        if (action == 51001)
            _seller->OnGossipHello(player, creature);
        else
            _seller->OnGossipSelect(player, creature, sender, action);
        return true;
    }
private:
    GuildHouseSeller* _seller;
};

void AddGuildHouseScripts()
{
    new GuildHelper();
    GuildHouseSeller* seller = new GuildHouseSeller();
    new GuildHouseFlightMaster(seller);
    new GuildHousePlayerScript();
    new GuildHouseCommand();
    new GuildHouseGlobal();
    new GuildHouseWorld();
}
