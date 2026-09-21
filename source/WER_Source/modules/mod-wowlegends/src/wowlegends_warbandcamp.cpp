/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - Warband Camp.
 *
 * Suggested by AeonFlux (#suggestions, "Warband & Ingame Shop" + "Player Housing
 * or Private Instance" - the same idea from two directions). Design lives in
 * WARBAND_CAMP_DESIGN.md.
 *
 * A camp is a patch of ground that belongs to your ACCOUNT, not to one
 * character. You claim it WHERE YOU ARE STANDING, anywhere in the world you
 * like, and you furnish it yourself out of ordinary WotLK scenery.
 *
 * ===========================================================================
 * READ THIS BEFORE CHANGING ANYTHING: THE FIRST VERSION WAS SCRAPPED
 * ===========================================================================
 * v1 put every camp on map 37 (Azshara Crater) - an unfinished vanilla zone
 * with no spawns, so plots could never collide with live content. It had a
 * five-point terrain validator, a 682-slot plot grid and a race-proof
 * allocator, and every one of those parts worked exactly as written.
 *
 * It was still completely wrong, and it took about fifteen seconds in-game to
 * see why: Azshara Crater is a cut developer zone. No world map, no flying,
 * untextured ground. Landing there does not read as "my camp", it reads as
 * having fallen out of the world. The verdict was blunt and correct - "it is a
 * zone a player should never be".
 *
 * 🛑 THE LESSON, because it is the expensive one: "technically valid ground"
 *    is not the same as "somewhere a player wants to stand". Every gate in v1
 *    measured the terrain. Not one of them asked whether the place was any
 *    good. If you are about to add a clever placement rule, ask the second
 *    question first.
 *
 * v1 also ruled out real zones on a premise that was simply FALSE: "phaseMask
 * is 32-bit, so a realm only has 31 phases and you cannot give everyone one".
 * The count is right and the conclusion is wrong. InSamePhase is a plain
 * bitmask test (Object.h:544) AND seeing anything additionally requires being
 * near it. Two camps sharing a phase bit are invisible to each other unless
 * they are also in the same place. 31 is therefore a cap on CO-LOCATED camps,
 * not on the realm - and camps scattered across Azeroth never come close.
 *
 * ⇒ So: the camp goes where you are standing, and gets a phase bit that no
 *   other camp within PHASE_REUSE_RADIUS is using.
 *
 * ===========================================================================
 * HOW THE PHASING ACTUALLY BEHAVES
 * ===========================================================================
 * Camp props are spawned with the camp's bit ALONE (never PHASEMASK_NORMAL),
 * so nobody sees them by default. A player standing within CAMP_RADIUS of a
 * camp centre is given (PHASEMASK_NORMAL | campBit):
 *
 *   - they keep seeing the entire normal world, so Ashenvale still looks like
 *     Ashenvale. Phasing them into the bit ALONE would delete every tree,
 *     building and NPC around them, which is the obvious implementation and
 *     is badly wrong.
 *   - they see that one camp's props.
 *   - everyone still shares PHASEMASK_NORMAL, so players remain visible to
 *     each other regardless of whose camp they are standing in.
 *
 * Walk in and the camp fades up around you; walk out and it fades away. No
 * invite list, no instance, no loading screen - a friend simply walks in.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Player.h"
#include "WorldSession.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Map.h"
#include "MapMgr.h"
#include "CharacterCache.h"     // name -> guid without touching SQL
#include "Object.h"             // PHASEMASK_NORMAL
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "DBCStores.h"
#include "GridTerrainData.h"    // MAX_HEIGHT / INVALID_HEIGHT
#include "Log.h"
// Only `.camp alts` needs these, and only to tell a gathered alt to hold its
// ground. wowlegends_aichat.cpp already links against them, so the dependency
// costs nothing new. ADDING a bot still goes through ParseCommands - see the
// comment on HandleCampAltsCommand for why that distinction matters.
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ChatCommandTable lives in Acore::ChatCommands; every WL command script pulls
// the namespace in this way (see wowlegends_companion.cpp:50). Without it the
// return type of GetCommands does not resolve and the class stays abstract.
using namespace Acore::ChatCommands;

namespace
{
    std::atomic<bool> g_enabled{false};

    // ---------------------------------------------------------------------
    // Tunables
    // ---------------------------------------------------------------------

    // How close you must be for the camp to exist around you. Also the size of
    // the bubble props have to live inside.
    constexpr float CAMP_RADIUS = 40.0f;

    // Props must sit inside the bubble, with a margin - a prop exactly on the
    // boundary would pop in and out as you shuffle around next to it.
    constexpr float CAMP_PROP_RADIUS = 32.0f;

    // Camps may not be planted closer together than this. Comfortably more
    // than two bubbles, so no player is ever inside two camps at once (which
    // would make "which camp am I in" a coin toss) and neighbours are not
    // pitching tents through each other.
    constexpr float CAMP_MIN_SEPARATION = 150.0f;

    // Two camps may reuse the same phase bit only if they are further apart
    // than this. It must exceed the largest possible view distance by a wide
    // margin: MAX_VISIBILITY_DISTANCE is 250 and the shipped
    // Visibility.Distance.Continents is 100, so 600 leaves room even if an
    // owner raises it to the cap (40 yard bubble + 250 sight = 290 < 600).
    constexpr float PHASE_REUSE_RADIUS = 600.0f;

    // Bit 0 (PHASEMASK_NORMAL) is the ordinary world, so camps get 1..31.
    constexpr uint8 CAMP_PHASE_BIT_MIN = 1;
    constexpr uint8 CAMP_PHASE_BIT_MAX = 31;

    // Prop cap is CONF-DRIVEN (0 = unlimited). Kneuma, 2026-08-09: "do not
    // limit props to 30, what if i want to place 30 flowers?" A cap still
    // exists as a default because every placement burns a per-map GUID that
    // is never returned, and an unbounded table grows the grid forever - but
    // the number is the owner's call, not ours.
    std::atomic<uint32> g_maxProps{200};
    constexpr uint32 CAMP_GO_COOLDOWN_SECONDS = 300;

    // A prop placement or removal spawns/destroys a real GameObject, and every
    // spawn burns a per-map GUID from a counter that is never reset (cap
    // 0x00FFFFFF; exhausting it calls World::StopNow). 3 seconds is invisible
    // to someone decorating and makes an automated place/remove loop take
    // roughly a year and a half to matter.
    constexpr uint32 CAMP_PROP_COOLDOWN_SECONDS = 3;

    // How often a player's surroundings are checked for a camp. 1s is well
    // under the time it takes to walk CAMP_RADIUS, so the transition always
    // feels immediate.
    constexpr uint32 CAMP_PROXIMITY_INTERVAL_MS = 1000;
    // Delay before the FIRST proximity check of a session. Must outlast the
    // client's login handshake (and the 2500ms origin-restore teleport below),
    // or the phase is applied to a client that is not ready to receive it.
    constexpr uint32 CAMP_LOGIN_PHASE_DELAY_MS  = 3000;

    // ---------------------------------------------------------------------
    // The prop catalogue
    // ---------------------------------------------------------------------
    // Stock 3.3.5a scenery, every entry GAMEOBJECT_TYPE_GENERIC (type 5).
    //
    // ⚠️ TYPE 5 IS NOT AN AESTHETIC CHOICE. Generic objects are completely
    // inert: no loot, no use, no interaction, so they never enter
    // GO_JUST_DEACTIVATED and can never be looted, opened, clicked or
    // consumed. Add a CHEST (type 3) or GOOBER (type 10) here and you have
    // handed players an interactive object with despawn semantics inside a
    // phase nobody is policing.
    //
    // Entries were read out of the live gameobject_template; displayIds are
    // re-verified against GameObjectDisplayInfo.dbc at startup and anything
    // that fails is dropped from the catalogue rather than spawned invisible.
    //
    // ⚠️ THAT CHECK IS NOT PARANOIA - IT HAS ALREADY EARNED ITS KEEP. The first
    // catalogue picked four entries by name from the world DB (19429 Hay Stack,
    // 19432 Scarecrow, 19454 Fence, 19522 Cauldron) whose displayId was simply
    // `entry - 10000` - junk data that no client model answers to. They would
    // have spawned real, solid, completely invisible objects that a player had
    // "placed" and could neither see nor find to remove. The startup pass
    // caught all four and logged them, which is how they came to be replaced.
    // Look for that giveaway pattern if you add anything here by hand.
    // `clearance` is how many yards IN FRONT of the player the thing is set
    // down.
    //
    // ⚠️ Placing at the player's own feet is the obvious implementation and it
    // traps them. A campfire or a crate is small enough to get away with it,
    // but the first wagon placed on the PTR swallowed the player whole - the
    // model is several yards long and they ended up standing inside it looking
    // at the underside of the boards. Anything with a body needs to be put
    // down at arm's length, and the bigger the model the further out it goes.
    struct PropDef
    {
        char const* key;
        uint32 entry;
        char const* label;
        float clearance;
    };

    PropDef const g_propCatalogue[] =
    {
        // shelter - big, and you walk into them
        { "tent",       184592, "천막",             6.0f },
        { "tent-a",     201868, "얼라이언스 천막",    7.0f },
        { "tent-h",     201886, "호드 천막",       7.0f },
        // fire and light - small, and you want them close
        { "campfire",   182059, "모닥불",         3.0f },
        { "bonfire",    180434, "화톳불",          3.5f },
        { "brazier",    180473, "화로",          2.5f },
        // ⚠️ 180765's displayId 6537 EXISTS in the DBC but renders NOTHING in
        // the 3.3.5 client (Kneuma placed it and saw nothing, 2026-08-09).
        // The DBC check catches missing entries, not empty models - only a
        // human eyeball catches these. 6038 is a lantern that renders.
        { "lantern",    179977, "등불",          2.0f },
        // furniture
        { "table",      181075, "탁자",            3.5f },
        { "chair",      193949, "의자",            2.5f },
        { "bench",      190694, "긴 의자",            3.0f },
        { "rug",        181077, "양탄자",              3.0f },
        { "bookshelf",  183268, "책꽂이",        3.0f },
        { "bookcase",   190693, "책장",         3.0f },
        // storage
        // 178646 is filed under "Alliance Supply Crate" but displayId 336 is a
        // plain wooden box with no markings, so it stays the neutral default.
        // 178442 is a visibly Horde-styled crate for anyone who wants one.
        // There is no Horde wagon in 3.3.5a at all - 188696 is the only cart
        // model, and it is unmarked, so it also stays neutral.
        { "crate",      178646, "보급품 상자",     2.5f },
        { "crate-h",    178442, "호드 상자",      2.5f },
        { "barrel",     180779, "통",           2.5f },
        { "keg",        180575, "맥주통",              2.5f },
        { "cauldron",   180414, "가마솥",         2.5f },
        { "cookpot",    184670, "요리 냄비",         2.5f },
        // yard
        { "wagon",      188696, "수레",            8.0f },
        { "haystack",   179968, "건초더미",         3.5f },
        { "haybale",    180700, "건초 묶음",         3.0f },
        { "woodpile",   190687, "장작더미",        3.5f },
        { "logpile",    194393, "통나무더미",         3.0f },
        { "fence",      180035, "울타리",            4.0f },
        { "rockwall",   211064, "돌담",   4.0f },
        { "pumpkin",    195164, "호박",          2.0f },
        // craft
        { "anvil",      201771, "모루",            3.0f },
        { "forge",      201772, "가열로",            3.5f },
        { "coals",      201773, "가열로 숯",      2.5f },
        { "weaponrack", 183269, "무기 거치대",      3.0f },
        // colours
        //
        // ⚠️ FACTION PARITY IS A REAL REQUIREMENT, NOT A NICETY. The first
        // catalogue shipped `tent-a` AND `tent-h` but only an Alliance banner,
        // which Kneuma spotted immediately. Horde players furnishing a camp
        // would have found the one flag on offer was the other side's.
        //
        // The Alliance banner it used (201869, displayId 8573) has no Horde
        // counterpart in its asset set at all, so it was swapped rather than
        // twinned: 192252 / 192254 are the SAME banner in the two factions'
        // colours - consecutive displayIds 5651 / 5652, identical size 1.0.
        // If you add any faction-flavoured prop here, add both halves or
        // neither.
        { "banner",     180773, "깃발",           2.5f },
        { "banner-a",   192252, "얼라이언스 깃발",  2.5f },
        { "banner-h",   192254, "호드 깃발",     2.5f },
        // more light
        { "torch",      180352, "횃불",            2.5f },
        { "candle",       1558, "양초",           2.0f },
        { "candelabra",   2697, "촛대",       2.5f },
        // more storage and food
        { "sack",       195197, "곡물 자루",       2.5f },
        { "basket",     195196, "바구니",           2.5f },
        { "corn",       195192, "옥수수 바구니",   2.0f },
        { "bucket",       2696, "양동이",           2.0f },
        { "bottle",       2687, "병",           2.0f },
        { "bread",      180051, "빵",            2.0f },
        { "food",        56903, "차려진 음식",   2.0f },
        { "chest",      185503, "상자",            3.0f },
        // atmosphere
        { "skull",        2371, "해골",            2.5f },
        // "bones" (193961 Frozen Bones, displayId 8512) REMOVED 2026-08-09:
        // it renders as a huge translucent blue ice sheet across the whole
        // camp - Kneuma hunted it down prop by prop. Ice-effect model, not
        // scenery. Do not re-add.
        { "totem",      187890, "토템",            2.0f },
        { "gong",       180386, "징",             3.0f },
        { "drum",       186865, "북",             2.5f },
        // "bell" REMOVED 2026-08-09: displayId 3972 renders nothing, and ALL
        // bell gameobjects share it - there is no visible bell to swap in.
        { "statue",     192948, "비취 조각상",      3.0f },
        { "grave",      211065, "무덤",            3.0f },
        { "cage",       181379, "우리",             3.0f },
        { "anchor",     177791, "닻",           2.5f },
        { "signpost",   180026, "이정표",         2.5f },
        { "scroll",     182005, "두루마리",           2.0f },
        { "shovel",     180651, "삽",           2.5f },
        // nature
        { "mushroom",   182073, "거대 버섯",   7.0f },
        { "flower",     181103, "꽃",          2.0f },
        { "bush",       181824, "덤불",             2.5f },
        // more craft
        { "alchemy",    187114, "연금술 탁자",    3.0f },
        { "fishing",    173086, "낚시 장비",     3.0f },
        // more shelter
        { "foodtent",   186681, "음식 천막",        7.0f },
        // structures - Kneuma's ask 2026-08-09: "can we add bigger props like
        // taverns or small houses?" Yes: buildings are ordinary type-5
        // doodads too. Big clearance so nobody spawns a tavern on their head;
        // placement from the camp edge falls back to at-your-feet as usual.
        // ⚠️ TAKE TWO on structures (Kneuma eyeballed take one, 2026-08-09):
        // the vanilla "Tavern"/"Inn"/"House" gameobjects are the city
        // DIRECTION SIGNBOARDS guards point people at, not buildings - they
        // rendered as a row of pub signs. Guard Tower's model is not even in
        // the client (error cube). Buildings that actually render are the
        // EVENT structures, because Blizzard spawns those as gameobjects at
        // real world events: Brewfest tents, the AQ war-effort pavilions,
        // the Karazhan opera cottage, the Winter Veil stable.
        { "cottage",    183493, "작은 집",         14.0f },
        { "beertent",   186682, "맥주 천막",       10.0f },
        { "pavilion",   188021, "대형 행사 천막",        10.0f },
        // 🛑 "lodge" (188267 Amberpine Lodge) REMOVED 2026-08-09, PRIME
        // SUSPECT in a dump-less worldserver death: placed twice at 00:29,
        // server died ~00:31 with NO crash dump - the stack-overflow
        // signature (recursive collision/LoS against a huge or degenerate
        // model bypasses the crash handler). It is the only real
        // world-building WMO that was in this list; the survivors are event
        // M2s Blizzard itself spawns as gameobjects. Do not re-add without
        // soak-testing it alone on the PTR with bots pathing around it.
        { "bigtent",    184593, "대형 천막",       8.0f },
        { "stable",     180719, "마구간",          10.0f },
        { "doghouse",   180033, "개집",         4.0f },
        { "outhouse",   180006, "야외 화장실",         5.0f },
    };

    // Catalogue entries that survived template + displayId validation.
    std::vector<PropDef> g_props;
    bool g_propsBuilt = false;

    void BuildPropCatalogue()
    {
        if (g_propsBuilt)
            return;
        g_propsBuilt = true;

        for (PropDef const& def : g_propCatalogue)
        {
            GameObjectTemplate const* tpl =
                sObjectMgr->GetGameObjectTemplate(def.entry);
            if (!tpl)
            {
                LOG_WARN("server", "[warbandcamp] prop '{}' entry {} has no "
                    "gameobject_template - dropped", def.key, def.entry);
                continue;
            }

            // The same guard .gobject add uses. A bad displayId spawns an
            // object that exists, blocks and cannot be seen.
            if (!tpl->displayId ||
                !sGameObjectDisplayInfoStore.LookupEntry(tpl->displayId))
            {
                LOG_WARN("server", "[warbandcamp] prop '{}' entry {} has an "
                    "invalid displayId {} - dropped", def.key, def.entry,
                    tpl->displayId);
                continue;
            }

            // Belt and braces against someone pasting an interactive entry
            // into the table above.
            if (tpl->type != GAMEOBJECT_TYPE_GENERIC)
            {
                LOG_WARN("server", "[warbandcamp] prop '{}' entry {} is type "
                    "{}, not GENERIC - dropped", def.key, def.entry, tpl->type);
                continue;
            }

            g_props.push_back(def);
        }

        LOG_INFO("server", "[warbandcamp] {} of {} props available",
            g_props.size(), std::size(g_propCatalogue));
    }

    PropDef const* FindProp(std::string const& key)
    {
        for (PropDef const& def : g_props)
            if (key == def.key)
                return &def;
        return nullptr;
    }

    // ---------------------------------------------------------------------
    // Camp state
    // ---------------------------------------------------------------------

    // 🛑 EVERY CONTAINER BELOW IS SHARED ACROSS MAP THREADS. GUARD IT.
    //
    // The proximity check lives in PlayerScript::OnPlayerUpdate, which is
    // called from Player::Update (PlayerUpdates.cpp:310) - and that runs
    // inside Map::Update, which MapUpdater fans out across MapUpdate.Threads
    // worker threads. The PTR runs 6. Two players on two different maps
    // therefore hit this code AT THE SAME TIME on different threads, and an
    // unsynchronised std::unordered_map insert from two threads is not a race
    // that produces a wrong answer, it is one that corrupts the container and
    // takes the worldserver down.
    //
    // Chat command handlers do NOT need to worry about the map threads
    // specifically - World::Update runs UpdateSessions() and sMapMgr->Update()
    // in sequence, never concurrently - but they share these containers with
    // the update hook, so they take the lock too.
    //
    // Contention is a non-issue: only real players ever reach it, once a
    // second each.
    std::mutex g_campMutex;

    struct Camp
    {
        uint32 accountId = 0;
        uint32 map = 0;
        float x = 0.0f, y = 0.0f, z = 0.0f, o = 0.0f;
        uint8 phaseBit = 0;
        uint32 zoneId = 0;
    };

    // Every camp on the realm, held in memory. The proximity check runs for
    // each real player every second and must never touch the database to do
    // it. Kept in sync by hand on claim and leave - the table is the durable
    // copy, this is the working one.
    std::vector<Camp> g_camps;

    // Live props, keyed by their row id in wowlegends_warband_camp_object.
    // A camp's props are created the first time somebody walks into it and
    // then simply left standing.
    //
    // ⚠️ NOT "spawn on enter, despawn on leave". That is the obvious design
    // and it is a slow way to kill the worldserver: each spawn takes a GUID
    // from a per-map counter with a hard cap of 0x00FFFFFF that is never
    // reset, and running out calls World::StopNow. Cycling a 30-prop camp on
    // every visit burns thousands of GUIDs a day for no benefit. Spawned once
    // per uptime, a realm would need decades.
    std::unordered_map<uint64, ObjectGuid> g_liveProps;

    struct PlayerCampState
    {
        uint32 timer = 0;           // ms until the next proximity check
        uint32 campAccount = 0;     // camp we are phased into, 0 = none
        bool ownsPhase = false;     // WE set this player's phase, so we may
                                    // put it back. Without this flag the
                                    // restore is a guess, and it would happily
                                    // stomp a phase a GM set by hand with
                                    // `.modify phase` one second later.
    };
    std::unordered_map<ObjectGuid, PlayerCampState> g_playerState;

    std::unordered_map<uint32, time_t> g_goCooldown;
    std::unordered_map<uint32, time_t> g_propCooldown;

    // ---------------------------------------------------------------------
    // Warband alts: camp-parked bots + the gathering queue
    // ---------------------------------------------------------------------

    constexpr uint32 CAMP_MAX_ALTS = 8;
    constexpr float CAMP_ALT_RING = 8.0f;

    std::atomic<bool> g_autoAlts{true};
    // How close a player must be before a camp phases in. Kneuma 2026-08-09:
    // make the 40 configurable, 0 = "show always". 0 still cannot beat the
    // CLIENT draw distance (~90-180yd typical) - what it really means is
    // "no server-side gate": bits for every camp in range, materialised as
    // soon as the grid can hold them. Clamped [20..250] because <20 flickers
    // on walk-in and >250 is beyond what any client renders anyway.
    std::atomic<float> g_viewDist{40.0f};
    // Gather only alts of the faction currently being played (default), or
    // the whole account. Horde alts strolling an Alliance camp read as a bug
    // to anyone watching, so same-faction is the shipped behaviour.
    std::atomic<bool> g_altsSameFactionOnly{true};

    // bot guidLow -> owning accountId. Read by mod-playerbots' idle stroll
    // (WlWarbandCampParked below) so parked alts amble around the CAMP with
    // no group, no master attention and no follow. The camp centre is
    // resolved through g_camps at read time, so a released camp unparks
    // everyone implicitly.
    std::unordered_map<uint32, uint32> g_parkedAlts;

    // One in-flight gathering per account. The two-step dance ("run it once
    // to wake them, again to seat them") existed because a freshly woken bot
    // takes seconds to enter the world and the command has long returned by
    // then. This queue is the fix: the world-update seater below watches the
    // woken names arrive and seats each one itself.
    struct AltGather
    {
        uint32 accountId = 0;
        ObjectGuid owner;                   // the real player who asked
        std::vector<std::string> names;     // alts still to wake/seat
        time_t wakeAt = 0;                  // 0 = wake already dispatched
        time_t deadline = 0;
        uint32 seated = 0;
        bool announced = false;
    };
    std::vector<AltGather> g_altGathers;

    bool WlIsRealPlayer(Player* p)
    {
        return p && p->GetSession() && !p->GetSession()->IsBot();
    }

    Camp* FindCamp(uint32 accountId)
    {
        for (Camp& c : g_camps)
            if (c.accountId == accountId)
                return &c;
        return nullptr;
    }

    float Dist2D(float ax, float ay, float bx, float by)
    {
        float const dx = ax - bx;
        float const dy = ay - by;
        return std::sqrt(dx * dx + dy * dy);
    }

    // ---------------------------------------------------------------------
    // Phase bit allocation
    // ---------------------------------------------------------------------
    // Greedy graph colouring over "camps close enough to see each other".
    // Anything further away than PHASE_REUSE_RADIUS is free to share a bit,
    // which is what keeps 31 bits sufficient for an entire world.
    uint8 PickPhaseBit(uint32 map, float x, float y)
    {
        uint32 used = 0;
        for (Camp const& c : g_camps)
        {
            if (c.map != map)
                continue;
            if (Dist2D(c.x, c.y, x, y) > PHASE_REUSE_RADIUS)
                continue;
            used |= (1u << c.phaseBit);
        }

        for (uint8 bit = CAMP_PHASE_BIT_MIN; bit <= CAMP_PHASE_BIT_MAX; ++bit)
            if (!(used & (1u << bit)))
                return bit;

        return 0;   // 31 camps within 600 yards; caller refuses the claim
    }

    // ---------------------------------------------------------------------
    // Prop spawning
    // ---------------------------------------------------------------------

    // Create one prop in the world. Deliberately NOT SummonGameObject: that
    // calls Unit::AddGameObject for a player summoner, which ties the object's
    // lifetime to the summoner and removes it when they log out or die.
    //
    // Also deliberately NOT SaveToDB. That writes a row to the WORLD database,
    // and the world DB is the one this repack ships as a dump and owners may
    // reimport - player-created content living there would be destroyed by a
    // routine reinstall. Our own table in the characters DB is the record;
    // what is in the map is a materialisation of it.
    ObjectGuid SpawnProp(Map* map, uint32 entry, float x, float y, float z,
        float o, uint32 phaseMask)
    {
        GameObjectTemplate const* tpl = sObjectMgr->GetGameObjectTemplate(entry);
        if (!tpl)
            return ObjectGuid::Empty;

        GameObject* go = new GameObject();
        ObjectGuid::LowType const guidLow =
            map->GenerateLowGuid<HighGuid::GameObject>();

        G3D::Quat const rot =
            G3D::Quat::fromAxisAngleRotation(G3D::Vector3::unitZ(), o);

        if (!go->Create(guidLow, entry, map, phaseMask, x, y, z, o, rot, 0,
                GO_STATE_READY))
        {
            delete go;
            return ObjectGuid::Empty;
        }

        // 0 = never respawns on a timer, because it never despawns. Combined
        // with no owner and no spell id, GameObject::Update leaves a GENERIC
        // object alone forever.
        go->SetRespawnTime(0);

        if (!map->AddToMap(go))
        {
            delete go;
            return ObjectGuid::Empty;
        }

        return go->GetGUID();
    }

    // Make sure every prop this camp owns is standing. Called when a player
    // walks in.
    //
    // ⚠️ The liveness check is not optional. A grid unloads once the last
    // player leaves it, and unloading destroys objects that have no row in the
    // world `gameobject` table - which is all of ours. Trusting g_liveProps
    // alone would leave the camp permanently empty after the first time
    // everybody wandered off, and it would look exactly like data loss.
    void MaterialiseCamp(Camp const& camp, Map* map)
    {
        QueryResult r = CharacterDatabase.Query(
            "SELECT id, entry, pos_x, pos_y, pos_z, orientation "
            "FROM wowlegends_warband_camp_object WHERE account_id = {}",
            camp.accountId);
        if (!r)
            return;

        uint32 const phaseMask = 1u << camp.phaseBit;
        uint32 spawned = 0;

        do
        {
            Field* f = r->Fetch();
            uint64 const id = f[0].Get<uint64>();

            auto const it = g_liveProps.find(id);
            if (it != g_liveProps.end())
            {
                if (map->GetGameObject(it->second))
                    continue;           // still standing
                g_liveProps.erase(it);  // grid unloaded it; put it back
            }

            ObjectGuid const guid = SpawnProp(map, f[1].Get<uint32>(),
                f[2].Get<float>(), f[3].Get<float>(), f[4].Get<float>(),
                f[5].Get<float>(), phaseMask);

            if (guid)
            {
                g_liveProps[id] = guid;
                ++spawned;
            }
        }
        while (r->NextRow());

        if (spawned)
            LOG_DEBUG("server", "[warbandcamp] materialised {} props for "
                "account {}", spawned, camp.accountId);
    }

    void DespawnProp(Map* map, uint64 id)
    {
        auto const it = g_liveProps.find(id);
        if (it == g_liveProps.end())
            return;

        if (GameObject* go = map->GetGameObject(it->second))
        {
            go->SetRespawnTime(0);
            go->Delete();
        }
        g_liveProps.erase(it);
    }

    // ---------------------------------------------------------------------
    // Where a camp may be planted
    // ---------------------------------------------------------------------
    // Permissive on purpose. The whole point of the rebuild is that you put
    // your camp somewhere YOU like, so this rejects only places where a camp
    // would be broken or obnoxious, not places somebody decided were untidy.
    char const* CampSiteBlocker(Player* p)
    {
        Map* map = p->GetMap();
        if (!map)
            return "여기에는 야영지를 만들 수 없습니다.";

        if (map->Instanceable())
            return "인스턴스 안에는 만들 수 없습니다. 일반 필드의 야외로 이동하세요.";
        if (p->InBattleground() || p->InArena())
            return "전장에는 야영지를 만들 수 없습니다.";

        if (p->IsInWater() || p->IsUnderWater())
            return "물속에는 야영지를 만들 수 없습니다.";
        if (p->IsFlying() || p->IsInFlight())
            return "먼저 지상에 내려서세요.";
        if (p->IsFalling())
            return "공중에 떠 있거나 낙하 중에는 사용할 수 없습니다.";
        if (!p->IsAlive() || p->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            return "죽은 상태에서는 사용할 수 없습니다.";
        if (p->IsInCombat())
            return "전투 중에는 사용할 수 없습니다.";

        // Transports move. A camp pinned to world coordinates on a boat would
        // be left hanging over the sea the moment the boat sailed.
        if (p->GetTransport())
            return "배나 비행선 위에는 야영지를 만들 수 없습니다.";

        AreaTableEntry const* area =
            sAreaTableStore.LookupEntry(p->GetAreaId());
        if (!area)
            return "지역 정보를 확인할 수 없어 야영지를 만들 수 없습니다.";

        if (area->flags & (AREA_FLAG_CAPITAL | AREA_FLAG_SLAVE_CAPITAL |
                           AREA_FLAG_SLAVE_CAPITAL2))
            return "대도시 안에는 만들 수 없습니다. 도시 밖의 야외로 이동하세요.";
        if (area->IsSanctuary())
            return "성역에는 야영지를 만들 수 없습니다.";
        if (area->flags & (AREA_FLAG_ARENA | AREA_FLAG_ARENA_INSTANCE))
            return "투기장에는 야영지를 만들 수 없습니다.";

        // Indoors covers inns, cellars, caves and the inside of buildings -
        // all places where a tent clips through somebody's floor.
        if (!p->IsOutdoors())
            return "야영지는 야외에 만들어야 합니다. 건물이나 동굴 밖으로 이동하세요.";

        // The ground has to be where the player thinks it is. Standing on a
        // bridge, a rooftop or a rock arch puts real ground metres below, and
        // props placed there would hang in the air or sink out of sight.
        float const groundZ = map->GetHeight(p->GetPhaseMask(),
            p->GetPositionX(), p->GetPositionY(), p->GetPositionZ(), true,
            MAX_FALL_DISTANCE);
        if (groundZ <= INVALID_HEIGHT)
            return "이곳의 지면을 확인할 수 없습니다.";
        if (std::fabs(p->GetPositionZ() - groundZ) > 3.0f)
            return "다리나 지붕 위가 아닌 땅 위에 서세요.";

        return nullptr;
    }

    // The Pilgrim's Way swears to cross the world on foot - no mounts, no
    // flight paths, no hearthstone. It enforces that by gating the hearthstone
    // ITEM and named spells, so it cannot see a chat command at all: `.camp
    // go` would have been a free teleport straight through a permanent oath,
    // which is worse than a convenience bug because the Path cannot be
    // un-sworn.
    //
    // Read from the DB rather than reaching into wowlegends_paths.cpp - the
    // path state lives in an anonymous namespace there. `.camp go` is rare and
    // already does DB work, so one more small read is the cheaper trade
    // against coupling two features together. status 0 = PATH_ACTIVE
    // (wowlegends_paths.cpp:129).
    bool IsSwornPilgrim(Player* p)
    {
        QueryResult r = CharacterDatabase.Query(
            "SELECT 1 FROM wowlegends_paths WHERE guid = {} "
            "AND path = 'pilgrims_way' AND status = 0 LIMIT 1",
            p->GetGUID().GetCounter());
        return r != nullptr;
    }

    // Everything that must NOT be escapable by teleporting home. Without these
    // `.camp go` is a free combat break, a mid-match arena exit, a corpse-run
    // skip and an instance escape.
    char const* CampTravelBlocker(Player* p)
    {
        if (IsSwornPilgrim(p))
            return "순례자의 길을 선택하여 순간이동할 수 없습니다. 직접 걸어서 이동해야 합니다.";

        if (p->IsInCombat())            return "전투 중에는 사용할 수 없습니다.";
        // TeleportTo strips crowd control on the way out, so combat alone is
        // not a sufficient gate - a stunned or feared player could otherwise
        // blink out of exactly the effect that was holding them.
        if (p->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_FLEEING |
                            UNIT_STATE_CONFUSED | UNIT_STATE_ROOT))
            return "이동 불가나 공포 등 제어 효과가 풀린 뒤 이동하세요.";
        if (!p->IsAlive())              return "죽은 상태에서는 사용할 수 없습니다.";
        if (p->HasPlayerFlag(PLAYER_FLAGS_GHOST))
            return "유령 상태에서는 이동할 수 없습니다. 먼저 시체를 찾으세요.";
        if (p->IsBeingTeleported())     return "이미 이동 중입니다.";
        if (p->IsInFlight())            return "비행 중에는 사용할 수 없습니다.";
        if (p->IsFalling())             return "공중에 떠 있거나 낙하 중에는 사용할 수 없습니다.";
        if (p->InBattleground())        return "전장에서는 야영지로 이동할 수 없습니다.";
        if (p->InArena())               return "투기장에서는 야영지로 이동할 수 없습니다.";
        if (p->IsSpectator())           return "관전 중에는 사용할 수 없습니다.";
        if (p->GetMap() && p->GetMap()->Instanceable())
            return "인스턴스 안에서는 야영지로 이동할 수 없습니다.";
        return nullptr;
    }

    // ---------------------------------------------------------------------
    // Proximity phasing
    // ---------------------------------------------------------------------

    void ApplyPhase(Player* p, uint32 mask)
    {
        if (p->GetPhaseMask() != mask)
            p->SetPhaseMask(mask, true);
    }

    // ⚠️ Restores the phase as well as the bookkeeping, and both halves matter.
    // An early version cleared only st.campAccount on map change, which looked
    // harmless and was not: hearthstone out of a camp and the player kept the
    // camp's phase BIT for the rest of the session, because every later call
    // saw campAccount == 0 and returned before touching the mask. They would
    // then see a stranger's tents through a hillside on the far side of the
    // world, any time they wandered near a camp that had been given the same
    // bit.
    void ClearCampPhase(Player* p, PlayerCampState& st)
    {
        st.campAccount = 0;

        if (!st.ownsPhase)
            return;
        st.ownsPhase = false;

        // Never take a phase off someone whose phase is not ours any more.
        if (p->IsGameMaster() || p->GetPhaseByAuras())
            return;

        ApplyPhase(p, PHASEMASK_NORMAL);
    }

    void UpdatePlayerCampPhase(Player* p, PlayerCampState& st)
    {
        // 🛑 HANDS OFF anyone whose phase is not ours to set.
        //
        // A GM is on PHASEMASK_ANYWHERE so they can see everything, and quest
        // phasing is driven by SPELL_AURA_PHASE. Overwriting either would break
        // it silently: a GM would stop seeing half the world, and a player
        // mid-way through a phased quest chain would watch the quest's version
        // of the zone vanish. Neither failure points back at this file.
        if (p->IsGameMaster() || p->GetPhaseByAuras())
        {
            // Something else owns the mask now. Forget both the camp and our
            // claim on the phase, so we never try to "restore" it later.
            st.campAccount = 0;
            st.ownsPhase = false;
            return;
        }

        Map* map = p->GetMap();
        if (!map)
            return;

        uint32 const mapId = map->GetId();
        float const px = p->GetPositionX();
        float const py = p->GetPositionY();

        // View distance is configurable; 0 means "no server-side gate",
        // bounded in practice by client draw distance - 250 covers it.
        // With the default 40 and the 150-yard camp separation at most ONE
        // camp can be in range; raise the distance (or set 0) and several
        // can, so the mask is the OR of every camp in range, not just the
        // nearest - that is what makes "see all the camps in a zone" work.
        float const viewConf = g_viewDist.load();
        float const view = viewConf > 0.0f ? viewConf : 250.0f;

        Camp const* best = nullptr;
        float bestDist = view;
        uint32 wantMask = PHASEMASK_NORMAL;
        // Small fixed buffer, no allocation on the hot path: 31 phase bits is
        // the hard ceiling on distinct camps, in range or not.
        Camp const* inRange[31];
        uint32 inRangeCount = 0;
        for (Camp const& c : g_camps)
        {
            if (c.map != mapId)
                continue;
            float const d = Dist2D(c.x, c.y, px, py);
            if (d > view)
                continue;
            wantMask |= (1u << c.phaseBit);
            if (inRangeCount < 31)
                inRange[inRangeCount++] = &c;
            if (d < bestDist)
            {
                bestDist = d;
                best = &c;
            }
        }

        if (!best)
        {
            ClearCampPhase(p, st);
            return;
        }

        if (st.campAccount == best->accountId &&
            p->GetPhaseMask() == wantMask)
            return;                                  // already correct

        // Materialise ONLY on a mask transition, exactly as v1 did for its
        // single camp - doing it every 1-second tick would walk every prop's
        // liveness check per player per second for nothing. A grid can only
        // unload when no player is near, and no player near means no ticks,
        // so a transition is always there to catch the respawn.
        for (uint32 i = 0; i < inRangeCount; ++i)
            MaterialiseCamp(*inRange[i], map);

        st.campAccount = best->accountId;
        st.ownsPhase = true;
        ApplyPhase(p, wantMask);
    }

    // Seat one woken alt at its ring slot and turn it into camp scenery: out
    // of the party, follow stood down, registered for the camp strolls. The
    // master link deliberately survives - whispers still reach it, and a
    // spoken `$follow` is the way to call it back to your side.
    // Caller holds g_campMutex.
    bool SeatAltAtCamp(Player* alt, Camp const& camp, uint32 slot)
    {
        float const angle =
            (2.0f * float(M_PI) * float(slot)) / float(CAMP_MAX_ALTS);
        float const x = camp.x + std::cos(angle) * CAMP_ALT_RING;
        float const y = camp.y + std::sin(angle) * CAMP_ALT_RING;
        float z = camp.z;
        if (Map* map = sMapMgr->FindBaseMap(camp.map))
        {
            float const g = map->GetHeight(PHASEMASK_NORMAL, x, y,
                camp.z + 10.0f, true, MAX_FALL_DISTANCE);
            if (g > INVALID_HEIGHT && std::fabs(g - camp.z) < 15.0f)
                z = g;
        }

        // Remember where this character REALLY was before we move it, so a
        // human logging in later can be put back there. Two guards, both
        // learned from live data on day one:
        // - INSERT IGNORE: a row that already exists (parked before, never
        //   human-visited since) holds the ORIGINAL spot; keep it.
        // - 🛑 NEVER record a position that is already inside the camp
        //   bubble. An alt re-gathered after a restart wakes AT the camp
        //   (that is its saved position), and recording that as its
        //   "origin" poisons the row forever - the first live table was
        //   eight rows all pointing at the firepit. No row is honest here:
        //   where the alt truly was is history the restart already lost.
        bool const wasAtCamp = alt->GetMapId() == camp.map &&
            Dist2D(camp.x, camp.y, alt->GetPositionX(), alt->GetPositionY())
                < CAMP_RADIUS + 20.0f;
        if (!wasAtCamp)
            CharacterDatabase.Execute(
                "INSERT IGNORE INTO wowlegends_warband_alt_origin "
                "(guid, map, pos_x, pos_y, pos_z, orientation, parked_at) "
                "VALUES ({}, {}, {}, {}, {}, {}, {})",
                alt->GetGUID().GetCounter(), alt->GetMapId(),
                alt->GetPositionX(), alt->GetPositionY(),
                alt->GetPositionZ(), alt->GetOrientation(),
                uint32(time(nullptr)));

        if (!alt->TeleportTo(camp.map, x, y, z,
                std::atan2(camp.y - y, camp.x - x)))
            return false;   // never park a bot that refused to travel

        if (alt->GetGroup())
            alt->RemoveFromGroup();
        if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(alt))
            botAI->ChangeStrategy("-follow", BOT_STATE_NON_COMBAT);

        g_parkedAlts[alt->GetGUID().GetCounter()] = camp.accountId;
        return true;
    }

    // Queue a gathering: the seater in OnUpdate wakes and seats from here.
    // wakeDelay > 0 defers the `.playerbots bot add` dispatches - needed on
    // the login path, where this module's login hook may run BEFORE
    // mod-playerbots has built the player's PlayerbotMgr (cross-module hook
    // order is undefined), and "bot add" through a session with no mgr is a
    // silent refusal. A few seconds ducks the race entirely.
    // Caller holds g_campMutex.
    void EnqueueAltGather(Player* owner, uint32 accountId, time_t wakeDelay)
    {
        g_altGathers.erase(
            std::remove_if(g_altGathers.begin(), g_altGathers.end(),
                [accountId](AltGather const& g)
                { return g.accountId == accountId; }),
            g_altGathers.end());

        AltGather g;
        g.accountId = accountId;
        g.owner = owner->GetGUID();
        g.wakeAt = time(nullptr) + wakeDelay;
        g.deadline = g.wakeAt + 45;

        // No LIMIT in the query: the faction filter runs after the fetch,
        // and limiting first could fill the quota with characters the filter
        // then throws away. Accounts hold 10 characters; this is tiny.
        bool const sameOnly = g_altsSameFactionOnly.load();
        TeamId const ownerTeam = owner->GetTeamId();
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT name, race FROM characters WHERE account = {} "
                "AND guid <> {} AND deleteInfos_Account IS NULL",
                accountId, owner->GetGUID().GetCounter()))
        {
            do
            {
                Field* f = r->Fetch();
                if (sameOnly &&
                    Player::TeamIdForRace(f[1].Get<uint8>()) != ownerTeam)
                    continue;
                g.names.push_back(f[0].Get<std::string>());
                if (g.names.size() >= CAMP_MAX_ALTS)
                    break;
            }
            while (r->NextRow());
        }

        if (!g.names.empty())
            g_altGathers.push_back(std::move(g));
    }
}

