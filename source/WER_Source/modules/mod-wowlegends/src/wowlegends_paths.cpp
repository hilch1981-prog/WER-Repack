/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - "Paths of Legends": opt-in playthrough challenges
 *
 * A character (level <= MaxLevelToSwear) swears a Path at the Herald of the
 * Fallen - a permanent, self-imposed set of rules for that ONE character.
 * Paths never touch the world or other players: no world-DB rewrites, no
 * realm-wide flags - just per-character gates enforced by hooks. Renouncing
 * a sworn Path is allowed (config) but marks it FORSAKEN forever; the Path
 * does not forgive.
 *
 * The registry ships FOUR Paths (more are table entries + hooks):
 *
 *   THE LONG ROAD - relive the whole history of Azeroth's wars, in order.
 *     Your experience locks at 60 until the lords of the old world have
 *     fallen to you: Ragnaros, Onyxia, Nefarian, C'Thun. Then Outland's
 *     road opens - and locks again at 70 until Illidan falls. Only then
 *     are Northrend and level 80 yours. (Boss kills credit EVERY sworn
 *     group member in the raid, so you clear them with your bot squad.)
 *
 *   THE IRON OATH - sworn at level 1 in your starting clothes: no auction
 *     house, no trades, no mail, no guild bank until level 80. The mail
 *     seal is a core overlay patch (WorldSession::CanOpenMailBox consults
 *     WlPathsMailboxSealed - the one choke point every mail opcode
 *     passes); the guild-money veto is a core overlay patch too (a
 *     script-zeroed amount refuses the withdraw - without it, a zeroed
 *     guild-funded REPAIR would repair with nobody paying).
 *
 *   THE PILGRIM'S WAY - no mounts, no flight paths, no hearthstone, no
 *     self travel magic until level 80; foot, ship and zeppelin only.
 *     Gossip-less flight masters are routed through gossip by a wl-custom
 *     SQL (npcflag |= GOSSIP) so the taxi OPTION veto can fire for them.
 *
 *   THE SLOW BURN - all experience halved until level 80, no way out.
 *
 * Ends-at-80 Paths flip to COMPLETED on the level-80 ding (or login
 * catch-up) - fulfilled, announced, gates lifted.
 *
 * Per-path toggles: WowLegends.Paths.<Path>.Enabled. Disabling a Path
 * stops new oaths AND lifts all of that Path's gates for already-sworn
 * characters - the record is kept (progress/forsake stay visible at the
 * Herald) and re-arms if re-enabled. Pause, never erase.
 *
 * Storage: characters.wowlegends_paths (guid, path, status, progress,
 * sworn_time), self-creating. DB writes are async fire-and-forget.
 *
 * KILL CREDIT is a UnitScript::OnUnitDeath handler, NOT the PlayerScript
 * kill hooks: OnPlayerCreatureKill misses pet/totem killing blows entirely
 * (Unit.cpp fires OnPlayerCreatureKilledByPet instead, and guardians fire
 * neither) and misses SCRIPTED deaths - Illidan dies via Unit::Kill(nullptr)
 * from his own script, which would make the Road uncompletable. OnUnitDeath
 * fires unconditionally for every death; crediting derives from the loot
 * TAP (GetLootRecipientGroup/GetLootRecipient), which is set by the first
 * player damage and survives a null killer.
 *
 * THREADING: OnUnitDeath / OnPlayerGiveXP / OnPlayerCanGiveLevel run on
 * MAP-UPDATE worker threads; gossip/commands/login run on the world thread.
 * All state lives behind g_pathsMtx and every operation is self-contained
 * under the lock (no pointers into the map escape it). ALL player whispers
 * and realm announces produced from map context are queued and delivered by
 * WorldScript::OnUpdate on the world thread (the wowlegends_autosummon
 * handoff pattern) - map threads never touch another player's session.
 *
 * The Herald's gossip lives in wowlegends_hardcore.cpp (one CreatureScript
 * per NPC); it calls the two Wl:: externs below to add/route Path items.
 * Gossip action ids: 200-249 swear, 250-299 forsake, 300-349 progress -
 * the registry is therefore capped at 50 Paths (guarded at startup).
 *
 * Config: WowLegends.Paths.* (Enabled default 1 - the feature is pure
 * opt-in; Enabled=0 lifts all gates AND pauses boss credit).
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Player.h"
#include "Creature.h"
#include "GameObject.h"
#include "Group.h"
#include "Guild.h"
#include "Bag.h"
#include "DBCStores.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "ScriptedGossip.h"
#include "GossipDef.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellAuraDefines.h"
#include "WorldSession.h"
#include "World.h"
#include "GameTime.h"
#include "DatabaseEnv.h"
#include "Configuration/Config.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    bool CfgEnabled()
    { return sConfigMgr->GetOption<bool>("WowLegends.Paths.Enabled", true); }
    uint32 CfgMaxLevelToSwear()
    { return sConfigMgr->GetOption<uint32>("WowLegends.Paths.MaxLevelToSwear", 4); }
    bool CfgAnnounce()
    { return sConfigMgr->GetOption<bool>("WowLegends.Paths.Announce", true); }
    bool CfgAllowAbandon()
    { return sConfigMgr->GetOption<bool>("WowLegends.Paths.AllowAbandon", true); }
    bool CfgTrophies()
    { return sConfigMgr->GetOption<bool>("WowLegends.Paths.Trophies.Enabled", true); }
    bool CfgHeraldHonors()
    { return sConfigMgr->GetOption<bool>("WowLegends.Paths.HeraldHonors.Enabled", true); }

    bool WlIsRealPlayer(Player* p)
    {
        return p && p->GetSession() && !p->GetSession()->IsBot();
    }

    enum PathStatus : uint8
    {
        PATH_ACTIVE    = 0,
        PATH_COMPLETED = 1,
        PATH_FORSAKEN  = 2,
    };

    /* ------------------------------------------------------------------ */
    /*  The Long Road - stage data                                         */
    /* ------------------------------------------------------------------ */
    char const* const PATH_LONG_ROAD = "long_road";

    struct RoadBoss
    {
        uint32 entry;
        uint32 bit;
        char const* name;
    };
    struct RoadStage
    {
        uint8 cap;                    // XP locks at this level until cleared
        char const* title;            // for progress lines
        char const* completeLine;     // realm announce on clearing the stage
        std::vector<RoadBoss> bosses;
    };
    // Bits are FIXED forever (they persist in the DB) - only append. The
    // registry itself is capped at 50 Paths by the gossip action-id bands.
    std::vector<RoadStage> const& RoadStages()
    {
        static std::vector<RoadStage> const stages =
        {
            { 60, "the old world",
              "has broken the old world's lords - the Dark Portal calls!",
              { { 11502, 0, "Ragnaros" },
                { 10184, 1, "Onyxia" },
                { 11583, 2, "Nefarian" },
                { 15727, 3, "C'Thun" } } },
            { 70, "Outland",
              "has cast down the Betrayer - Northrend awaits!",
              { { 22917, 4, "Illidan Stormrage" } } },
        };
        return stages;
    }

    uint32 StageMask(RoadStage const& s)
    {
        uint32 m = 0;
        for (RoadBoss const& b : s.bosses)
            m |= (1u << b.bit);
        return m;
    }

    // XP cap for a given progress mask: the first incomplete stage's cap,
    // or 0 when the whole road has been walked (no cap).
    uint8 RoadCap(uint32 progress)
    {
        for (RoadStage const& s : RoadStages())
            if ((progress & StageMask(s)) != StageMask(s))
                return s.cap;
        return 0;
    }

    std::string RoadProgressLine(uint32 progress)
    {
        std::string out;
        for (RoadStage const& s : RoadStages())
        {
            uint32 have = 0;
            std::string missing;
            for (RoadBoss const& b : s.bosses)
            {
                if (progress & (1u << b.bit))
                    ++have;
                else
                    missing += (missing.empty() ? "" : ", ") + std::string(b.name);
            }
            out += Acore::StringFormat("{} {}/{}", s.title, have, uint32(s.bosses.size()));
            if (!missing.empty())
                out += " (awaits: " + missing + ")";
            out += "; ";
        }
        if (RoadCap(progress) == 0)
            out += "the road is WALKED.";
        return out;
    }

    /* ------------------------------------------------------------------ */
    /*  Path registry (generic; HARD CAP 50 - see header)                  */
    /* ------------------------------------------------------------------ */
    char const* const PATH_IRON_OATH = "iron_oath";
    char const* const PATH_PILGRIM   = "pilgrims_way";
    char const* const PATH_SLOW_BURN = "slow_burn";

    // Forward decls for per-path swear rules (defined after the helpers).
    std::string LongRoadSwearCheck(Player* p);
    std::string IronOathSwearCheck(Player* p);

    struct PathDef
    {
        char const* key;
        char const* name;
        char const* offer;      // gossip line when eligible
        char const* creed;      // the confirm-box text (the oath)
        char const* enableKey;  // per-path conf toggle
        uint32 maxSwearLevel;   // 0 = use the global CfgMaxLevelToSwear()
        bool completesAt80;     // reaching 80 fulfills the Path (gates lift)
        std::string (*extraSwearCheck)(Player*);   // nullptr = none
        // keepsake granted on fulfillment (0 = none). NOT a 900k custom id:
        // the 3.3.5 client draws bag icons only from its LOCAL Item.dbc, so
        // the trophies hijack unobtainable Deprecated entries the client
        // already knows (audit + rationale in the wl-custom SQL).
        uint32 trophyItem;
    };

    // progress bit 31 = "trophy granted" (set on successful DELIVERY, not
    // at fulfillment - a disconnect in between must not eat the keepsake).
    // Road boss bits only ever append upward from 0; collision needs a
    // 32-boss road.
    uint32 const PROGRESS_TROPHY_GRANTED = 1u << 31;
    std::vector<PathDef> const& Registry()
    {
        static std::vector<PathDef> const paths =
        {
            { PATH_LONG_ROAD, "긴 여정",
              "긴 여정을 맹세합니다. 역사를 건너뛰지 않겠습니다.",
              "긴 여정은 영구 적용됩니다. 라그나로스, 오닉시아, 네파리안, 쑨을 처치할 때까지 "
              "60레벨에서 경험치가 잠기며, 일리단을 처치할 때까지 70레벨에서 다시 잠깁니다. "
              "포기하면 영원히 다시 선택할 수 없습니다. 맹세하시겠습니까?",
              "WowLegends.Paths.LongRoad.Enabled", 0, false,
              &LongRoadSwearCheck, 1335 },     // Chronicle of the Long Road
            { PATH_IRON_OATH, "강철의 맹세",
              "강철의 맹세를 서약합니다. 모든 장비는 제 힘으로 얻겠습니다.",
              "강철의 맹세는 80레벨까지 영구 적용됩니다. 경매장, 거래, 우편, 길드 은행을 "
              "사용할 수 없으며 모든 장비를 직접 얻거나 제작해야 합니다. 1레벨에 시작 장비만 "
              "지닌 상태에서 서약해야 합니다. 맹세하시겠습니까?",
              "WowLegends.Paths.IronOath.Enabled", 1, true,
              &IronOathSwearCheck, 734 },      // Seal of the Iron Oath
            { PATH_PILGRIM, "순례자의 길",
              "순례자의 길을 맹세합니다. 모든 길을 제 두 발로 걷겠습니다.",
              "순례자의 길은 80레벨까지 영구 적용됩니다. 탈것, 비행 경로, 귀환석을 사용할 수 "
              "없으며 도보, 배, 비행선만 이용할 수 있습니다. 맹세하시겠습니까?",
              "WowLegends.Paths.PilgrimsWay.Enabled", 1, true,
              nullptr, 1663 },                 // The Pilgrim's Worn Map
            { PATH_SLOW_BURN, "느린 불꽃",
              "느린 불꽃을 맹세합니다. 두 배로 긴 여정을 택하겠습니다.",
              "느린 불꽃은 80레벨까지 영구 적용됩니다. 획득 경험치가 절반으로 줄며 다시 높일 "
              "수 없습니다. 긴 길만이 유일한 길입니다. 맹세하시겠습니까?",
              "WowLegends.Paths.SlowBurn.Enabled", 1, true,
              nullptr, 1638 },                 // Candle of the Slow Burn
        };
        return paths;
    }

    bool PathEnabled(PathDef const& def)
    {
        return sConfigMgr->GetOption<bool>(def.enableKey, true);
    }
    PathDef const* FindPath(std::string const& key)
    {
        for (PathDef const& p : Registry())
            if (key == p.key)
                return &p;
        return nullptr;
    }

    /* ------------------------------------------------------------------ */
    /*  State (guarded by g_pathsMtx - see THREADING in the header)        */
    /* ------------------------------------------------------------------ */
    struct PathState
    {
        uint8 status = PATH_ACTIVE;
        uint32 progress = 0;
    };
    std::mutex g_pathsMtx;
    // guid low -> (path key -> state); loaded at login, evicted at logout.
    std::unordered_map<uint32, std::unordered_map<std::string, PathState>> g_paths;

    // Whispers + realm announces queued from ANY thread, delivered on the
    // world thread by WorldScript::OnUpdate. targetGuidLow 0 = realm-wide.
    struct PendingMsg
    {
        uint32 targetGuidLow;
        std::string text;
    };
    std::mutex g_msgMtx;
    std::vector<PendingMsg> g_msgPending;

    void QueueWhisper(uint32 guidLow, std::string text)
    {
        std::lock_guard<std::mutex> lk(g_msgMtx);
        g_msgPending.push_back({ guidLow, std::move(text) });
    }

    void QueueAnnounce(std::string msg)
    {
        if (!CfgAnnounce())
            return;
        std::lock_guard<std::mutex> lk(g_msgMtx);
        g_msgPending.push_back({ 0, std::move(msg) });
    }

    // Trophy grants queued from ANY thread, delivered on the world thread
    // by WorldScript::OnUpdate. The drain dedupes via PROGRESS_TROPHY_
    // GRANTED (a record can be queued twice: the level-80 ding AND the
    // login catch-up), so queueing is fire-and-forget.
    struct PendingTrophy
    {
        uint32 guidLow;
        PathDef const* def;   // points into the static registry
    };
    std::mutex g_trophyMtx;
    std::vector<PendingTrophy> g_trophyPending;

    void QueueTrophy(uint32 guidLow, PathDef const* def)
    {
        if (!def || !def->trophyItem || !CfgTrophies())
            return;
        std::lock_guard<std::mutex> lk(g_trophyMtx);
        g_trophyPending.push_back({ guidLow, def });
    }

    void SavePath(uint32 guidLow, std::string const& key, PathState const& st)
    {
        // async fire-and-forget; the executor is thread-safe. `key` only
        // ever comes from the compile-time registry, never from players.
        CharacterDatabase.Execute(
            "INSERT INTO wowlegends_paths (guid,path,status,progress,sworn_time) "
            "VALUES ({},'{}',{},{},UNIX_TIMESTAMP()) "
            "ON DUPLICATE KEY UPDATE status={}, progress={}",
            guidLow, key, uint32(st.status), st.progress,
            uint32(st.status), st.progress);
    }

    // World thread (login). Sync single-player PK fetch - the same
    // deliberate trade wowlegends_hardcore makes; the cache is guaranteed
    // populated before any map-thread hook can fire for this player.
    void LoadPaths(uint32 guidLow)
    {
        QueryResult r = CharacterDatabase.Query(
            "SELECT path, status, progress FROM wowlegends_paths WHERE guid={}",
            guidLow);
        std::lock_guard<std::mutex> lk(g_pathsMtx);
        auto& mine = g_paths[guidLow];
        mine.clear();
        if (!r)
            return;
        do
        {
            Field* f = r->Fetch();
            PathState st;
            st.status = f[1].Get<uint8>();
            st.progress = f[2].Get<uint32>();
            mine[f[0].Get<std::string>()] = st;
        } while (r->NextRow());
    }

    // Copy-out accessor: returns true + fills `out` if the player has this
    // path in ANY status. No pointers into the map ever leave the lock.
    bool GetPath(uint32 guidLow, char const* key, PathState& out)
    {
        std::lock_guard<std::mutex> lk(g_pathsMtx);
        auto it = g_paths.find(guidLow);
        if (it == g_paths.end())
            return false;
        auto jt = it->second.find(key);
        if (jt == it->second.end())
            return false;
        out = jt->second;
        return true;
    }

    bool HasActivePath(uint32 guidLow, char const* key)
    {
        PathState st;
        return GetPath(guidLow, key, st) && st.status == PATH_ACTIVE;
    }

    // Is this Path's enforcement switched on? (registry keys only)
    bool PathLive(char const* key)
    {
        PathDef const* def = FindPath(key);
        return def && PathEnabled(*def);
    }

    // The ONE check every enforcement hook uses: sworn AND the Path's
    // toggle is on. Disabling a Path lifts its gates for the already-sworn
    // exactly like the global Enabled=0 (pause, never erase). Cache lookup
    // first - the common no-path player never pays the config lookup.
    bool PathGateLive(uint32 guidLow, char const* key)
    {
        return HasActivePath(guidLow, key) && PathLive(key);
    }

    // ALL guids with an ACTIVE Iron Oath row - ONLINE OR NOT (guarded by
    // g_pathsMtx). The mail receiver check needs this: the login-scoped
    // g_paths cache would wave parcels through to any OFFLINE sworn
    // character. Loaded at startup, maintained at every state change.
    std::unordered_set<uint32> g_ironSworn;

    bool IronSwornLive(uint32 guidLow)
    {
        if (!PathLive(PATH_IRON_OATH))
            return false;
        std::lock_guard<std::mutex> lk(g_pathsMtx);
        return g_ironSworn.count(guidLow) > 0;
    }

    // Effective Long Road XP cap for a player (0 = no gate). Map threads.
    uint8 EffectiveRoadCap(uint32 guidLow)
    {
        if (!PathLive(PATH_LONG_ROAD))
            return 0;
        PathState st;
        if (!GetPath(guidLow, PATH_LONG_ROAD, st) || st.status != PATH_ACTIVE)
            return 0;
        return RoadCap(st.progress);
    }

    // Fast path for the per-cast Pilgrim check: skip the mutex entirely
    // while no sworn pilgrim is online (500 casting bots = real contention
    // otherwise). Recounted under the lock on every state change (rare).
    std::atomic<uint32> g_pilgrimsOnline{0};

    void RecountPilgrims()
    {
        uint32 n = 0;
        std::lock_guard<std::mutex> lk(g_pathsMtx);
        for (auto const& [guid, paths] : g_paths)
        {
            auto it = paths.find(PATH_PILGRIM);
            if (it != paths.end() && it->second.status == PATH_ACTIVE)
                ++n;
        }
        g_pilgrimsOnline = n;
    }

    /* ------------------------------------------------------------------ */
    /*  Per-path swear rules                                               */
    /* ------------------------------------------------------------------ */
    // The Long Road refuses oaths past its first gate, whatever the
    // MaxLevelToSwear knob says - an already-past-the-wall oath would be
    // an instant, unexplained total XP freeze.
    std::string LongRoadSwearCheck(Player* p)
    {
        if (p->GetLevel() > RoadStages().front().cap)
            return "긴 여정은 구세계의 여정을 마치기 전(60레벨 이전)에 맹세해야 합니다.";
        return "";
    }

    // The Iron Oath is sworn in your starting clothes: level 1, carrying
    // NOTHING beyond the class/race starting kit (+ hearthstone) - no
    // pre-stocked bags, no banked head start, no borrowed gear.
    std::string IronOathSwearCheck(Player* p)
    {
        // allowed set: CharStartOutfit.dbc + playercreateinfo_item + 6948
        std::vector<uint32> allowed = { 6948 };
        if (CharStartOutfitEntry const* outfit = GetCharStartOutfitEntry(
                p->getRace(), p->getClass(), p->getGender()))
            for (uint32 j = 0; j < MAX_OUTFIT_ITEMS; ++j)
                if (outfit->ItemId[j] > 0)
                    allowed.push_back(uint32(outfit->ItemId[j]));
        if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(
                p->getRace(), p->getClass()))
            for (PlayerCreateInfoItem const& it : info->item)
                allowed.push_back(it.item_id);

        auto isAllowed = [&allowed](Item* item)
        {
            return std::find(allowed.begin(), allowed.end(),
                item->GetEntry()) != allowed.end();
        };
        // name the offender - a level-1 with one quest reward in the bag
        // deserves to know WHAT to drop, not just "no"
        auto refuse = [](Item* item)
        {
            return Acore::StringFormat(
                "강철의 맹세는 시작 장비만 지닌 상태에서 서약해야 합니다. 시작할 때 없던 아이템({})을 소지하고 있습니다.",
                item->GetTemplate() ? item->GetTemplate()->Name1 : "알 수 없는 아이템");
        };

        // a funded head start hides in the purse too (small slack for the
        // copper a level-1 picks up on the way to the Herald)
        if (p->GetMoney()
            > sWorld->getIntConfig(CONFIG_START_PLAYER_MONEY) + 100)
            return "강철의 맹세는 시작할 때 받은 금액을 초과한 돈이 없는 상태에서 서약해야 합니다.";

        // equipped + backpack + keyring/tokens + bank main slots
        for (uint8 i = EQUIPMENT_SLOT_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                if (!isAllowed(item))
                    return refuse(item);
        for (uint8 i = KEYRING_SLOT_START; i < CURRENCYTOKEN_SLOT_END; ++i)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                if (!isAllowed(item))
                    return refuse(item);
        for (uint8 i = BANK_SLOT_ITEM_START; i < BANK_SLOT_ITEM_END; ++i)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                if (!isAllowed(item))
                    return refuse(item);
        // any equipped or bank BAG at all = not a fresh start
        for (uint8 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; ++i)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                return refuse(item);
        for (uint8 i = BANK_SLOT_BAG_START; i < BANK_SLOT_BAG_END; ++i)
            if (Item* item = p->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
                return refuse(item);
        // sold-but-reclaimable items hide in buyback - invisible to
        // GetItemByPos (its slot guard stops at the bank slots)
        for (uint32 i = BUYBACK_SLOT_START; i < BUYBACK_SLOT_END; ++i)
            if (Item* item = p->GetItemFromBuyBackSlot(i))
                return refuse(item);
        return "";
    }

    /* ------------------------------------------------------------------ */
    /*  Swearing / renouncing (world thread: gossip + commands)            */
    /* ------------------------------------------------------------------ */
    // "" = eligible; otherwise the reason shown at the Herald.
    std::string SwearIneligibleReason(Player* p, PathDef const& def)
    {
        bool const korean = p->GetSession()->GetSessionDbLocaleIndex() == LOCALE_koKR;
        if (!CfgEnabled() || !PathEnabled(def))
            return korean ? "이 서버에서는 도전의 길을 선택할 수 없습니다." : "The Paths lie dormant on this realm.";
        PathState st;
        if (GetPath(p->GetGUID().GetCounter(), def.key, st))
        {
            switch (st.status)
            {
                case PATH_ACTIVE:    return korean ? "이미 이 도전을 진행 중입니다." : "You have already sworn this Path.";
                case PATH_COMPLETED: return korean ? "이미 이 도전을 완료했습니다." : "You have already walked this Path to its end.";
                default:             return korean ? "포기한 도전은 다시 선택할 수 없습니다." : "You forsook this Path. It does not forgive.";
            }
        }
        uint32 maxLevel = def.maxSwearLevel ? def.maxSwearLevel
                                            : CfgMaxLevelToSwear();
        if (p->GetLevel() > maxLevel)
            return Acore::StringFormat(
                korean ? "이 도전은 {}레벨이 되기 전에 선택해야 합니다. 현재 레벨에서는 시작할 수 없습니다."
                       : "This Path must be sworn before level {} - your journey is already under way.",
                maxLevel + 1);
        if (def.extraSwearCheck)
            return def.extraSwearCheck(p);
        return "";
    }

    void DoSwear(Player* p, PathDef const& def)
    {
        uint32 guidLow = p->GetGUID().GetCounter();
        PathState st;
        {
            std::lock_guard<std::mutex> lk(g_pathsMtx);
            g_paths[guidLow][def.key] = st;
            if (std::string_view(def.key) == PATH_IRON_OATH)
                g_ironSworn.insert(guidLow);
        }
        RecountPilgrims();
        SavePath(guidLow, def.key, st);
        ChatHandler(p->GetSession()).PSendSysMessage(
            "|cffFFD700{}을(를) 맹세했습니다.|r 전령과 서버가 이 서약을 기억합니다.",
            def.name);
        QueueAnnounce(Acore::StringFormat(
            "|cffFFD700[전설의 길]|r {} 님이 {}을(를) 맹세했습니다!", p->GetName(), def.name));
    }

    void DoForsake(Player* p, PathDef const& def)
    {
        uint32 guidLow = p->GetGUID().GetCounter();
        PathState st;
        {
            std::lock_guard<std::mutex> lk(g_pathsMtx);
            auto it = g_paths.find(guidLow);
            if (it == g_paths.end())
                return;
            auto jt = it->second.find(def.key);
            if (jt == it->second.end() || jt->second.status != PATH_ACTIVE)
                return;
            jt->second.status = PATH_FORSAKEN;
            st = jt->second;
            if (std::string_view(def.key) == PATH_IRON_OATH)
                g_ironSworn.erase(guidLow);
        }
        RecountPilgrims();
        SavePath(guidLow, def.key, st);
        ChatHandler(p->GetSession()).PSendSysMessage(
            "|cffff4020{}을(를) 포기했습니다.|r 제한은 해제되지만 이 명예는 영원히 잃게 됩니다.",
            def.name);
        QueueAnnounce(Acore::StringFormat(
            "|cffff4020[전설의 길]|r {} 님이 {}을(를) 포기했습니다.", p->GetName(), def.name));
    }

    /* ------------------------------------------------------------------ */
    /*  The Long Road - credit & completion (MAP THREADS)                  */
    /*  All messaging goes through the world-thread queue; the only reads  */
    /*  of the credited player here are guid + name (immutable online).    */
    /* ------------------------------------------------------------------ */
    void CreditRoadBoss(uint32 guidLow, std::string const& playerName,
        RoadStage const& stage, RoadBoss const& boss, bool testQuiet = false)
    {
        PathState st;
        bool stageDone = false;
        bool walked = false;
        {
            std::lock_guard<std::mutex> lk(g_pathsMtx);
            auto it = g_paths.find(guidLow);
            if (it == g_paths.end())
                return;
            auto jt = it->second.find(PATH_LONG_ROAD);
            if (jt == it->second.end() || jt->second.status != PATH_ACTIVE
                || (jt->second.progress & (1u << boss.bit)))
                return;
            jt->second.progress |= (1u << boss.bit);
            stageDone = (jt->second.progress & StageMask(stage)) == StageMask(stage);
            walked = stageDone && RoadCap(jt->second.progress) == 0;
            if (walked)
                jt->second.status = PATH_COMPLETED;
            st = jt->second;
        }

        QueueWhisper(guidLow, Acore::StringFormat(
            "|cffFFD700[The Long Road]|r {} has fallen{}. {}",
            boss.name, testQuiet ? " (test credit)" : "",
            RoadProgressLine(st.progress)));

        if (!testQuiet)
        {
            if (stageDone)
                QueueAnnounce(Acore::StringFormat(
                    "|cffFFD700[Paths of Legends]|r {} {}", playerName, stage.completeLine));
            if (walked)
                QueueAnnounce(Acore::StringFormat(
                    "|cffFFD700[Paths of Legends]|r {} has walked The Long Road "
                    "to its end. Kneel, travelers - a legend passes.",
                    playerName));
        }
        SavePath(guidLow, PATH_LONG_ROAD, st);
        if (walked)   // GM test credits grant too - that verifies the flow
            QueueTrophy(guidLow, FindPath(PATH_LONG_ROAD));
    }

    // Reaching level 80 FULFILLS every active ends-at-80 Path: gates lift,
    // status flips to COMPLETED, the realm hears it. Called from the
    // level-change hook (MAP threads) and the login catch-up (world thread)
    // - messaging therefore always goes through the world-thread queue.
    void CompleteAt80(uint32 guidLow, std::string const& playerName)
    {
        // the SAVED state carries the record's real progress - the trophy
        // bit lives there too, so never persist a fresh PathState here
        std::vector<std::pair<PathDef const*, PathState>> done;
        {
            std::lock_guard<std::mutex> lk(g_pathsMtx);
            auto it = g_paths.find(guidLow);
            if (it == g_paths.end())
                return;
            for (PathDef const& def : Registry())
            {
                if (!def.completesAt80)
                    continue;
                auto jt = it->second.find(def.key);
                if (jt == it->second.end()
                    || jt->second.status != PATH_ACTIVE)
                    continue;
                jt->second.status = PATH_COMPLETED;
                if (std::string_view(def.key) == PATH_IRON_OATH)
                    g_ironSworn.erase(guidLow);
                done.push_back({ &def, jt->second });
            }
        }
        if (done.empty())
            return;
        RecountPilgrims();
        for (auto const& [def, st] : done)
        {
            SavePath(guidLow, def->key, st);
            QueueWhisper(guidLow, Acore::StringFormat(
                "|cffFFD700{} is FULFILLED.|r Its oath releases you - you "
                "carried it all the way.", def->name));
            QueueAnnounce(Acore::StringFormat(
                "|cffFFD700[Paths of Legends]|r {} has fulfilled {} - "
                "sworn at the beginning, carried to level 80!",
                playerName, def->name));
            QueueTrophy(guidLow, def);
        }
    }

    // Is this group member "at the kill" for crediting purposes? Same Map
    // INSTANCE as the boss (pointer identity - also guarantees the member
    // is owned by the currently running map worker), or dead-and-released
    // with their corpse still on the boss's map.
    bool MemberPresentAtKill(Player* member, Creature* boss)
    {
        if (member->IsInMap(boss))
            return true;
        if (!member->IsAlive()
            && member->GetCorpseLocation().GetMapId() == boss->GetMapId())
            return true;
        return false;
    }
}