// Read by mod-playerbots' idle-stroll action on the MAP THREADS, through the
// module-standard plain `extern` (no cross-module header). Mutex-guarded for
// the same reason everything else here is: six map threads.
bool WlWarbandCampParked(uint32 lowGuid, float& cx, float& cy, float& cz)
{
    std::lock_guard<std::mutex> lock(g_campMutex);
    auto const it = g_parkedAlts.find(lowGuid);
    if (it == g_parkedAlts.end())
        return false;
    for (Camp const& c : g_camps)
    {
        if (c.accountId == it->second)
        {
            cx = c.x;
            cy = c.y;
            cz = c.z;
            return true;
        }
    }
    return false;
}

class WowLegendsWarbandCampCommand : public CommandScript
{
public:
    WowLegendsWarbandCampCommand()
        : CommandScript("WowLegendsWarbandCampCommand") { }

    ChatCommandTable GetCommands() const override
    {
        // ⚠️ These MUST be static. The parent entry stores a
        // std::reference_wrapper to the vector rather than copying it, so a
        // local would dangle the moment GetCommands returns.
        static ChatCommandTable campTable =
        {
            { "claim",  HandleCampClaimCommand,  SEC_PLAYER, Console::No },
            { "go",     HandleCampGoCommand,     SEC_PLAYER, Console::No },
            { "leave",  HandleCampLeaveCommand,  SEC_PLAYER, Console::No },
            { "props",  HandleCampPropsCommand,  SEC_PLAYER, Console::No },
            { "place",  HandleCampPlaceCommand,  SEC_PLAYER, Console::No },
            { "remove", HandleCampRemoveCommand, SEC_PLAYER, Console::No },
            // The only entry that is Console::Yes, deliberately - see the
            // handler for why it exists.
            { "diag",   HandleCampDiagCommand,   SEC_ADMINISTRATOR,
                                                             Console::Yes },
            { "alts",   HandleCampAltsCommand,   SEC_PLAYER, Console::No },
            { "visit",  HandleCampVisitCommand,  SEC_PLAYER, Console::No },
            { "list",   HandleCampListCommand,   SEC_PLAYER, Console::No },
            { "catalogue", HandleCampCatalogueCommand,
                                              SEC_GAMEMASTER, Console::No },
            { "",       HandleCampStatusCommand, SEC_PLAYER, Console::No },
        };

        static ChatCommandTable baseTable =
        {
            { "camp", campTable },
        };

        return baseTable;
    }

    static bool Gate(ChatHandler* handler, Player*& me)
    {
        me = handler->GetSession() ? handler->GetSession()->GetPlayer()
                                   : nullptr;

        // Bots first, before anything is said or written. Every bot holds a
        // real WorldSession with a real account id, and there are thousands of
        // them - one unguarded write path would litter the world with camps.
        if (!WlIsRealPlayer(me))
            return false;

        if (!g_enabled.load())
        {
            handler->SendSysMessage(
                "이 서버에서는 전투단 야영지가 비활성화되어 있습니다.");
            return false;
        }
        return true;
    }

    static void DescribeCamp(ChatHandler* handler, Camp const& c)
    {
        char const* zone = "somewhere";
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(c.zoneId))
            zone = area->area_name[0];

        uint32 props = 0;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM wowlegends_warband_camp_object "
                "WHERE account_id = {}", c.accountId))
            props = r->Fetch()[0].Get<uint32>();

        uint32 const maxProps = g_maxProps.load();
        if (maxProps)
            handler->PSendSysMessage(
                "야영지는 |cffffff00{}|r에 있으며, 소품 {} / {}개를 설치했습니다.", zone, props, maxProps);
        else
            handler->PSendSysMessage(
                "야영지는 |cffffff00{}|r에 있으며, 소품 {}개를 설치했습니다.", zone, props);
    }

    static bool HandleCampStatusCommand(ChatHandler* handler)
    {
        Player* me = nullptr;
        if (!Gate(handler, me))
            return true;

        std::lock_guard<std::mutex> lock(g_campMutex);

        Camp const* c = FindCamp(me->GetSession()->GetAccountId());
        if (!c)
        {
            handler->SendSysMessage("아직 야영지가 없습니다. 원하는 야외 지면에서 |cffffff00.camp claim|r을 입력하세요.");
            handler->SendSysMessage("야영지는 계정에 속하며 같은 계정의 모든 캐릭터가 공유합니다.");
            return true;
        }

        DescribeCamp(handler, *c);
        handler->SendSysMessage("|cffffff00.camp go|r로 야영지로 이동하고, "
            "|cffffff00.camp props|r로 설치 가능한 물건을 확인하세요.");
        return true;
    }

    static bool HandleCampClaimCommand(ChatHandler* handler)
    {
        Player* me = nullptr;
        if (!Gate(handler, me))
            return true;

        std::lock_guard<std::mutex> lock(g_campMutex);

        uint32 const accountId = me->GetSession()->GetAccountId();
        if (Camp const* existing = FindCamp(accountId))
        {
            DescribeCamp(handler, *existing);
            handler->SendSysMessage("다른 곳에 만들려면 먼저 |cffffff00.camp leave|r로 기존 야영지를 철거하세요.");
            return true;
        }

        if (char const* blocked = CampSiteBlocker(me))
        {
            handler->SendSysMessage(blocked);
            return true;
        }

        uint32 const mapId = me->GetMapId();
        float const x = me->GetPositionX();
        float const y = me->GetPositionY();

        for (Camp const& c : g_camps)
        {
            if (c.map != mapId)
                continue;
            if (Dist2D(c.x, c.y, x, y) < CAMP_MIN_SEPARATION)
            {
                handler->SendSysMessage("다른 야영지와 너무 가깝습니다. 조금 더 떨어진 곳에서 시도하세요.");
                return true;
            }
        }

        uint8 const bit = PickPhaseBit(mapId, x, y);
        if (!bit)
        {
            handler->SendSysMessage("주변에 야영지가 너무 많습니다. 더 멀리 떨어진 곳에서 시도하세요.");
            return true;
        }

        Camp c;
        c.accountId = accountId;
        c.map = mapId;
        c.x = x;
        c.y = y;
        c.z = me->GetPositionZ();
        c.o = me->GetOrientation();
        c.phaseBit = bit;
        c.zoneId = me->GetZoneId();

        // INSERT IGNORE plus a read-back, not ON DUPLICATE KEY UPDATE: two
        // characters on the same account claiming at once must not end up
        // with two rows or a silently moved camp. account_id is the only
        // unique key, so IGNORE makes the first one win cleanly.
        CharacterDatabase.DirectExecute(
            "INSERT IGNORE INTO wowlegends_warband_camp "
            "(account_id, map, pos_x, pos_y, pos_z, orientation, phase_bit, "
            "zone_id) VALUES ({}, {}, {:.4f}, {:.4f}, {:.4f}, {:.4f}, {}, {})",
            c.accountId, c.map, c.x, c.y, c.z, c.o, c.phaseBit, c.zoneId);

        QueryResult r = CharacterDatabase.Query(
            "SELECT phase_bit FROM wowlegends_warband_camp "
            "WHERE account_id = {}", accountId);
        if (!r)
        {
            handler->SendSysMessage("야영지를 저장하지 못했습니다. 관리자에게 알려 주세요.");
            LOG_ERROR("server", "[warbandcamp] claim write failed for "
                "account {}", accountId);
            return true;
        }
        c.phaseBit = r->Fetch()[0].Get<uint8>();

        g_camps.push_back(c);

        char const* zone = "here";
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(c.zoneId))
            zone = area->area_name[0];

        handler->PSendSysMessage("|cffffff00{}|r에 전투단 야영지를 만들었습니다.", zone);
        handler->SendSysMessage("|cffffff00.camp place campfire|r으로 모닥불을 설치하세요. 다른 소품은 |cffffff00.camp props|r로 확인할 수 있습니다.");
        LOG_INFO("server", "[warbandcamp] account {} ({}) claimed map {} "
            "({:.1f}, {:.1f}) zone {} phase bit {}", accountId, me->GetName(),
            c.map, c.x, c.y, c.zoneId, c.phaseBit);
        return true;
    }

    static bool HandleCampGoCommand(ChatHandler* handler)
    {
        Player* me = nullptr;
        if (!Gate(handler, me))
            return true;

        std::lock_guard<std::mutex> lock(g_campMutex);

        uint32 const accountId = me->GetSession()->GetAccountId();
        Camp const* found = FindCamp(accountId);
        if (!found)
        {
            handler->SendSysMessage("아직 야영지가 없습니다. 원하는 야외 지면에서 |cffffff00.camp claim|r을 입력하세요.");
            return true;
        }
        Camp const camp = *found;

        if (char const* blocked = CampTravelBlocker(me))
        {
            handler->SendSysMessage(blocked);
            return true;
        }

        // Without a cooldown this is a reagentless, castless, uninterruptible
        // Hearthstone, which would hand every player the
        // no-hearthstone-cooldown feature that deliberately ships OFF.
        time_t const now = time(nullptr);
        auto const it = g_goCooldown.find(accountId);
        if (it != g_goCooldown.end() && now < it->second)
        {
            handler->PSendSysMessage(
                "다시 이동하려면 {}초 기다려야 합니다.",
                uint32(it->second - now));
            return true;
        }

        // Charge the cooldown ONLY if the teleport was actually accepted.
        // TeleportTo can refuse (a Death Knight still confined to Ebon Hold is
        // the live case), and billing someone five minutes for a journey that
        // never happened reads as a broken feature.
        if (!me->TeleportTo(camp.map, camp.x, camp.y, camp.z, camp.o))
        {
            handler->SendSysMessage(
                "현재 상태에서는 야영지로 이동할 수 없습니다.");
            return true;
        }

        g_goCooldown[accountId] = now + CAMP_GO_COOLDOWN_SECONDS;
        return true;
    }

    static bool HandleCampLeaveCommand(ChatHandler* handler,
        Optional<std::string> confirm)
    {
        Player* me = nullptr;
        if (!Gate(handler, me))
            return true;

        std::lock_guard<std::mutex> lock(g_campMutex);

        uint32 const accountId = me->GetSession()->GetAccountId();
        Camp const* found = FindCamp(accountId);
        if (!found)
        {
            handler->SendSysMessage("철거할 야영지가 없습니다.");
            return true;
        }
        Camp const camp = *found;

        // Two-step, because this throws away everything they built and there
        // is no undo.
        if (!confirm || *confirm != "confirm")
        {
            DescribeCamp(handler, camp);
            handler->SendSysMessage("|cffff2020되돌릴 수 없습니다.|r 야영지의 모든 소품이 사라집니다.");
            handler->SendSysMessage("정말 철거하려면 |cffffff00.camp leave confirm|r을 입력하세요.");
            return true;
        }

        // Take the props out of the world before the rows go, otherwise the
        // objects stand there for the rest of the uptime with nothing left
        // pointing at them.
        // FindBaseMap can be null if nobody has been near that map since the
        // last restart - in which case none of the props were ever spawned, so
        // there is nothing standing to take down. Either way the bookkeeping
        // entries have to go, or they leak for the rest of the uptime.
        Map* campMap = sMapMgr->FindBaseMap(camp.map);
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT id FROM wowlegends_warband_camp_object "
                "WHERE account_id = {}", accountId))
        {
            do
            {
                uint64 const id = r->Fetch()[0].Get<uint64>();
                if (campMap)
                    DespawnProp(campMap, id);
                else
                    g_liveProps.erase(id);
            }
            while (r->NextRow());
        }

        CharacterDatabase.DirectExecute(
            "DELETE FROM wowlegends_warband_camp_object WHERE account_id = {}",
            accountId);
        CharacterDatabase.DirectExecute(
            "DELETE FROM wowlegends_warband_camp WHERE account_id = {}",
            accountId);

        g_camps.erase(std::remove_if(g_camps.begin(), g_camps.end(),
            [accountId](Camp const& e) { return e.accountId == accountId; }),
            g_camps.end());

        // Unpark the warband and cancel any gathering still in flight. The
        // alts stay logged in wherever they stand - they just stop being
        // camp scenery, since there is no camp.
        for (auto it = g_parkedAlts.begin(); it != g_parkedAlts.end();)
        {
            if (it->second == accountId)
                it = g_parkedAlts.erase(it);
            else
                ++it;
        }
        g_altGathers.erase(
            std::remove_if(g_altGathers.begin(), g_altGathers.end(),
                [accountId](AltGather const& g)
                { return g.accountId == accountId; }),
            g_altGathers.end());

        // Anyone standing in it right now must be let back out into the
        // ordinary world, this player included.
        for (auto& [guid, st] : g_playerState)
            if (st.campAccount == accountId)
                if (Player* other = ObjectAccessor::FindPlayer(guid))
                    ClearCampPhase(other, st);

        handler->SendSysMessage("야영지를 철거했습니다. 다른 곳에 다시 만들 수 있습니다.");
        LOG_INFO("server", "[warbandcamp] account {} ({}) released its camp",
            accountId, me->GetName());
        return true;
    }

    static bool HandleCampPropsCommand(ChatHandler* handler)
    {
        Player* me = nullptr;
        if (!Gate(handler, me))
            return true;

        std::lock_guard<std::mutex> lock(g_campMutex);

        handler->SendSysMessage("야영지에 설치할 수 있는 소품:");

        // Three per line - a 30-entry list one-per-line scrolls the whole chat
        // frame away.
        std::string line;
        uint32 n = 0;
        for (PropDef const& def : g_props)
        {
            line += "|cffffff00";
            line += def.key;
            line += "|r ";
            if (++n % 3 == 0)
            {
                handler->SendSysMessage(line);
                line.clear();
            }
        }
        if (!line.empty())
            handler->SendSysMessage(line);

        handler->SendSysMessage("설치할 위치에 서서 |cffffff00.camp place <name>|r을 입력하세요.");
        return true;
    }

    static bool HandleCampPlaceCommand(ChatHandler* handler,
        Optional<std::string> what)
    {
        Player* me = nullptr;
        if (!Gate(handler, me))
            return true;

        std::lock_guard<std::mutex> lock(g_campMutex);

        uint32 const accountId = me->GetSession()->GetAccountId();

        // ⚠️ Copy, do not hold the pointer. FindCamp returns into g_camps, and
        // anything below that could push_back or erase would invalidate it.
        // Nothing here does today; the next person to add a line might.
        Camp const* found = FindCamp(accountId);
        if (!found)
        {
            handler->SendSysMessage("아직 야영지가 없습니다. 원하는 야외 지면에서 |cffffff00.camp claim|r을 입력하세요.");
            return true;
        }
        Camp const camp = *found;
        uint32 const campMap = camp.map;
        uint8 const campPhaseBit = camp.phaseBit;

        if (!what || what->empty())
        {
            handler->SendSysMessage("설치할 소품을 지정하세요. |cffffff00.camp props|r로 목록을 볼 수 있습니다.");
            return true;
        }

        std::string key = *what;
        std::transform(key.begin(), key.end(), key.begin(),
            [](unsigned char ch) { return char(std::tolower(ch)); });

        PropDef const* def = FindProp(key);
        if (!def)
        {
            handler->PSendSysMessage("'{}' 소품이 없습니다. |cffffff00.camp props|r로 확인하세요.", key);
            return true;
        }

        if (me->GetMapId() != campMap ||
            Dist2D(camp.x, camp.y, me->GetPositionX(), me->GetPositionY())
                > CAMP_PROP_RADIUS)
        {
            handler->SendSysMessage("자신의 야영지 안에 있어야 합니다. |cffffff00.camp go|r로 이동하세요.");
            return true;
        }

        // Placing while airborne or swimming would leave the prop floating.
        if (me->IsInWater() || me->IsFlying() || me->IsInFlight() ||
            me->IsFalling())
        {
            handler->SendSysMessage("먼저 땅 위에 내려서세요.");
            return true;
        }

        uint32 count = 0;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT COUNT(*) FROM wowlegends_warband_camp_object "
                "WHERE account_id = {}", accountId))
            count = r->Fetch()[0].Get<uint32>();

        uint32 const maxProps = g_maxProps.load();
        if (maxProps && count >= maxProps)
        {
            handler->PSendSysMessage("야영지 소품이 가득 찼습니다 ({}개). |cffffff00.camp remove|r로 일부를 철거하세요.",
                maxProps);
            return true;
        }

        time_t const now = time(nullptr);
        auto const cd = g_propCooldown.find(accountId);
        if (cd != g_propCooldown.end() && now < cd->second)
        {
            handler->SendSysMessage("잠시 기다렸다가 하나씩 설치하세요.");
            return true;
        }

        Map* map = me->GetMap();
        float const o = me->GetOrientation();

        // Set it down IN FRONT of you, not on top of you. See PropDef -
        // placing at the player's own feet put the first wagon around the
        // player like a box.
        float x = me->GetPositionX() + std::cos(o) * def->clearance;
        float y = me->GetPositionY() + std::sin(o) * def->clearance;

        // ⚠️ The offset can push a prop out of the camp when you are placing
        // from the edge - a wagon steps 8 yards, and the gap between
        // CAMP_PROP_RADIUS and CAMP_RADIUS is exactly that. If it lands
        // outside, keep it at the player's feet rather than dropping it into
        // open country where nobody phased into the camp can see it.
        if (Dist2D(camp.x, camp.y, x, y) > CAMP_RADIUS)
        {
            x = me->GetPositionX();
            y = me->GetPositionY();
            handler->SendSysMessage("야영지 경계에 너무 가까워 발밑에 설치했습니다.");
        }

        // Height: FOLLOW THE PLAYER, NOT THE TERRAIN, when they are standing
        // on something. Kneuma, 2026-08-09: a candle placed while standing ON
        // a table went to the floor under it. Cause: our placed props are
        // DYNAMIC gameobjects, and Map::GetHeight only sees terrain + static
        // vmaps - so the probe looks straight through the table. The probe
        // under the PLAYER tells us which world they are in: standing more
        // than half a yard above the terrain means they are on a structure,
        // and the prop belongs at THEIR height (the table top), not the
        // floor's. On plain ground the old terrain-follow still applies, so
        // props ahead of you sit on the slope instead of floating.
        float z = me->GetPositionZ();
        float const underMe = map->GetHeight(PHASEMASK_NORMAL,
            me->GetPositionX(), me->GetPositionY(), z + 2.0f, true,
            MAX_FALL_DISTANCE);
        bool const onStructure =
            underMe > INVALID_HEIGHT && (z - underMe) > 0.5f;
        if (!onStructure)
        {
            float const groundZ = map->GetHeight(PHASEMASK_NORMAL, x, y,
                z + 2.0f, true, MAX_FALL_DISTANCE);
            if (groundZ > INVALID_HEIGHT && std::fabs(z - groundZ) < 15.0f)
                z = groundZ;
        }

        CharacterDatabase.DirectExecute(
            "INSERT INTO wowlegends_warband_camp_object "
            "(account_id, entry, pos_x, pos_y, pos_z, orientation) "
            "VALUES ({}, {}, {:.4f}, {:.4f}, {:.4f}, {:.4f})",
            accountId, def->entry, x, y, z, o);

        // Read the row back to learn its id. LAST_INSERT_ID() is per
        // CONNECTION and consecutive DirectExecute calls round-robin the pool,
        // so it cannot be trusted here.
        uint64 id = 0;
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT id FROM wowlegends_warband_camp_object "
                "WHERE account_id = {} ORDER BY id DESC LIMIT 1", accountId))
            id = r->Fetch()[0].Get<uint64>();

        // ⚠️ No id means the INSERT did not land. Spawning anyway would put an
        // object in the world that nothing owns: no row to delete it from, no
        // entry in g_liveProps, and no way for the player to remove it. It
        // would simply stand there until the next restart.
        if (!id)
        {
            handler->SendSysMessage("저장하지 못했습니다. 관리자에게 알려 주세요.");
            LOG_ERROR("server", "[warbandcamp] prop insert failed for "
                "account {} (entry {})", accountId, def->entry);
            return true;
        }

        ObjectGuid const guid = SpawnProp(map, def->entry, x, y, z, o,
            1u << campPhaseBit);
        if (!guid)
        {
            CharacterDatabase.DirectExecute(
                "DELETE FROM wowlegends_warband_camp_object WHERE id = {}", id);
            handler->SendSysMessage("여기에는 설치할 수 없습니다. 옆으로 조금 이동해서 시도하세요.");
            return true;
        }

        if (id)
            g_liveProps[id] = guid;

        g_propCooldown[accountId] = now + CAMP_PROP_COOLDOWN_SECONDS;
        if (maxProps)
            handler->PSendSysMessage("|cffffff00{}|r 설치 완료 ({} / {}개).",
                def->label, count + 1, maxProps);
        else
            handler->PSendSysMessage("|cffffff00{}|r 설치 완료 (현재 {}개).",
                def->label, count + 1);
        return true;
    }

    // ⚙️ ADMIN DIAGNOSTIC. The one command here that works from RA and the
    // console, and the reason it exists is worth writing down.
    //
    // Every other `.camp` command is Console::No, because each one needs the
    // player who typed it. That makes the whole feature untestable without a
    // client logged in - and the single most dangerous unknown is whether
    // GameObject::Create + Map::AddToMap actually accept our parameters. If
    // they do not, EVERY prop placement fails silently, the camp stays empty,
    // and the first person to find out is a player.
    //
    // So: point this at any character who is online (a bot will do) and it runs
    // the real gates against their real position, then spawns one real prop
    // through the real SpawnProp path, proves the object exists in the map, and
    // removes it again. It reports rather than changes anything.
    //
    // ⚠️ It takes a character NAME rather than a selection precisely so it can
    // be driven from RA. SEC_ADMINISTRATOR because it names arbitrary players.
    static bool HandleCampDiagCommand(ChatHandler* handler, std::string name)
    {
        Player* target = ObjectAccessor::FindPlayerByName(name, true);
        if (!target)
        {
            handler->PSendSysMessage("'{}' 캐릭터가 접속해 있지 않습니다.", name);
            return true;
        }

        Map* map = target->GetMap();
        if (!map)
        {
            handler->SendSysMessage("해당 캐릭터의 지도 정보가 없습니다.");
            return true;
        }

        float const x = target->GetPositionX();
        float const y = target->GetPositionY();
        float const z = target->GetPositionZ();

        handler->PSendSysMessage("--- 전투단 진단: {} ---", target->GetName());
        handler->PSendSysMessage("지도 {} 지역 {} 구역 {} 위치 {:.1f} {:.1f} {:.1f}",
            map->GetId(), target->GetZoneId(), target->GetAreaId(), x, y, z);

        char const* zoneName = "?";
        if (AreaTableEntry const* a =
                sAreaTableStore.LookupEntry(target->GetZoneId()))
            zoneName = a->area_name[0];
        handler->PSendSysMessage("지역명: {}", zoneName);

        float const groundZ = map->GetHeight(target->GetPhaseMask(), x, y, z,
            true, MAX_FALL_DISTANCE);
        handler->PSendSysMessage("야외 {} | 지면 {:.2f} (높이 차 {:.2f}) | 수중 {} | 위상 {}", target->IsOutdoors() ? "예" : "아니요",
            groundZ, z - groundZ, target->IsInWater() ? "예" : "아니요",
            target->GetPhaseMask());

        std::lock_guard<std::mutex> lock(g_campMutex);

        char const* blocked = CampSiteBlocker(target);
        handler->PSendSysMessage("현재 위치 야영지 생성: {}",
            blocked ? blocked : "가능");

        uint8 const bit = PickPhaseBit(map->GetId(), x, y);
        handler->PSendSysMessage("사용 가능한 위상 비트: {} | 서버 야영지 수: {}",
            bit, g_camps.size());

        // The part that cannot be checked any other way: does a prop actually
        // materialise in a live map?
        uint32 const testEntry = g_props.empty() ? 0 : g_props[0].entry;
        if (!testEntry)
        {
            handler->SendSysMessage("소품 생성: 생략 (목록 없음)");
            return true;
        }

        ObjectGuid const guid = SpawnProp(map, testEntry, x, y, z, 0.0f,
            1u << CAMP_PHASE_BIT_MAX);
        if (!guid)
        {
            handler->PSendSysMessage("소품 생성: |cffff2020실패|r - GameObject::Create 또는 AddToMap이 항목 {}을 거부했습니다.", testEntry);
            LOG_ERROR("server", "[warbandcamp] diag: spawn of {} FAILED on "
                "map {}", testEntry, map->GetId());
            return true;
        }

        GameObject* go = map->GetGameObject(guid);
        handler->PSendSysMessage("소품 생성: |cff20ff20성공|r 항목 {} 식별자 {} | 지도 등록: {} | 표시 위상 {}", testEntry, guid.GetCounter(),
            go ? "예" : "NO", go ? go->GetPhaseMask() : 0);

        // Never entered into g_liveProps, so there is no bookkeeping to undo -
        // it just goes straight back out of the world.
        if (go)
        {
            go->SetRespawnTime(0);
            go->Delete();
        }

        // --- the proximity phase pipeline -------------------------------
        // UpdatePlayerCampPhase is what the per-tick hook calls, and it is the
        // heart of the feature: walk in, the camp appears. The hook itself only
        // runs for real players, so this is the only way to exercise the
        // function without a client logged in. Plant a throwaway camp exactly
        // where the target stands, run the real code, read the mask back, then
        // put everything the way it was.
        uint32 const beforeMask = target->GetPhaseMask();
        Camp probe;
        probe.accountId = 0xFFFFFFFF;
        probe.map = map->GetId();
        probe.x = x;
        probe.y = y;
        probe.z = z;
        probe.phaseBit = bit ? bit : CAMP_PHASE_BIT_MIN;
        probe.zoneId = target->GetZoneId();
        g_camps.push_back(probe);

        PlayerCampState st;
        UpdatePlayerCampPhase(target, st);
        uint32 const inCampMask = target->GetPhaseMask();
        uint32 const wantMask = PHASEMASK_NORMAL | (1u << probe.phaseBit);

        ClearCampPhase(target, st);
        uint32 const afterMask = target->GetPhaseMask();

        g_camps.pop_back();

        bool const phasedIn = (inCampMask == wantMask);
        bool const restored = (afterMask == beforeMask);
        handler->PSendSysMessage("위상 진입: {} (마스크 {} -> {}, 기대값 {})",
            phasedIn ? "|cff20ff20성공|r" : "|cffff2020실패|r",
            beforeMask, inCampMask, wantMask);
        handler->PSendSysMessage("위상 해제: {} (복원 마스크 {})",
            restored ? "|cff20ff20성공|r" : "|cffff2020실패|r", afterMask);

        if (!phasedIn || !restored)
            LOG_ERROR("server", "[warbandcamp] diag: phase pipeline broken for "
                "{} ({} -> {} -> {}, wanted {})", target->GetName(),
                beforeMask, inCampMask, afterMask, wantMask);

        handler->SendSysMessage("진단을 마쳤습니다. 변경 사항은 없습니다.");
        return true;
    }

    // Travel to somebody else's camp by character name.
    //
    // 📌 This closes a hole in the promise rather than adding a new feature.
    // The whole pitch is "no invite list, a friend just walks in" - but until
    // now there was no way to FIND a friend's camp. They could tell you the
    // zone and you could ride around looking. A neighbourhood nobody can
    // navigate to is a neighbourhood of one.
    //
    // Camps are public BY DESIGN (see the design doc: no invite plumbing was
    // always the point), so this needs no permission system. It reuses the
    // same travel blockers and the same cooldown as `.camp go`, so it cannot
    // become a cheaper escape hatch than travelling to your own camp.
    static bool HandleCampVisitCommand(ChatHandler* handler,
        Optional<std::string> who)
    {
        Player* me = nullptr;
        if (!Gate(handler, me))
            return true;

        std::lock_guard<std::mutex> lock(g_campMutex);

        if (!who || who->empty())
        {
            handler->SendSysMessage("방문할 캐릭터를 지정하세요: |cffffff00.camp visit <name>|r. 근처 야영지는 |cffffff00.camp list|r로 확인하세요.");
            return true;
        }

        std::string name = *who;
        if (!normalizePlayerName(name))
        {
            handler->SendSysMessage("캐릭터 이름이 올바르지 않습니다.");
            return true;
        }

        // Character name -> account -> camp. Deliberately by CHARACTER, since
        // that is what a player knows about their friend; the camp itself
        // belongs to the account behind it.
        //
        // 🛑 RESOLVED THROUGH THE CHARACTER CACHE, NOT A QUERY, AND THAT IS A
        // SECURITY DECISION. The obvious version builds
        // "... WHERE name = '{}'" out of `name`, and `name` is whatever the
        // player typed. normalizePlayerName does NOT make that safe: read it
        // (ObjectMgr.cpp:209) and it rejects only empty strings, embedded
        // SPACES and invalid UTF-8. It does not touch quotes. `Bob';DROP` has
        // no space in it and sails straight through into the query.
        //
        // The cache is an in-memory lookup, so there is no string to escape,
        // nothing to get wrong later, and it is faster besides. Never
        // interpolate a player-supplied name into SQL in this module.
        ObjectGuid const targetGuid =
            sCharacterCache->GetCharacterGuidByName(name);
        uint32 const targetAccount = targetGuid
            ? sCharacterCache->GetCharacterAccountIdByGuid(targetGuid) : 0;

        if (!targetAccount)
        {
            handler->PSendSysMessage("이 서버에 {} 캐릭터가 없습니다.",
                name);
            return true;
        }

        Camp const* found = FindCamp(targetAccount);
        if (!found)
        {
            handler->PSendSysMessage("{}은 아직 야영지를 만들지 않았습니다.", name);
            return true;
        }
        Camp const camp = *found;

        if (camp.accountId == me->GetSession()->GetAccountId())
        {
            handler->SendSysMessage("자신의 야영지입니다. |cffffff00.camp go|r로 이동하세요.");
            return true;
        }

        if (char const* blocked = CampTravelBlocker(me))
        {
            handler->SendSysMessage(blocked);
            return true;
        }

        uint32 const accountId = me->GetSession()->GetAccountId();
        time_t const now = time(nullptr);
        auto const it = g_goCooldown.find(accountId);
        if (it != g_goCooldown.end() && now < it->second)
        {
            handler->PSendSysMessage(
                "다시 이동하려면 {}초 기다려야 합니다.",
                uint32(it->second - now));
            return true;
        }

        // Arrive a few paces off the centre and facing in, so a visitor lands
        // at the edge of the camp looking at it rather than materialising on
        // top of the host's campfire.
        float const ax = camp.x + std::cos(camp.o + float(M_PI)) * 12.0f;
        float const ay = camp.y + std::sin(camp.o + float(M_PI)) * 12.0f;
        float az = camp.z;
        if (Map* map = sMapMgr->FindBaseMap(camp.map))
        {
            float const g = map->GetHeight(PHASEMASK_NORMAL, ax, ay,
                camp.z + 10.0f, true, MAX_FALL_DISTANCE);
            if (g > INVALID_HEIGHT && std::fabs(g - camp.z) < 15.0f)
                az = g;
        }

        if (!me->TeleportTo(camp.map, ax, ay, az,
                std::atan2(camp.y - ay, camp.x - ax)))
        {
            handler->SendSysMessage(
                "현재 상태에서는 야영지로 이동할 수 없습니다.");
            return true;
        }

        g_goCooldown[accountId] = now + CAMP_GO_COOLDOWN_SECONDS;
        handler->PSendSysMessage("{}의 야영지로 이동합니다.", name);
        return true;
    }

    // Who else is camped near you. Discovery, so the neighbourhood is
    // something you can stumble into rather than something you have to be
    // told about.
    static bool HandleCampListCommand(ChatHandler* handler)
    {
        Player* me = nullptr;
        if (!Gate(handler, me))
            return true;

        std::lock_guard<std::mutex> lock(g_campMutex);

        if (g_camps.empty())
        {
            handler->SendSysMessage("아직 생성된 야영지가 없습니다. |cffffff00.camp claim|r으로 첫 야영지를 만들어 보세요.");
            return true;
        }

        // Nearest first, same-map only. A list of camps on other continents
        // is trivia; a list of your neighbours is useful.
        uint32 const mapId = me->GetMapId();
        // ⚠️ NOT `near`. Windows' windef.h still #defines `near` and `far`
        // as empty macros for 16-bit segmented memory, so `std::vector<...> near;`
        // silently becomes `std::vector<...> ;` and every error lands on the
        // lines AFTER the real one.
        std::vector<std::pair<float, Camp>> nearby;
        for (Camp const& c : g_camps)
        {
            if (c.map != mapId)
                continue;
            nearby.emplace_back(Dist2D(c.x, c.y, me->GetPositionX(),
                me->GetPositionY()), c);
        }

        std::sort(nearby.begin(), nearby.end(),
            [](auto const& a, auto const& b) { return a.first < b.first; });

        handler->PSendSysMessage("이 대륙의 야영지 {}개, 서버 전체 {}개:",
            uint32(nearby.size()), uint32(g_camps.size()));

        uint32 shown = 0;
        for (auto const& [dist, c] : nearby)
        {
            if (shown++ >= 8)
                break;

            // The account's most recently played character is the name its
            // owner is most likely to be known by.
            std::string owner = "someone";
            if (QueryResult r = CharacterDatabase.Query(
                    "SELECT name FROM characters WHERE account = {} "
                    "ORDER BY logout_time DESC LIMIT 1", c.accountId))
                owner = r->Fetch()[0].Get<std::string>();

            char const* zone = "somewhere";
            if (AreaTableEntry const* area =
                    sAreaTableStore.LookupEntry(c.zoneId))
                zone = area->area_name[0];

            handler->PSendSysMessage("  |cffffff00{}|r - {} (거리 {}미터)",
                owner, zone, uint32(dist));
        }

        if (nearby.size() > shown)
            handler->PSendSysMessage("  ...외 {}개.",
                uint32(nearby.size() - shown));

        handler->SendSysMessage("|cffffff00.camp visit <name>|r으로 방문할 수 있습니다.");
        return true;
    }

    // "Your alts turning up at the camp" - the third thing AeonFlux asked for,
    // after the land and the furniture.
    //
    // Your other characters are logged in as playerbots and gathered round the
    // fire. Run it once to wake them, once more to bring them in; it says which
    // it just did.
    //
    // 🛑 THE DISPATCH ROUTE IS NOT A STYLE CHOICE. Bots are added by pushing
    // `.playerbots bot add <name>` through the PLAYER'S OWN SESSION, exactly as
    // wowlegends_companion and wowlegends_gear already do. Two reasons, and
    // both have already cost this project a crash:
    //
    //   1. Spinning up a playerbot constructs an AI Engine. Doing that OUTSIDE
    //      the world tick is the mod-playerbots issue #2474 use-after-free
    //      class. ChatHandler::ParseCommands runs in-tick, so it is immune.
    //   2. It keeps mod-wowlegends free of mod-playerbots headers and link
    //      order, which is the only reason these two modules still build
    //      independently.
    //
    // ⚠️ And it must be a COMMAND, never the proximity hook. OnPlayerUpdate
    // runs inside Map::Update across six threads here; adding bots from there
    // would be the same crash with extra steps.
    static bool HandleCampAltsCommand(ChatHandler* handler)
    {
        Player* me = nullptr;
        if (!Gate(handler, me))
            return true;

        std::lock_guard<std::mutex> lock(g_campMutex);

        uint32 const accountId = me->GetSession()->GetAccountId();
        Camp const* found = FindCamp(accountId);
        if (!found)
        {
            handler->SendSysMessage("모일 야영지가 없습니다. 먼저 |cffffff00.camp claim|r으로 생성하세요.");
            return true;
        }
        Camp const camp = *found;

        if (me->GetMapId() != camp.map ||
            Dist2D(camp.x, camp.y, me->GetPositionX(), me->GetPositionY())
                > CAMP_RADIUS)
        {
            handler->SendSysMessage("먼저 야영지 안으로 이동하세요: |cffffff00.camp go|r.");
            return true;
        }

        // One command now: the queue below is drained by the seater in
        // WorldScript::OnUpdate, which wakes the offline alts, watches each
        // one actually ARRIVE in the world, and seats it round the fire -
        // ungrouped, follow stood down, parked so the idle strolls anchor to
        // the camp instead of to you. The old "run it twice" dance existed
        // only because this command used to return before the logins landed.
        EnqueueAltGather(me, accountId, /*wakeDelay*/ 0);

        if (g_altGathers.empty() ||
            g_altGathers.back().accountId != accountId)
        {
            handler->SendSysMessage("이 서버에 같은 계정의 다른 캐릭터가 없습니다.");
            return true;
        }

        handler->SendSysMessage("전투단을 부르고 있습니다. 잠시 후 모닥불 주변에 모입니다.");
        LOG_INFO("server", "[warbandcamp] account {} ({}) queued an alt "
            "gathering ({} names)", accountId, me->GetName(),
            g_altGathers.back().names.size());
        return true;
    }

    // GM tool, not a player command. Wipes your camp and lays the ENTIRE
    // catalogue out in a grid, one of everything, so it can be photographed
    // for the website.
    //
    // It exists because the props are invisible to the outside world in a very
    // literal sense: they are decorative doodads, and Wowhead's object
    // database does not carry them (checked - Campfire 182059, Alliance Tent
    // 201868 and Wagon 188696 all 404, while a gameplay object like Peacebloom
    // 1618 resolves fine). There is no third-party page to link a player to.
    // Our own screenshots are the only way anyone can see what "cookpot"
    // means, so making those easy to take is worth a command.
    //
    // ⚠️ The grid MUST fit inside CAMP_RADIUS. Props are spawned in the camp's
    // phase, and you are only phased into that camp while within 40 yards of
    // its CENTRE - so anything further out than that would spawn correctly,
    // stay in the table, and be invisible the moment you walked over to
    // photograph it. 6 x 6 at 10 yards puts the far corner at 35.4 yards,
    // which leaves room to stand next to the outermost prop and still see it.
    static bool HandleCampCatalogueCommand(ChatHandler* handler)
    {
        Player* me = nullptr;
        if (!Gate(handler, me))
            return true;

        std::lock_guard<std::mutex> lock(g_campMutex);

        uint32 const accountId = me->GetSession()->GetAccountId();
        Camp const* found = FindCamp(accountId);
        if (!found)
        {
            handler->SendSysMessage("먼저 평평하고 탁 트인 곳에서 야영지를 만드세요.");
            return true;
        }
        Camp const camp = *found;

        // ⚠️ Must be standing IN it, not merely on the same map. Spawning
        // needs the grid loaded, and what keeps a grid loaded is a player
        // being in it. Doing this from across the continent would push
        // objects at a grid that is not there.
        if (me->GetMapId() != camp.map ||
            Dist2D(camp.x, camp.y, me->GetPositionX(), me->GetPositionY())
                > CAMP_RADIUS)
        {
            handler->SendSysMessage("먼저 야영지 안으로 이동하세요: |cffffff00.camp go|r.");
            return true;
        }

        Map* map = me->GetMap();

        // Clear whatever is there, in the world and in the table.
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT id FROM wowlegends_warband_camp_object "
                "WHERE account_id = {}", accountId))
        {
            do
            {
                DespawnProp(map, r->Fetch()[0].Get<uint64>());
            }
            while (r->NextRow());
        }
        CharacterDatabase.DirectExecute(
            "DELETE FROM wowlegends_warband_camp_object WHERE account_id = {}",
            accountId);

        // ⚠️ MORE SLOTS THAN PROPS, SORTED CENTRE-OUTWARD, AND A FLATNESS GATE.
        //
        // The first version was a bare 6x6 grid with one slot per prop and no
        // check beyond "is there ground here". Run in Durotar it planted a
        // third of the catalogue halfway up a canyon wall - the probe found
        // real ground every time, because a cliff face IS ground.
        //
        // So: offer 49 candidate slots for 32 props, walk them nearest-first,
        // and reject any whose floor is more than CATALOGUE_MAX_DROP off the
        // camp's own level. Cliffs, ledges and pits fail that test; gentle
        // undulation passes. The catalogue then lands on whatever flat ground
        // exists nearby instead of decorating a rock face.
        //
        // 8 x 8 at 7 yards: the offset corner sits 3.5 * 7 * sqrt(2) = 34.6
        // yards out - inside CAMP_RADIUS, which it must be or the prop is
        // invisible the moment you walk over to photograph it. 64 slots for
        // 64 props; the earlier 7 x 7 grid had 49 and silently benched 15 of
        // the catalogue. 7 yards is tight for two tents side by side, but this
        // is a photography rig, not a home.
        constexpr float SPACING = 7.0f;
        constexpr int COLS = 8;                  // 8 x 8, corner-offset grid
        constexpr float CATALOGUE_MAX_DROP = 6.0f;

        struct Slot { float x, y, z, d; };
        std::vector<Slot> slots;
        for (int gx = 0; gx < COLS; ++gx)
        {
            for (int gy = 0; gy < COLS; ++gy)
            {
                float const x = camp.x + (float(gx) - 3.5f) * SPACING;
                float const y = camp.y + (float(gy) - 3.5f) * SPACING;

                // Probe from well above the camp floor so the ray finds the
                // real surface rather than starting underneath a slope.
                float const z = map->GetHeight(PHASEMASK_NORMAL, x, y,
                    camp.z + 20.0f, true, MAX_FALL_DISTANCE);
                if (z <= INVALID_HEIGHT)
                    continue;
                if (std::fabs(z - camp.z) > CATALOGUE_MAX_DROP)
                    continue;

                slots.push_back({ x, y, z, Dist2D(camp.x, camp.y, x, y) });
            }
        }

        // Nearest first, so a cramped site still fills the good middle.
        std::sort(slots.begin(), slots.end(),
            [](Slot const& a, Slot const& b) { return a.d < b.d; });

        uint32 placed = 0, skipped = 0;
        for (size_t i = 0; i < g_props.size(); ++i)
        {
            if (i >= slots.size())
            {
                ++skipped;
                continue;
            }

            float const x = slots[i].x;
            float const y = slots[i].y;
            float const z = slots[i].z;

            CharacterDatabase.DirectExecute(
                "INSERT INTO wowlegends_warband_camp_object "
                "(account_id, entry, pos_x, pos_y, pos_z, orientation) "
                "VALUES ({}, {}, {:.4f}, {:.4f}, {:.4f}, 0)",
                accountId, g_props[i].entry, x, y, z);

            uint64 id = 0;
            if (QueryResult r = CharacterDatabase.Query(
                    "SELECT id FROM wowlegends_warband_camp_object "
                    "WHERE account_id = {} ORDER BY id DESC LIMIT 1",
                    accountId))
                id = r->Fetch()[0].Get<uint64>();

            ObjectGuid const guid = SpawnProp(map, g_props[i].entry, x, y, z,
                0.0f, 1u << camp.phaseBit);
            if (guid && id)
            {
                g_liveProps[id] = guid;
                ++placed;
            }
            else
            {
                ++skipped;
            }
        }

        handler->PSendSysMessage("소품 목록 배치: {}개 설치, {}개 생략 (평탄도 검사를 통과한 위치 {}개).", placed, skipped,
            uint32(slots.size()));

        if (skipped)
            handler->SendSysMessage("모든 소품을 놓을 평평한 땅이 부족합니다. 멀고어처럼 더 평탄한 곳에서 시도하세요.");

        // Slots are filled nearest-first, so name them in that order: the
        // listing IS the layout, spiralling outward from where you stand.
        // Without this you are stood in front of 32 objects trying to
        // remember which barrel is the keg.
        handler->SendSysMessage("현재 위치에서 바깥쪽 순서:");
        std::string line;
        uint32 n = 0;
        for (size_t i = 0; i < g_props.size() && i < slots.size(); ++i)
        {
            line += g_props[i].label;
            if (++n % 5 == 0 || i + 1 == g_props.size() ||
                i + 1 == slots.size())
            {
                handler->SendSysMessage(line);
                line.clear();
            }
            else
            {
                line += ", ";
            }
        }

        LOG_INFO("server", "[warbandcamp] account {} ({}) laid out the "
            "catalogue: {} placed, {} skipped", accountId, me->GetName(),
            placed, skipped);
        return true;
    }

    static bool HandleCampRemoveCommand(ChatHandler* handler)
    {
        Player* me = nullptr;
        if (!Gate(handler, me))
            return true;

        std::lock_guard<std::mutex> lock(g_campMutex);

        uint32 const accountId = me->GetSession()->GetAccountId();
        Camp const* found = FindCamp(accountId);
        if (!found)
        {
            handler->SendSysMessage("야영지가 없습니다.");
            return true;
        }
        Camp const camp = *found;

        if (me->GetMapId() != camp.map ||
            Dist2D(camp.x, camp.y, me->GetPositionX(), me->GetPositionY())
                > CAMP_RADIUS)
        {
            handler->SendSysMessage("자신의 야영지 안에 있어야 합니다.");
            return true;
        }

        QueryResult r = CharacterDatabase.Query(
            "SELECT id, entry, pos_x, pos_y FROM wowlegends_warband_camp_object "
            "WHERE account_id = {}", accountId);
        if (!r)
        {
            handler->SendSysMessage("아직 설치한 소품이 없습니다.");
            return true;
        }

        // Nearest one to the player - the natural reading of "take that down"
        // when you are stood in front of it.
        uint64 bestId = 0;
        uint32 bestEntry = 0;
        float bestDist = 10.0f;
        do
        {
            Field* f = r->Fetch();
            float const d = Dist2D(f[2].Get<float>(), f[3].Get<float>(),
                me->GetPositionX(), me->GetPositionY());
            if (d < bestDist)
            {
                bestDist = d;
                bestId = f[0].Get<uint64>();
                bestEntry = f[1].Get<uint32>();
            }
        }
        while (r->NextRow());

        if (!bestId)
        {
            handler->SendSysMessage("철거하려는 소품에 더 가까이 이동하세요.");
            return true;
        }

        DespawnProp(me->GetMap(), bestId);
        CharacterDatabase.DirectExecute(
            "DELETE FROM wowlegends_warband_camp_object WHERE id = {}", bestId);

        char const* label = "It";
        for (PropDef const& def : g_props)
            if (def.entry == bestEntry)
                label = def.label;

        handler->PSendSysMessage("|cffffff00{}|r을 철거했습니다.", label);
        return true;
    }
};