/* -------------------------------------------------------------------------- */
/*  Externs for the Herald's gossip (the CreatureScript lives in               */
/*  wowlegends_hardcore.cpp - one script per NPC). Action ids 200-399 ours.    */
/* -------------------------------------------------------------------------- */
void WlPathsAddGossip(Player* player)
{
    if (!CfgEnabled() || !WlIsRealPlayer(player))
        return;

    auto const& reg = Registry();
    // ineligible-reason lines collect here and go out AFTER the loop,
    // deduped: four identical "sworn before level N" lines are noise,
    // one is information. reason -> the path indices sharing it.
    std::vector<std::pair<std::string, std::vector<uint32>>> reasons;
    for (uint32 i = 0; i < reg.size() && i < 50; ++i)   // 50 = action-id band
    {
        PathDef const& def = reg[i];
        bool active = HasActivePath(player->GetGUID().GetCounter(), def.key);
        // disabled Paths are not offered - but the already-sworn keep
        // their progress and forsake lines (gates lifted, record held)
        if (!PathEnabled(def) && !active)
            continue;
        std::string reason = SwearIneligibleReason(player, def);
        if (reason.empty())
        {
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, def.offer,
                GOSSIP_SENDER_MAIN, 200 + i, def.creed, 0, false);
            continue;
        }
        if (active)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                std::string(def.name) + " - 진행 상황 보기",
                GOSSIP_SENDER_MAIN, 300 + i);
            if (CfgAllowAbandon())
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                    std::string(def.name) + " 포기...",
                    GOSSIP_SENDER_MAIN, 250 + i,
                    "도전을 포기하면 영원히 다시 서약할 수 없고 명예도 잃게 됩니다. 정말 포기하시겠습니까?",
                    0, false);
            continue;
        }
        // ineligible with no active path: SAY WHY (a silent Herald looks
        // broken - the level-60 refusal especially)
        auto it = std::find_if(reasons.begin(), reasons.end(),
            [&reason](std::pair<std::string, std::vector<uint32>> const& r)
            { return r.first == reason; });
        if (it == reasons.end())
            reasons.push_back({ reason, { i } });
        else
            it->second.push_back(i);
    }
    // Non-actionable lines; clicking lands in the harmless progress branch.
    for (auto const& [reason, idxs] : reasons)
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            idxs.size() == 1
                ? (player->GetSession()->GetSessionDbLocaleIndex() == LOCALE_koKR && reg[idxs[0]].key == PATH_LONG_ROAD
                    ? std::string("긴 여정") : std::string(reg[idxs[0]].name)) + ": " + reason
                : reason,
            GOSSIP_SENDER_MAIN, 300 + idxs[0]);
}