class WowLegendsWarbandCampPlayer : public PlayerScript
{
public:
    WowLegendsWarbandCampPlayer()
        : PlayerScript("WowLegendsWarbandCampPlayer",
            { PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_LOGOUT,
              PLAYERHOOK_ON_MAP_CHANGED, PLAYERHOOK_ON_LOGIN })
    {
    }

    // ⚠️ This runs for EVERY player object every tick, and on this realm that
    // is thousands of bots. The bot test is first and is two pointer
    // dereferences; the real work is behind a one-second timer and a walk of
    // an in-memory vector. Nothing here touches the database unless a player
    // actually crosses into a camp.
    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!g_enabled.load() || !WlIsRealPlayer(player))
            return;

        std::lock_guard<std::mutex> lock(g_campMutex);

        PlayerCampState& st = g_playerState[player->GetGUID()];
        if (st.timer > diff)
        {
            st.timer -= diff;
            return;
        }
        st.timer = CAMP_PROXIMITY_INTERVAL_MS;

        UpdatePlayerCampPhase(player, st);
    }

    // A teleport out of a camp has to drop the phase immediately rather than
    // waiting for the next tick - the player would otherwise arrive at the
    // other end still carrying a camp bit.
    void OnPlayerMapChanged(Player* player) override
    {
        if (!WlIsRealPlayer(player))
            return;

        std::lock_guard<std::mutex> lock(g_campMutex);
        auto const it = g_playerState.find(player->GetGUID());
        if (it != g_playerState.end())
            ClearCampPhase(player, it->second);
    }

    void OnPlayerLogout(Player* player) override
    {
        std::lock_guard<std::mutex> lock(g_campMutex);
        g_playerState.erase(player->GetGUID());
        // Covers bots too: a parked alt that logs out (owner logged off,
        // owner logged INTO it as a real character - which displaces the bot
        // session - or a GM kicked it) must not linger in the registry.
        g_parkedAlts.erase(player->GetGUID().GetCounter());
    }

    // The camp comes alive on its own: log in with a camp claimed and your
    // alts are woken and gathered without a command. Kneuma's ask, 2026-08-07
    // - "better than manually every time a player joins the server".
    //
    // ⚠️ The wake is DELAYED five seconds via the gather queue, not dispatched
    // here. Cross-module login-hook order is undefined, so at this instant
    // mod-playerbots may not have built this player's PlayerbotMgr yet - and
    // `.playerbots bot add` through a session with no mgr refuses silently.
    // The seater dispatches once the delay passes, by which point the mgr has
    // long existed.
    void OnPlayerLogin(Player* player) override
    {
        if (!g_enabled.load() || !WlIsRealPlayer(player))
            return;

        // 🛑 Hold the first proximity check back past the login handshake.
        // PlayerCampState::timer defaults to 0, so it used to fire on the very
        // next tick - while the client is still arriving. SetPhaseMask's
        // visibility update is thrown away by that client, but the mask is now
        // CORRECT server-side, so UpdatePlayerCampPhase's "already correct"
        // early-out means it never retries. Net effect: log out inside your own
        // camp and you log back in to bare ground, permanently, until you walk
        // out and back in and a fresh mask transition redraws it.
        // ⚠️ Nothing is ever lost here - the props sit untouched in
        // wowlegends_warband_camp_object the whole time; they are simply never
        // sent. Reported by Micha 2026-08-10 (he checked the table before
        // rebuilding, which is the only reason it was caught).
        // Same family as the origin-restore teleport below, same cure: wait.
        {
            std::lock_guard<std::mutex> lock(g_campMutex);
            g_playerState[player->GetGUID()].timer = CAMP_LOGIN_PHASE_DELAY_MS;
        }

        // A parked alt saved position IS the camp - the seater teleported
        // it there. A human logging into that character expects to be where
        // they last PLAYED it, so if an origin row exists and the character
        // is still sitting at the account camp, put them back. If they are
        // FAR from the camp the alt has clearly adventured since parking
        // (a whispered `follow`), and the origin is stale - drop it and keep
        // the newer life. Runs regardless of AutoAlts: rows written while it
        // was on must still restore after an owner turns it off.
        uint32 const lowGuid = player->GetGUID().GetCounter();
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT map, pos_x, pos_y, pos_z, orientation "
                "FROM wowlegends_warband_alt_origin WHERE guid = {}", lowGuid))
        {
            bool restore = false;
            {
                std::lock_guard<std::mutex> lock(g_campMutex);
                g_parkedAlts.erase(lowGuid);   // a human owns this body now
                if (Camp const* c =
                        FindCamp(player->GetSession()->GetAccountId()))
                    restore = c->map == player->GetMapId() &&
                        Dist2D(c->x, c->y, player->GetPositionX(),
                            player->GetPositionY()) < CAMP_RADIUS + 20.0f;
            }
            CharacterDatabase.Execute(
                "DELETE FROM wowlegends_warband_alt_origin WHERE guid = {}",
                lowGuid);
            if (restore)
            {
                // 🛑 NOT inline. A TeleportTo issued during the login
                // handshake is silently lost - near or far, the teleport
                // races the client's initial spawn sequence and the spawn
                // wins, so the player "restores" to exactly where they
                // already were. Kneuma reproduced it on day one: camp in
                // Howling Fjord, hearth elsewhere, login landed at the camp
                // every time. A short delay lets the client finish arriving;
                // then the teleport is ordinary and sticks on both paths.
                // Guid captured, resolved at fire - never hold the Player*.
                Field* f = r->Fetch();
                uint32 const map = f[0].Get<uint32>();
                float const x = f[1].Get<float>();
                float const y = f[2].Get<float>();
                float const z = f[3].Get<float>();
                float const o = f[4].Get<float>();
                ObjectGuid const pg = player->GetGUID();
                player->m_Events.AddEventAtOffset([pg, map, x, y, z, o]()
                {
                    Player* p = ObjectAccessor::FindPlayer(pg);
                    if (!p || !p->IsInWorld() || p->IsBeingTeleported())
                        return;
                    if (p->TeleportTo(map, x, y, z, o))
                        ChatHandler(p->GetSession()).SendSysMessage(
                            "야영지를 떠나기 전 위치로 돌아왔습니다.");
                }, Milliseconds(2500));
            }
        }

        if (!g_autoAlts.load())
            return;

        std::lock_guard<std::mutex> lock(g_campMutex);
        if (!FindCamp(player->GetSession()->GetAccountId()))
            return;

        EnqueueAltGather(player, player->GetSession()->GetAccountId(),
            /*wakeDelay*/ 5);
    }
};

class WowLegendsWarbandCampWorld : public WorldScript
{
public:
    WowLegendsWarbandCampWorld()
        : WorldScript("WowLegendsWarbandCampWorld",
            { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_STARTUP,
              WORLDHOOK_ON_UPDATE })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_enabled = sConfigMgr->GetOption<bool>(
            "WowLegends.WarbandCamp.Enabled", false);
        g_autoAlts = sConfigMgr->GetOption<bool>(
            "WowLegends.WarbandCamp.AutoAlts", true);
        g_maxProps = sConfigMgr->GetOption<uint32>(
            "WowLegends.WarbandCamp.MaxProps", 200);
        float view = sConfigMgr->GetOption<float>(
            "WowLegends.WarbandCamp.ViewDistance", 40.0f);
        if (view != 0.0f)
            view = std::clamp(view, 20.0f, 250.0f);
        g_viewDist = view;
        g_altsSameFactionOnly = sConfigMgr->GetOption<bool>(
            "WowLegends.WarbandCamp.AltsSameFactionOnly", true);
    }

    // THE SEATER. Drains the gather queue: wakes offline alts once their
    // delay passes, then watches each name actually arrive in the world and
    // seats it. Runs on the world thread (never concurrent with the map
    // threads - World::Update runs hooks and map updates in sequence), every
    // 2 seconds, and touches nothing when the queue is empty.
    void OnUpdate(uint32 diff) override
    {
        m_seaterTimer += diff;
        if (m_seaterTimer < 2000)
            return;
        m_seaterTimer = 0;

        if (!g_enabled.load())
            return;

        std::lock_guard<std::mutex> lock(g_campMutex);
        if (g_altGathers.empty())
            return;

        time_t const now = time(nullptr);
        for (auto it = g_altGathers.begin(); it != g_altGathers.end();)
        {
            AltGather& g = *it;
            Player* owner = ObjectAccessor::FindPlayer(g.owner);
            Camp const* camp = FindCamp(g.accountId);

            // Owner gone, camp gone, or out of patience: stop. Anything
            // already seated stays seated - only the outstanding work dies.
            if (!owner || !camp || now > g.deadline)
            {
                it = g_altGathers.erase(it);
                continue;
            }

            if (g.wakeAt)
            {
                if (now < g.wakeAt)
                {
                    ++it;
                    continue;
                }

                // Wake whoever is not in the world yet. Dispatched through
                // the OWNER's session exactly like the command version - the
                // in-tick ParseCommands path that mod-playerbots' Engine
                // rules require (#2474).
                uint32 woken = 0;
                for (std::string const& name : g.names)
                {
                    if (!ObjectAccessor::FindPlayerByName(name, true))
                    {
                        ChatHandler(owner->GetSession()).ParseCommands(
                            ".playerbots bot add " + name);
                        ++woken;
                    }
                }
                g.wakeAt = 0;

                if (!g.announced && woken)
                {
                    ChatHandler(owner->GetSession()).SendSysMessage(
                        "전투단이 하나둘 움직이기 시작합니다...");
                    g.announced = true;
                }
                ++it;
                continue;
            }

            // Seat everyone who has arrived since the last tick.
            for (auto nameIt = g.names.begin(); nameIt != g.names.end();)
            {
                Player* alt = ObjectAccessor::FindPlayerByName(*nameIt, true);
                if (!alt)
                {
                    ++nameIt;
                    continue;
                }

                // The owner logged into this character themselves, or it is
                // being played for real - not ours to move.
                if (!alt->GetSession() || !alt->GetSession()->IsBot())
                {
                    nameIt = g.names.erase(nameIt);
                    continue;
                }

                if (SeatAltAtCamp(alt, *camp, g.seated))
                    ++g.seated;
                nameIt = g.names.erase(nameIt);
            }

            if (g.names.empty())
            {
                if (g.seated)
                    ChatHandler(owner->GetSession()).PSendSysMessage(
                        "전투단원 {}명이 모닥불가에 자리를 잡았습니다.",
                        g.seated);
                it = g_altGathers.erase(it);
                continue;
            }
            ++it;
        }
    }