bool WlPathsGossipSelect(Player* player, uint32 action)
{
    if (action < 200 || action > 399)
        return false;

    auto const& reg = Registry();
    uint32 idx = action % 50;   // 200+i / 250+i / 300+i
    if (idx >= reg.size())
        return true;
    PathDef const& def = reg[idx];

    if (action >= 300)          // progress
    {
        PathState st;
        if (GetPath(player->GetGUID().GetCounter(), def.key, st)
            && st.status == PATH_ACTIVE)
        {
            std::string line = std::string_view(def.key) == PATH_LONG_ROAD
                ? RoadProgressLine(st.progress)
                : std::string(def.completesAt80
                    ? "서약이 유지되고 있습니다. 80레벨이 되면 제한이 해제됩니다."
                    : "서약이 유지되고 있습니다.");
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffFFD700[{}]|r {}", def.name, line);
        }
        return true;
    }
    if (action >= 250)          // forsake (confirmed via gossip box)
    {
        if (CfgAllowAbandon())
            DoForsake(player, def);
        return true;
    }
    // swear (confirmed via gossip box) - re-check, the world may have moved
    std::string reason = SwearIneligibleReason(player, def);
    if (reason.empty())
        DoSwear(player, def);
    else
        ChatHandler(player->GetSession()).PSendSysMessage("{}", reason);
    return true;
}