private:
    uint32 m_seaterTimer = 0;

public:
    void OnStartup() override
    {
        // Runtime DDL, because a shipped .sql would never run: this repack
        // sets Updates.EnableDatabases = 0, so the worldserver applies no SQL
        // at all. Same "zero manual SQL" pattern as wowlegends_companion.
        CharacterDatabase.DirectExecute(
            "CREATE TABLE IF NOT EXISTS wowlegends_warband_camp ("
            "account_id INT UNSIGNED NOT NULL, "
            "map SMALLINT UNSIGNED NOT NULL, "
            "pos_x FLOAT NOT NULL, "
            "pos_y FLOAT NOT NULL, "
            "pos_z FLOAT NOT NULL, "
            "orientation FLOAT NOT NULL, "
            "phase_bit TINYINT UNSIGNED NOT NULL, "
            "zone_id INT UNSIGNED NOT NULL DEFAULT 0, "
            "claimed_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
            "PRIMARY KEY (account_id), "
            "KEY idx_map (map)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
            "COLLATE=utf8mb4_unicode_ci");

        CharacterDatabase.DirectExecute(
            // Where a parked alt REALLY lives. The seater teleports alts to
            // the camp, which silently becomes their saved position - so a
            // human logging into that character spawned at the camp instead
            // of wherever they actually left off (Kneuma, day-one feedback).
            // Row written when an alt is first parked, kept until a REAL
            // login near the camp restores it (or a login far away proves
            // the alt has since adventured, which just deletes it).
            "CREATE TABLE IF NOT EXISTS wowlegends_warband_alt_origin ("
            "  guid INT UNSIGNED NOT NULL PRIMARY KEY,"
            "  map INT UNSIGNED NOT NULL,"
            "  pos_x FLOAT NOT NULL, pos_y FLOAT NOT NULL,"
            "  pos_z FLOAT NOT NULL, orientation FLOAT NOT NULL,"
            "  parked_at INT UNSIGNED NOT NULL"
            ") ENGINE=InnoDB",
            "CREATE TABLE IF NOT EXISTS wowlegends_warband_camp_object ("
            "id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
            "account_id INT UNSIGNED NOT NULL, "
            "entry INT UNSIGNED NOT NULL, "
            "pos_x FLOAT NOT NULL, "
            "pos_y FLOAT NOT NULL, "
            "pos_z FLOAT NOT NULL, "
            "orientation FLOAT NOT NULL, "
            "placed_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP, "
            "PRIMARY KEY (id), "
            "KEY idx_account (account_id)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
            "COLLATE=utf8mb4_unicode_ci");

        // 🛑 v1 SHIPPED A `plot` COLUMN AND A UNIQUE KEY ON IT. Anyone
        // upgrading from a PTR build that ran the old schema has a table whose
        // `plot` is NOT NULL with no default, and every insert here would fail
        // with "field doesn't have a default value" - a feature that silently
        // refuses every claim. The old column is meaningless now: it indexed a
        // plot grid on a map we no longer use.
        if (CharacterDatabase.Query("SHOW COLUMNS FROM wowlegends_warband_camp "
                "LIKE 'plot'"))
        {
            LOG_WARN("server", "[warbandcamp] dropping the v1 `plot` column "
                "(camps are placed freely now)");

            // The v1 rows all pointed at map 37 plots that no longer mean
            // anything, so there is nothing to migrate - only to clear.
            CharacterDatabase.DirectExecute(
                "DELETE FROM wowlegends_warband_camp");
            CharacterDatabase.DirectExecute(
                "ALTER TABLE wowlegends_warband_camp DROP INDEX uk_plot");
            CharacterDatabase.DirectExecute(
                "ALTER TABLE wowlegends_warband_camp DROP COLUMN plot");

            // Guarded separately: a half-finished migration from an earlier
            // boot would already have these, and ALTER ADD on an existing
            // column is an error, not a no-op.
            if (!CharacterDatabase.Query("SHOW COLUMNS FROM "
                    "wowlegends_warband_camp LIKE 'phase_bit'"))
                CharacterDatabase.DirectExecute(
                    "ALTER TABLE wowlegends_warband_camp ADD COLUMN phase_bit "
                    "TINYINT UNSIGNED NOT NULL DEFAULT 1");

            if (!CharacterDatabase.Query("SHOW COLUMNS FROM "
                    "wowlegends_warband_camp LIKE 'zone_id'"))
                CharacterDatabase.DirectExecute(
                    "ALTER TABLE wowlegends_warband_camp ADD COLUMN zone_id "
                    "INT UNSIGNED NOT NULL DEFAULT 0");
        }

        BuildPropCatalogue();

        // 🛑 NO ORPHAN SWEEP. The obvious one - delete rows whose account_id
        // has no row in `characters` - is WRONG, and it destroys real players'
        // data: "this account has no character ON THIS REALM" is not "this
        // account was deleted". Anyone who deletes their last character to
        // reroll, or who plays on another realm of the same install, would
        // silently lose their camp on the next restart.

        std::lock_guard<std::mutex> lock(g_campMutex);
        g_camps.clear();
        if (QueryResult r = CharacterDatabase.Query(
                "SELECT account_id, map, pos_x, pos_y, pos_z, orientation, "
                "phase_bit, zone_id FROM wowlegends_warband_camp"))
        {
            do
            {
                Field* f = r->Fetch();
                Camp c;
                c.accountId = f[0].Get<uint32>();
                c.map = f[1].Get<uint16>();
                c.x = f[2].Get<float>();
                c.y = f[3].Get<float>();
                c.z = f[4].Get<float>();
                c.o = f[5].Get<float>();
                c.phaseBit = f[6].Get<uint8>();
                c.zoneId = f[7].Get<uint32>();
                g_camps.push_back(c);
            }
            while (r->NextRow());
        }

        // Props are NOT spawned here. They come up the first time somebody
        // walks into the camp, which keeps a realm with a thousand camps from
        // paying for all of them at boot and keeps grids that nobody visits
        // empty.
        LOG_INFO("server", "[warbandcamp] {} camps loaded", g_camps.size());
    }
};

void AddWowLegendsWarbandCampScripts()
{
    new WowLegendsWarbandCampCommand();
    new WowLegendsWarbandCampPlayer();
    new WowLegendsWarbandCampWorld();
}