/* -------------------------------------------------------------------------- */
/*  Extern for WorldSession::CanOpenMailBox (core overlay patch) - the ONE     */
/*  choke point every mail opcode passes (list/take/delete/return/send).       */
/*  Blocking only the mailbox GOSSIP is cosmetic: the client opens the mail    */
/*  frame itself and drives CMSG_GET_MAIL_LIST, gated only by this check.      */
/*  World thread (opcode handler). GMs keep their mailbox for support work.    */
/* -------------------------------------------------------------------------- */
bool WlPathsMailboxSealed(Player* player)
{
    if (!CfgEnabled() || !WlIsRealPlayer(player))
        return false;
    if (player->GetSession()->GetSecurity() > SEC_PLAYER)
        return false;
    if (!PathGateLive(player->GetGUID().GetCounter(), PATH_IRON_OATH))
        return false;
    ChatHandler(player->GetSession()).SendSysMessage(
        "|cffff4020[강철의 맹세]|r 우편을 사용할 수 없습니다. 80레벨에 해제됩니다.");
    return true;
}

/* -------------------------------------------------------------------------- */
/*  Extern for the Herald's gossip (wowlegends_hardcore.cpp): facing someone   */
/*  with fulfilled Paths, the Herald bows and SAYS their name and deeds to     */
/*  everyone in earshot. CMSG_GOSSIP_HELLO is PROCESS_INPLACE - this can run   */
/*  on a MAP worker thread, so the throttle map is mutex-guarded; Say itself   */
/*  executes in the creature's own map context and touches no other session.   */
/* -------------------------------------------------------------------------- */
void WlPathsHeraldHonor(Player* player, Creature* herald)
{
    if (!herald || !CfgEnabled() || !CfgHeraldHonors()
        || !WlIsRealPlayer(player))
        return;
    if (player->IsGameMaster())
        return;   // never out an invisible GM by name
    uint32 guidLow = player->GetGUID().GetCounter();
    std::vector<char const*> deeds;
    {
        std::lock_guard<std::mutex> lk(g_pathsMtx);
        auto it = g_paths.find(guidLow);
        if (it == g_paths.end())
            return;
        for (PathDef const& def : Registry())
        {
            auto jt = it->second.find(def.key);
            if (jt != it->second.end()
                && jt->second.status == PATH_COMPLETED)
                deeds.push_back(def.name);
        }
    }
    if (deeds.empty())
        return;
    // one honor a minute per character - the Herald is not a soundboard
    static std::mutex honorMtx;
    static std::unordered_map<uint32, uint32> lastHonor;
    uint32 now = uint32(GameTime::GetGameTime().count());
    {
        std::lock_guard<std::mutex> lk(honorMtx);
        if (lastHonor.size() > 256)   // hygiene: shed expired entries
            std::erase_if(lastHonor, [now](std::pair<uint32 const, uint32> const& e)
                { return now - e.second >= 60; });
        uint32& last = lastHonor[guidLow];
        if (now - last < 60)
            return;
        last = now;
    }
    std::string list = deeds[0];
    for (std::size_t i = 1; i < deeds.size(); ++i)
        list += (i + 1 == deeds.size() ? std::string(" and ")
                                       : std::string(", ")) + deeds[i];
    herald->HandleEmoteCommand(EMOTE_ONESHOT_BOW);
    herald->Say(Acore::StringFormat(
        "Kneel, travelers! Before you stands {}, who carried {} to {}!",
        player->GetName(), list,
        deeds.size() > 1 ? "their ends" : "its end"), LANG_UNIVERSAL);
}

/* -------------------------------------------------------------------------- */
/*  UnitScript: boss-death credit via the loot tap (map threads).              */
/*  OnUnitDeath fires for EVERY death - normal kills, pet/totem killing        */
/*  blows, and scripted deaths like Illidan's Unit::Kill(nullptr, me).         */
/* -------------------------------------------------------------------------- */
class WowLegendsPathsUnit : public UnitScript
{
public:
    WowLegendsPathsUnit() : UnitScript("WowLegendsPathsUnit", true,
        { UNITHOOK_ON_UNIT_DEATH }) { }

    void OnUnitDeath(Unit* unit, Unit* killer) override
    {
        if (!CfgEnabled() || !unit)
            return;
        Creature* c = unit->ToCreature();
        if (!c)
            return;

        RoadStage const* stage = nullptr;
        RoadBoss const* boss = nullptr;
        for (RoadStage const& s : RoadStages())
            for (RoadBoss const& b : s.bosses)
                if (b.entry == c->GetEntry())
                {
                    stage = &s;
                    boss = &b;
                }
        if (!boss)
            return;

        // credit from the TAP, not the killing blow (which may be a pet, a
        // totem, or nullptr for a scripted death)
        if (Group* g = c->GetLootRecipientGroup())
        {
            for (GroupReference* itr = g->GetFirstMember(); itr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (member && WlIsRealPlayer(member)
                    && MemberPresentAtKill(member, c))
                    CreditRoadBoss(member->GetGUID().GetCounter(),
                        member->GetName(), *stage, *boss);
            }
            return;
        }
        if (Player* tap = c->GetLootRecipient())
        {
            if (WlIsRealPlayer(tap))
                CreditRoadBoss(tap->GetGUID().GetCounter(), tap->GetName(),
                    *stage, *boss);
            return;
        }
        // no tap at all (edge: full evade-wipe then scripted death): the
        // killing blow is the last resort
        if (killer)
            if (Player* kp = killer->ToPlayer())
                if (WlIsRealPlayer(kp))
                    CreditRoadBoss(kp->GetGUID().GetCounter(), kp->GetName(),
                        *stage, *boss);
    }
};

/* -------------------------------------------------------------------------- */
/*  PlayerScript: XP gate + level-grant veto + cache lifecycle                 */
/* -------------------------------------------------------------------------- */
class WowLegendsPathsPlayer : public PlayerScript
{
public:
    WowLegendsPathsPlayer() : PlayerScript("WowLegendsPathsPlayer",
        { PLAYERHOOK_ON_LOGIN,
          PLAYERHOOK_ON_LOGOUT,
          PLAYERHOOK_ON_DELETE,
          PLAYERHOOK_ON_GIVE_EXP,
          PLAYERHOOK_ON_CAN_GIVE_LEVEL,
          PLAYERHOOK_ON_LEVEL_CHANGED,
          PLAYERHOOK_CAN_INIT_TRADE,
          PLAYERHOOK_CAN_SEND_MAIL,
          PLAYERHOOK_CAN_PLACE_AUCTION_BID,
          PLAYERHOOK_CAN_USE_ITEM }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!WlIsRealPlayer(player))
            return;
        LoadPaths(player->GetGUID().GetCounter());
        RecountPilgrims();
        if (!CfgEnabled())
            return;   // dormant realm: no progress line, all gates lifted
        // catch-up: offline level-sets (.character level) bypass the ding
        // hook - fulfill ends-at-80 Paths at login instead
        if (player->GetLevel() >= 80)
            CompleteAt80(player->GetGUID().GetCounter(), player->GetName());
        PathState st;
        if (PathLive(PATH_LONG_ROAD)
            && GetPath(player->GetGUID().GetCounter(), PATH_LONG_ROAD, st)
            && st.status == PATH_ACTIVE)
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffFFD700[기나긴 여정]|r {}", RoadProgressLine(st.progress));
        // the ends-at-80 oaths share ONE line - a triple-sworn character
        // should not scroll through three reminders every login
        std::string oaths;
        uint32 nOaths = 0;
        for (PathDef const& def : Registry())
        {
            if (!def.completesAt80 || !PathEnabled(def))
                continue;
            PathState ps;
            if (GetPath(player->GetGUID().GetCounter(), def.key, ps)
                && ps.status == PATH_ACTIVE)
            {
                oaths += (oaths.empty() ? "" : ", ") + std::string(def.name);
                ++nOaths;
            }
        }
        if (nOaths)
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cffFFD700[전설의 길]|r 맹세 {}개 유지 중: {} - 80레벨에 제한이 해제됩니다.",
                nOaths, oaths);
        // trophy safety net: completions from before the trophies existed,
        // and deliveries lost to a disconnect, catch up here (the drain
        // skips anything already marked granted)
        for (PathDef const& def : Registry())
        {
            PathState ps;
            if (GetPath(player->GetGUID().GetCounter(), def.key, ps)
                && ps.status == PATH_COMPLETED
                && !(ps.progress & PROGRESS_TROPHY_GRANTED))
                QueueTrophy(player->GetGUID().GetCounter(), &def);
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        {
            std::lock_guard<std::mutex> lk(g_pathsMtx);
            g_paths.erase(player->GetGUID().GetCounter());
        }
        RecountPilgrims();
    }

    void OnPlayerDelete(ObjectGuid guid, uint32 /*accountId*/) override
    {
        {
            std::lock_guard<std::mutex> lk(g_pathsMtx);
            g_paths.erase(guid.GetCounter());
            g_ironSworn.erase(guid.GetCounter());
        }
        RecountPilgrims();
        CharacterDatabase.Execute(
            "DELETE FROM wowlegends_paths WHERE guid={}", guid.GetCounter());
    }

    // Reaching 80 fulfills every ends-at-80 Path (MAP threads - messaging
    // via the world-thread queue inside CompleteAt80).
    void OnPlayerLevelChanged(Player* player, uint8 oldlevel) override
    {
        if (!CfgEnabled() || !WlIsRealPlayer(player))
            return;
        if (oldlevel < 80 && player->GetLevel() >= 80)
            CompleteAt80(player->GetGUID().GetCounter(), player->GetName());
    }

    // --- Iron Oath: no trades (either direction) -------------------------
    bool OnPlayerCanInitTrade(Player* player, Player* target) override
    {
        if (!CfgEnabled())
            return true;
        for (Player* side : { player, target })
            if (side && WlIsRealPlayer(side)
                && PathGateLive(side->GetGUID().GetCounter(), PATH_IRON_OATH))
            {
                ChatHandler(side->GetSession()).SendSysMessage(
                    "|cffff4020[강철의 맹세]|r 거래할 수 없습니다. 물품은 직접 구해야 합니다.");
                Player* other = side == player ? target : player;
                if (other && other->GetSession())
                    ChatHandler(other->GetSession()).PSendSysMessage(
                        "{}은 강철의 맹세를 선택하여 거래할 수 없습니다.",
                        side->GetName());
                return false;
            }
        return true;
    }

    // --- Iron Oath: no player mail (sender or receiver sworn) ------------
    bool OnPlayerCanSendMail(Player* player, ObjectGuid receiverGuid,
        ObjectGuid /*mailbox*/, std::string& /*subject*/, std::string& /*body*/,
        uint32 /*money*/, uint32 /*COD*/, Item* /*item*/) override
    {
        if (!CfgEnabled())
            return true;
        // GMs may still deliver support mail
        if (player->GetSession()
            && player->GetSession()->GetSecurity() > SEC_PLAYER)
            return true;
        if (PathGateLive(player->GetGUID().GetCounter(), PATH_IRON_OATH))
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffff4020[강철의 맹세]|r 우편을 사용할 수 없습니다. 물품은 직접 구해야 합니다.");
            return false;
        }
        // the RECEIVER may be offline - the persistent set, not the cache
        if (IronSwornLive(receiverGuid.GetCounter()))
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "상대가 강철의 맹세를 선택하여 우편을 받을 수 없습니다.");
            return false;
        }
        return true;
    }

    // --- Iron Oath: no auction bids (belt; the AH window is also blocked) -
    bool OnPlayerCanPlaceAuctionBid(Player* player, AuctionEntry* /*auction*/) override
    {
        if (!CfgEnabled())
            return true;
        if (PathGateLive(player->GetGUID().GetCounter(), PATH_IRON_OATH))
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffff4020[강철의 맹세]|r 경매장을 사용할 수 없습니다.");
            return false;
        }
        return true;
    }

    // --- Pilgrim's Way: no hearthstone ------------------------------------
    bool OnPlayerCanUseItem(Player* player, ItemTemplate const* proto,
        InventoryResult& result) override
    {
        // hot path (every CanUseItem call): the one-cycle id compare
        // rejects ~all calls before the config-string lookup runs
        if (!proto || proto->ItemId != 6948 || !CfgEnabled())
            return true;
        if (PathGateLive(player->GetGUID().GetCounter(), PATH_PILGRIM))
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffff4020[순례자의 길]|r 귀환석을 사용할 수 없습니다. 직접 걸어서 이동해야 합니다.");
            result = EQUIP_ERR_CANT_DO_RIGHT_NOW;
            return false;
        }
        return true;
    }

    // The gate itself (map threads). GiveXP's level loop can cross several
    // levels in one huge grant, so clamp the amount to an exact ding at the
    // cap rather than only zeroing once already there.
    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/,
        uint8 /*xpSource*/) override
    {
        // bots can never appear in g_paths (LoadPaths is IsRealPlayer-
        // gated) - turn them away BEFORE the mutex, this is the 500-bot
        // XP stream. One lock serves both gates below.
        if (!CfgEnabled() || !amount || !WlIsRealPlayer(player))
            return;
        bool slowBurn = false;
        uint8 cap = 0;
        {
            std::lock_guard<std::mutex> lk(g_pathsMtx);
            auto it = g_paths.find(player->GetGUID().GetCounter());
            if (it == g_paths.end())
                return;
            auto sb = it->second.find(PATH_SLOW_BURN);
            slowBurn = sb != it->second.end()
                && sb->second.status == PATH_ACTIVE;
            auto lr = it->second.find(PATH_LONG_ROAD);
            if (lr != it->second.end()
                && lr->second.status == PATH_ACTIVE)
                cap = RoadCap(lr->second.progress);
        }
        // The Slow Burn: all experience halved, forever. Multiplicative
        // with any server/.xp rate, so the oath can never be escaped.
        if (slowBurn && PathLive(PATH_SLOW_BURN))
            amount = std::max<uint32>(1, amount / 2);
        if (!cap || !PathLive(PATH_LONG_ROAD))
            return;
        uint8 level = player->GetLevel();
        if (level >= cap)
        {
            amount = 0;
            return;
        }
        uint32 remain = player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP)
            - player->GetUInt32Value(PLAYER_XP);
        for (uint8 l = level + 1; l < cap; ++l)
            remain += sObjectMgr->GetXPForLevel(l);
        amount = std::min(amount, remain);
    }

    // Direct level grants (Recruit-a-Friend, GM .levelup) bypass GiveXP -
    // veto them past the cap. GMs testing gates use .path credit instead.
    bool OnPlayerCanGiveLevel(Player* player, uint8 newLevel) override
    {
        if (!CfgEnabled())
            return true;
        uint8 cap = EffectiveRoadCap(player->GetGUID().GetCounter());
        if (cap && newLevel > cap)
        {
            QueueWhisper(player->GetGUID().GetCounter(), Acore::StringFormat(
                "|cffFFD700[The Long Road]|r holds you at level {} until its "
                "gate falls.", uint32(cap)));
            return false;
        }
        return true;
    }
};

/* -------------------------------------------------------------------------- */
/*  Iron Oath: the auction house window never opens                            */
/* -------------------------------------------------------------------------- */
class WowLegendsPathsMisc : public MiscScript
{
public:
    WowLegendsPathsMisc() : MiscScript("WowLegendsPathsMisc",
        { MISCHOOK_CAN_SEND_AUCTIONHELLO }) { }

    bool CanSendAuctionHello(WorldSession const* session, ObjectGuid /*guid*/,
        Creature* /*creature*/) override
    {
        if (!CfgEnabled() || !session)
            return true;
        Player* player = session->GetPlayer();
        if (!player || !WlIsRealPlayer(player))
            return true;
        if (PathGateLive(player->GetGUID().GetCounter(), PATH_IRON_OATH))
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffff4020[강철의 맹세]|r 경매장을 사용할 수 없습니다. 물품은 직접 구해야 합니다.");
            return false;
        }
        return true;
    }
};

/* -------------------------------------------------------------------------- */
/*  Iron Oath: the guild bank neither opens nor moves money                    */
/* -------------------------------------------------------------------------- */
class WowLegendsPathsGuild : public GuildScript
{
public:
    WowLegendsPathsGuild() : GuildScript("WowLegendsPathsGuild",
        { GUILDHOOK_CAN_GUILD_SEND_BANK_LIST,
          GUILDHOOK_ON_MEMBER_DEPOSIT_MONEY,
          GUILDHOOK_ON_MEMBER_WITDRAW_MONEY }) { }

    bool CanGuildSendBankList(Guild const* /*guild*/, WorldSession* session,
        uint8 /*tabId*/, bool /*sendAllSlots*/) override
    {
        // nullptr session = guild-wide broadcast update; vetoing THAT would
        // hide bank changes from the whole guild
        if (!CfgEnabled() || !session || !session->GetPlayer())
            return true;
        Player* player = session->GetPlayer();
        if (!WlIsRealPlayer(player))
            return true;
        if (PathGateLive(player->GetGUID().GetCounter(), PATH_IRON_OATH))
        {
            ChatHandler(session).SendSysMessage(
                "|cffff4020[강철의 맹세]|r 길드 은행을 사용할 수 없습니다.");
            return false;
        }
        return true;
    }

    // Zeroing the amount IS the veto: the core overlay patch in Guild.cpp
    // refuses a script-zeroed deposit/withdraw outright. Without it, a
    // zeroed guild-funded REPAIR would still repair with nobody paying -
    // these hooks run after validation and cannot veto on their own.
    void OnMemberDepositMoney(Guild* /*guild*/, Player* player,
        uint32& amount) override
    {
        if (CfgEnabled() && WlIsRealPlayer(player)
            && PathGateLive(player->GetGUID().GetCounter(), PATH_IRON_OATH))
        {
            amount = 0;
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffff4020[강철의 맹세]|r 길드 은행을 사용할 수 없습니다.");
        }
    }

    void OnMemberWitdrawMoney(Guild* /*guild*/, Player* player,
        uint32& amount, bool isRepair) override
    {
        if (CfgEnabled() && WlIsRealPlayer(player)
            && PathGateLive(player->GetGUID().GetCounter(), PATH_IRON_OATH))
        {
            amount = 0;
            ChatHandler(player->GetSession()).SendSysMessage(
                isRepair
                    ? "|cffff4020[The Iron Oath]|r forbids guild-paid "
                      "repairs. What you carry, you mend yourself."
                    : "|cffff4020[강철의 맹세]|r 길드 은행을 사용할 수 없습니다.");
        }
    }
};

/* -------------------------------------------------------------------------- */
/*  Iron Oath: mailbox gossip refuses the forsworn - FLAVOR only. The client   */
/*  opens the mail frame itself and drives CMSG_GET_MAIL_LIST regardless of    */
/*  gossip; the REAL seal is WorldSession::CanOpenMailBox (core overlay        */
/*  patch -> WlPathsMailboxSealed above), which every mail opcode passes.      */
/*  INVERTED semantics: returning true BLOCKS the gossip.                      */
/*  NOTE (accepted cost): registering an AllGameObjectScript newly activates   */
/*  per-GO per-update virtual dispatch (the list was empty before). If map     */
/*  profiles ever show it, port the enabledHooks mask to All*Script in core.   */
/* -------------------------------------------------------------------------- */
class WowLegendsPathsAllGo : public AllGameObjectScript
{
public:
    WowLegendsPathsAllGo() : AllGameObjectScript("WowLegendsPathsAllGo") { }

    bool CanGameObjectGossipHello(Player* player, GameObject* go) override
    {
        if (!CfgEnabled() || !player || !go || !WlIsRealPlayer(player))
            return false;
        if (go->GetGoType() == GAMEOBJECT_TYPE_MAILBOX
            && PathGateLive(player->GetGUID().GetCounter(), PATH_IRON_OATH))
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffff4020[강철의 맹세]|r 우편함을 열 수 없습니다.");
            return true;    // INVERTED: true = block
        }
        return false;
    }
};

/* -------------------------------------------------------------------------- */
/*  Iron Oath: mailbox NPCs / Pilgrim's Way: flight masters. Same inverted     */
/*  gossip semantics as above. The hello veto covers only PURE flight          */
/*  masters (nothing but taxi + gossip) so multi-role FMs - 73 of 163 are      */
/*  also questgivers - keep their quests/wares for pilgrims; for those, the    */
/*  taxi OPTION is vetoed in CanCreatureGossipSelect. The 64 gossip-less FM    */
/*  templates are routed through gossip by a wl-custom SQL (npcflag |=         */
/*  GOSSIP), else the client goes straight to CMSG_TAXIQUERYAVAILABLENODES     */
/*  and no hook ever fires.                                                    */
/* -------------------------------------------------------------------------- */
class WowLegendsPathsAllCreature : public AllCreatureScript
{
public:
    WowLegendsPathsAllCreature()
        : AllCreatureScript("WowLegendsPathsAllCreature") { }

    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        if (!CfgEnabled() || !player || !creature || !WlIsRealPlayer(player))
            return false;
        uint32 guidLow = player->GetGUID().GetCounter();
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_MAILBOX)
            && PathGateLive(guidLow, PATH_IRON_OATH))
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffff4020[강철의 맹세]|r 우편을 사용할 수 없습니다.");
            return true;    // INVERTED: true = block
        }
        if (creature->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER)
            && !(creature->GetNpcFlags()
                & ~uint32(UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_GOSSIP))
            && PathGateLive(guidLow, PATH_PILGRIM))
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffff4020[순례자의 길]|r 비행 경로를 사용할 수 없습니다. 직접 걸어서 이동해야 합니다.");
            return true;    // INVERTED: true = block
        }
        return false;
    }

    // The taxi OPTION on any flight master's menu. `action` is the selected
    // item's stored action - MiscHandler passes GetGossipOptionAction(),
    // which is GOSSIP_OPTION_TAXIVENDOR for the built-in "flight" option.
    // Returning true = handled = Player::OnGossipSelect (SendTaxiMenu)
    // never runs.
    bool CanCreatureGossipSelect(Player* player, Creature* creature,
        uint32 /*sender*/, uint32 action) override
    {
        if (!CfgEnabled() || !player || !creature || !WlIsRealPlayer(player))
            return false;
        if (action == GOSSIP_OPTION_TAXIVENDOR
            && creature->HasNpcFlag(UNIT_NPC_FLAG_FLIGHTMASTER)
            && PathGateLive(player->GetGUID().GetCounter(), PATH_PILGRIM))
        {
            ChatHandler(player->GetSession()).SendSysMessage(
                "|cffff4020[순례자의 길]|r 비행 경로를 사용할 수 없습니다. 직접 걸어서 이동해야 합니다.");
            return true;    // INVERTED: true = block
        }
        return false;
    }
};

/* -------------------------------------------------------------------------- */
/*  Pilgrim's Way: no mounts, no taxi spells, no hearthstone-likes, and no     */
/*  SELF travel magic (mage Teleport, Death Gate, Astral Recall - explicit     */
/*  ids; a blanket TELEPORT_UNITS gate would break scripted/quest self-        */
/*  teleports). Magic performed by OTHERS - portals, summons - is not          */
/*  policed: an oath, not a cage. Boats/zeppelins are transports - no spell,   */
/*  untouched. Druid travel forms are shapeshifts, not mounts - allowed on     */
/*  purpose (class flavor). One choke point covers spellbook AND item mounts   */
/*  (both funnel through CheckCast).                                           */
/*  HOT PATH: fires for every cast by every unit on map threads - check the    */
/*  atomic counter and cheap predicates BEFORE touching the mutex.             */
/* -------------------------------------------------------------------------- */
namespace
{
    bool IsSelfTravelMagic(SpellInfo const* info)
    {
        switch (info->Id)
        {
            case 556:     // Astral Recall - a hearthstone in disguise
            case 50977:   // Death Gate
            case 3561:    // Teleport: Stormwind
            case 3562:    // Teleport: Ironforge
            case 3563:    // Teleport: Undercity
            case 3565:    // Teleport: Darnassus
            case 3566:    // Teleport: Thunder Bluff
            case 3567:    // Teleport: Orgrimmar
            case 32271:   // Teleport: Exodar
            case 32272:   // Teleport: Silvermoon
            case 33690:   // Teleport: Shattrath (Alliance)
            case 35715:   // Teleport: Shattrath (Horde)
            case 49358:   // Teleport: Stonard
            case 49359:   // Teleport: Theramore
            case 53140:   // Teleport: Dalaran
                return true;
            default:
                return info->HasAura(SPELL_AURA_MOUNTED)
                    || info->HasEffect(SPELL_EFFECT_SEND_TAXI);
        }
    }
}

class WowLegendsPathsAllSpell : public AllSpellScript
{
public:
    WowLegendsPathsAllSpell() : AllSpellScript("WowLegendsPathsAllSpell",
        { ALLSPELLHOOK_ON_SPELL_CHECK_CAST }) { }

    void OnSpellCheckCast(Spell* spell, bool strict,
        SpellCastResult& res) override
    {
        if (!g_pilgrimsOnline.load(std::memory_order_relaxed))
            return;
        if (!spell || res != SPELL_CAST_OK)
            return;
        Unit* caster = spell->GetCaster();
        if (!caster || !caster->IsPlayer())
            return;
        SpellInfo const* info = spell->GetSpellInfo();
        if (!info || !IsSelfTravelMagic(info))
            return;
        Player* player = caster->ToPlayer();
        if (!WlIsRealPlayer(player) || !CfgEnabled())
            return;
        if (!PathGateLive(player->GetGUID().GetCounter(), PATH_PILGRIM))
            return;
        res = SPELL_FAILED_NOT_HERE;
        if (strict)   // CheckCast runs twice; whisper only once (map thread)
            QueueWhisper(player->GetGUID().GetCounter(),
                "|cffff4020[The Pilgrim's Way]|r forbids mounts, the "
                "skyways and your own travel magic. Every mile on your "
                "own two feet.");
    }
};

/* -------------------------------------------------------------------------- */
/*  .path - status for players; credit/reset shortcuts for GM testing          */
/* -------------------------------------------------------------------------- */
class WowLegendsPathsCommand : public CommandScript
{
public:
    WowLegendsPathsCommand() : CommandScript("WowLegendsPathsCommand") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable sub =
        {
            { "",       HandleStatus,     SEC_PLAYER,     Console::No },
            { "swear",  HandleTestSwear,  SEC_GAMEMASTER, Console::No },
            { "credit", HandleTestCredit, SEC_GAMEMASTER, Console::No },
            { "reset",  HandleTestReset,  SEC_GAMEMASTER, Console::No },
        };
        static ChatCommandTable base =
        {
            { "path", sub },
        };
        return base;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return true;
        if (!CfgEnabled())
        {
            handler->SendSysMessage(
                "이 서버에서는 전설의 길이 비활성화되어 관련 제한이 적용되지 않습니다.");
            return true;
        }

        std::unordered_map<std::string, PathState> mine;
        {
            std::lock_guard<std::mutex> lk(g_pathsMtx);
            auto it = g_paths.find(p->GetGUID().GetCounter());
            if (it != g_paths.end())
                mine = it->second;
        }
        if (mine.empty())
        {
            handler->SendSysMessage(
                "선택한 길이 없습니다. 여정을 시작하기 전에 몰락자의 전령을 찾아가세요.");
            return true;
        }
        for (auto const& [key, st] : mine)
        {
            PathDef const* def = FindPath(key);
            char const* label = st.status == PATH_ACTIVE ? "sworn"
                : st.status == PATH_COMPLETED
                    ? (def && def->completesAt80 ? "FULFILLED" : "WALKED")
                : "forsaken";
            std::string tail = key == PATH_LONG_ROAD
                ? ": " + RoadProgressLine(st.progress) : "";
            handler->PSendSysMessage("|cffFFD700{}|r [{}]{}",
                def ? def->name : key.c_str(), label, tail);
        }
        return true;
    }

    // GM testing: force-swear a Path on the GM's own character, bypassing
    // ALL swear guards (level, gear) and WITHOUT the realm announce -
    // pairs with credit/reset so gates can be exercised on any character.
    // Usage: .path swear [long|iron|pilgrim|slow]  (default: long)
    static bool HandleTestSwear(ChatHandler* handler,
        Optional<std::string> which)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return true;
        std::string w = which.value_or("long");
        char const* key = PATH_LONG_ROAD;
        if (w == "iron")
            key = PATH_IRON_OATH;
        else if (w == "pilgrim")
            key = PATH_PILGRIM;
        else if (w == "slow")
            key = PATH_SLOW_BURN;
        else if (w != "long")
        {
            handler->SendSysMessage(
                ".path swear [long|iron|pilgrim|slow]");
            return true;
        }
        uint32 guidLow = p->GetGUID().GetCounter();
        PathState existing;
        if (GetPath(guidLow, key, existing))
        {
            handler->SendSysMessage(
                "이미 해당 길의 기록이 있습니다. 먼저 .path reset을 사용하세요.");
            return true;
        }
        PathState st;
        {
            std::lock_guard<std::mutex> lk(g_pathsMtx);
            g_paths[guidLow][key] = st;
            if (std::string_view(key) == PATH_IRON_OATH)
                g_ironSworn.insert(guidLow);
        }
        RecountPilgrims();
        SavePath(guidLow, key, st);
        PathDef const* def = FindPath(key);
        handler->PSendSysMessage(
            "{}을 선택했습니다 (시험용·공지 없음). 제한이 적용됩니다.",
            def ? def->name : key);
        return true;
    }

    // GM testing: credits the GM's own next missing Long Road boss QUIETLY
    // (whisper only, no realm announce), so gates can be verified without
    // clearing Molten Core - or spamming the realm.
    static bool HandleTestCredit(ChatHandler* handler)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return true;
        PathState st;
        if (!GetPath(p->GetGUID().GetCounter(), PATH_LONG_ROAD, st)
            || st.status != PATH_ACTIVE)
        {
            handler->SendSysMessage("기나긴 여정을 선택하지 않았습니다.");
            return true;
        }
        for (RoadStage const& s : RoadStages())
            for (RoadBoss const& b : s.bosses)
                if (!(st.progress & (1u << b.bit)))
                {
                    CreditRoadBoss(p->GetGUID().GetCounter(), p->GetName(),
                        s, b, /*testQuiet=*/true);
                    return true;
                }
        handler->SendSysMessage("이미 여정을 마쳤습니다.");
        return true;
    }

    // GM testing: wipe ALL of the GM's own Path records (cache + DB) so
    // the swear/credit cycle can be repeated.
    static bool HandleTestReset(ChatHandler* handler)
    {
        Player* p = handler->GetPlayer();
        if (!p)
            return true;
        uint32 guidLow = p->GetGUID().GetCounter();
        {
            std::lock_guard<std::mutex> lk(g_pathsMtx);
            g_paths.erase(guidLow);
            g_paths[guidLow];   // keep the loaded-but-empty login state
            g_ironSworn.erase(guidLow);
        }
        RecountPilgrims();
        CharacterDatabase.Execute(
            "DELETE FROM wowlegends_paths WHERE guid={}", guidLow);
        handler->SendSysMessage("모든 전설의 길 기록을 초기화했습니다 (시험용).");
        return true;
    }
};

/* -------------------------------------------------------------------------- */
/*  WorldScript: self-creating storage + world-thread message drain            */
/* -------------------------------------------------------------------------- */
class WowLegendsPathsWorld : public WorldScript
{
public:
    WowLegendsPathsWorld() : WorldScript("WowLegendsPathsWorld",
        { WORLDHOOK_ON_STARTUP, WORLDHOOK_ON_UPDATE }) { }

    void OnStartup() override
    {
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS wowlegends_paths ("
            "guid INT UNSIGNED NOT NULL,"
            "path VARCHAR(24) NOT NULL,"
            "status TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            "progress INT UNSIGNED NOT NULL DEFAULT 0,"
            "sworn_time INT UNSIGNED NOT NULL DEFAULT 0,"
            "PRIMARY KEY (guid, path)) ENGINE=InnoDB");
        if (Registry().size() > 50)
            LOG_ERROR("server", "[paths] registry exceeds 50 entries; the "
                "gossip action-id bands (200-399) cannot route the rest");
        // the OFFLINE-inclusive Iron Oath set (mail receiver checks) -
        // after the table create above, before any player can log in
        uint32 nIron = 0;
        if (QueryResult r = CharacterDatabase.Query(
            "SELECT guid FROM wowlegends_paths WHERE path='{}' AND status={}",
            PATH_IRON_OATH, uint32(PATH_ACTIVE)))
        {
            std::lock_guard<std::mutex> lk(g_pathsMtx);
            do
            {
                g_ironSworn.insert((*r)[0].Get<uint32>());
                ++nIron;
            } while (r->NextRow());
        }
        // flag binary-without-SQL installs before the first grant is due
        for (PathDef const& def : Registry())
            if (def.trophyItem && !sObjectMgr->GetItemTemplate(def.trophyItem))
                LOG_ERROR("server", "[paths] trophy item {} ({}) missing "
                    "from item_template - apply db/wl-custom/"
                    "2026-07-09_paths_trophies.sql", def.trophyItem, def.name);
        LOG_INFO("server", "[paths] wowlegends_paths ready (Paths of "
            "Legends; {} iron-sworn)", nIron);
    }

    // Whispers, announces and trophy grants queued from map threads go out
    // here. The two drains are independent - an empty message queue must
    // not starve pending trophies.
    void OnUpdate(uint32 /*diff*/) override
    {
        DrainTrophies();
        std::vector<PendingMsg> out;
        {
            std::lock_guard<std::mutex> lk(g_msgMtx);
            if (g_msgPending.empty())
                return;
            out.swap(g_msgPending);
        }
        for (PendingMsg const& m : out)
        {
            if (!m.targetGuidLow)
            {
                ChatHandler(nullptr).SendGlobalSysMessage(m.text.c_str());
                continue;
            }
            if (Player* p = ObjectAccessor::FindConnectedPlayer(
                    ObjectGuid(HighGuid::Player, m.targetGuidLow)))
                if (p->GetSession())
                    ChatHandler(p->GetSession()).PSendSysMessage("{}", m.text);
        }
    }

private:
    // World thread. Marks PROGRESS_TROPHY_GRANTED only once the keepsake
    // is actually in the player's hands (or their mailbox) and re-checks
    // the mark first - duplicate queue entries are expected (ding + login
    // catch-up). A player who slipped offline is skipped; the login
    // safety-net re-queues them.
    void DrainTrophies()
    {
        std::vector<PendingTrophy> out;
        {
            std::lock_guard<std::mutex> lk(g_trophyMtx);
            if (g_trophyPending.empty())
                return;
            out.swap(g_trophyPending);
        }
        for (PendingTrophy const& t : out)
        {
            if (!CfgTrophies())
                continue;   // recoverable: bit stays unset, login re-queues
            // binary-without-SQL installs: defer rather than mark-and-lose
            ItemTemplate const* proto =
                sObjectMgr->GetItemTemplate(t.def->trophyItem);
            if (!proto)
            {
                LOG_ERROR("server", "[paths] trophy item {} has no "
                    "item_template row - apply db/wl-custom/"
                    "2026-07-09_paths_trophies.sql; grant deferred",
                    t.def->trophyItem);
                continue;
            }
            Player* p = ObjectAccessor::FindConnectedPlayer(
                ObjectGuid(HighGuid::Player, t.guidLow));
            if (!p || !p->GetSession())
                continue;
            PathState st;
            {
                std::lock_guard<std::mutex> lk(g_pathsMtx);
                auto it = g_paths.find(t.guidLow);
                if (it == g_paths.end())
                    continue;
                auto jt = it->second.find(t.def->key);
                if (jt == it->second.end()
                    || jt->second.status != PATH_COMPLETED
                    || (jt->second.progress & PROGRESS_TROPHY_GRANTED))
                    continue;
                jt->second.progress |= PROGRESS_TROPHY_GRANTED;
                st = jt->second;
            }
            // already owned (bank counts) = only the mark was lost: restore
            // it silently. Unique maxcount=1 would fail AddItem and the
            // mail fallback would DUPE otherwise.
            bool mailed = false;
            if (!p->HasItemCount(t.def->trophyItem, 1, true))
            {
                if (p->AddItem(t.def->trophyItem, 1))
                {
                    // persist the inventory BEFORE the mark: the async DB
                    // worker is FIFO, so item-durability precedes mark-
                    // durability - a crash between them self-heals at next
                    // login (the item is visible to the dupe guard above)
                    p->SaveToDB(false, false);
                }
                else if (IronSwornLive(t.guidLow))
                {
                    // bags full and their own mailbox still sealed (a Long
                    // Road trophy can land mid-Iron-Oath): mailing would
                    // strand the keepsake past the 30-day expiry. Roll the
                    // mark back; the login safety net retries.
                    std::lock_guard<std::mutex> lk(g_pathsMtx);
                    auto it = g_paths.find(t.guidLow);
                    if (it != g_paths.end())
                    {
                        auto jt = it->second.find(t.def->key);
                        if (jt != it->second.end())
                            jt->second.progress &= ~PROGRESS_TROPHY_GRANTED;
                    }
                    continue;
                }
                else
                {
                    // full bags -> the Postmaster. Accepted residual: a
                    // crash between the mail commit and the mark can
                    // re-grant at next login (HasItemCount cannot see
                    // mail) - a spare worthless unique the player deletes.
                    p->SendItemRetrievalMail(t.def->trophyItem, 1);
                    mailed = true;
                }
            }
            std::string link = Acore::StringFormat(
                "|cffa335ee|Hitem:{}:0:0:0:0:0:0:0:0:0|h[{}]|h|r",
                t.def->trophyItem, proto->Name1);
            ChatHandler(p->GetSession()).PSendSysMessage(mailed
                ? "|cffFFD700[Paths of Legends]|r The Herald's keepsake, "
                  "{}, awaits you with the Postmaster."
                : "|cffFFD700[Paths of Legends]|r The Herald's keepsake "
                  "is yours: {}. Carry it proudly.", link);
            SavePath(t.guidLow, t.def->key, st);
        }
    }
};

void AddWowLegendsPathsScripts()
{
    new WowLegendsPathsUnit();
    new WowLegendsPathsPlayer();
    new WowLegendsPathsMisc();
    new WowLegendsPathsGuild();
    new WowLegendsPathsAllGo();
    new WowLegendsPathsAllCreature();
    new WowLegendsPathsAllSpell();
    new WowLegendsPathsCommand();
    new WowLegendsPathsWorld();
}
