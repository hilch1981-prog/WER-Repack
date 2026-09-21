/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - World PvP (all-zones open PvP, with optional timed windows)
 *
 * With WowLegends.WorldPvP.Enabled on, every non-GM player is held PvP-flagged
 * everywhere, so opposite-faction players - and the AI playerbots, which are
 * players too - are attackable out in the world. Same-faction stays safe
 * (faction reaction is untouched), GMs are excluded, and capitals / sanctuaries
 * stay safe (open world only).
 *
 * Modes (WowLegends.WorldPvP.Mode):
 *   - "always" (default): PvP is on continuously while Enabled.
 *   - "timed": PvP turns on for a window (Timed.DurationMinutes) every
 *     Timed.IntervalMinutes, with a server-wide announce as each window opens
 *     and closes. A GM can also open/close a window on demand with
 *     `.worldpvp start [minutes] | stop` (and check `.worldpvp status`).
 * Enabled = 0 is the off switch. Default OFF.
 *
 * Mechanism: force the real PvP byte via Player::UpdatePvP(true, true) and keep
 * pvpInfo.EndTimer at 0 so the core's 5-minute PvP timeout never lapses it. We
 * do NOT set PLAYER_FLAGS_IN_PVP, so a player's own /pvp toggle is never
 * disturbed; when PvP turns off (feature disabled, mode change, or a timed
 * window closing) we start the normal timeout on the players we were holding (a
 * one-time pass), so PvP lapses cleanly. The OnPlayerIsPvP hook keeps
 * Player::IsPvP() consistent for the client (red flag) and the playerbot
 * enemy-target value; the core attack gate reads the byte flag we set.
 *
 * g_active is the single "PvP on right now" flag everything keys off (incl. the
 * mod-playerbots leaf edits via WlWorldPvpEnabled()); in timed mode it tracks
 * the current window. All player-touching runs on the world thread (OnUpdate /
 * OnAfterConfigLoad); the .worldpvp command only flips atomics drained there.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "DBCStores.h"
#include "Map.h"
#include "Configuration/Config.h"
#include "GameTime.h"
#include "StringFormat.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <string>

using namespace Acore::ChatCommands;

namespace
{
    std::atomic<bool> g_masterEnabled{false};   // WorldPvP.Enabled
    std::atomic<bool> g_timedMode{false};       // Mode == "timed"
    std::atomic<bool> g_active{false};          // PvP forced on right now
    std::atomic<bool> g_announce{true};
    std::atomic<uint32> g_intervalMs{120 * 60 * 1000};
    std::atomic<uint32> g_durationMs{30 * 60 * 1000};

    // Scheduler timeline (world-thread driven; atomic so `.worldpvp status` can
    // read it from the command thread without tearing).
    std::atomic<uint64> g_nextWindowMs{0};
    std::atomic<uint64> g_windowEndsMs{0};

    // `.worldpvp` command -> world thread.
    std::atomic<bool> g_pendingOpen{false};
    std::atomic<bool> g_pendingClose{false};
    std::atomic<uint32> g_manualDurationMs{0};  // 0 = use Timed.DurationMinutes

    bool IsOutdoorPvpEligible(Player* player)
    {
        if (player->IsGameMaster() || player->GetLevel() < 10 || !player->GetMap() || player->GetMap()->Instanceable())
            return false;
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(player->GetAreaId());
        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(player->GetZoneId());
        auto safe = [](AreaTableEntry const* entry)
        {
            return entry && (entry->IsSanctuary() ||
                (entry->flags & (AREA_FLAG_CAPITAL | AREA_FLAG_SLAVE_CAPITAL | AREA_FLAG_SLAVE_CAPITAL2)));
        };
        return !safe(area) && !safe(zone);
    }

    void ApplyWorldPvP(Player* player)
    {
        if (!IsOutdoorPvpEligible(player))
            return;

        // Set the flag and keep EndTimer at 0 so the 5-minute PvP timeout never
        // lapses it. No PLAYER_FLAGS_IN_PVP, so the player's /pvp is untouched.
        if (!player->IsPvP() || player->pvpInfo.EndTimer != 0)
            player->UpdatePvP(true, true);
    }

    void ReleaseWorldPvP(Player* player)
    {
        // Release only players WE were holding (flagged, no timeout running,
        // and not via their own /pvp). Starting EndTimer lets the normal
        // 5-minute timeout clear the flag.
        if (player->IsPvP() && player->pvpInfo.EndTimer == 0 &&
            !player->HasPlayerFlag(PLAYER_FLAGS_IN_PVP))
            player->pvpInfo.EndTimer = GameTime::GetGameTime().count();
    }

    void Broadcast(std::string const& msg)
    {
        ChatHandler(nullptr).SendGlobalSysMessage(msg.c_str());
    }

    void ApplyToAll()
    {
        for (auto const& pair : ObjectAccessor::GetPlayers())
            if (Player* player = pair.second)
                ApplyWorldPvP(player);
    }

    void ReleaseAll()
    {
        for (auto const& pair : ObjectAccessor::GetPlayers())
            if (Player* player = pair.second)
                ReleaseWorldPvP(player);
    }

    // Flip the live PvP state, applying/releasing the flag on everyone online
    // and (optionally) announcing. World-thread only. No-op if unchanged.
    void SetActive(bool active, bool announce)
    {
        if (g_active.exchange(active) == active)
            return;

        if (active)
        {
            ApplyToAll();
            if (announce && g_announce.load())
            {
                uint64 const now = uint64(GameTime::GetGameTimeMS().count());
                uint64 const ends = g_windowEndsMs.load();
                uint32 const mins = ends > now
                    ? uint32((ends - now + 59999) / 60000)
                    : g_durationMs.load() / 60000;
                Broadcast(Acore::StringFormat(
                    "|cffff2020[월드 PvP]|r 지금부터 {}분 동안 야외 PvP가 활성화됩니다. 적 진영과 전투할 수 있습니다!",
                    mins));
            }
        }
        else
        {
            ReleaseAll();
            if (announce && g_announce.load())
                Broadcast("|cff20ff20[월드 PvP]|r 이번 전투 시간이 끝났습니다. 다음 시작까지 안전하게 이동하세요.");
        }
    }
}

// Exposed to the mod-playerbots leaf edits (PossibleTargetsValue.cpp,
// AiFactory.cpp) via a plain `extern bool WlWorldPvpEnabled();` forward decl,
// so all-zones PvP + bot target priority follow the live World PvP state (timed
// mode that means only while a window is open).
bool WlWorldPvpEnabled()
{
    return g_active.load();
}

class WowLegendsWorldPvpWorld : public WorldScript
{
public:
    WowLegendsWorldPvpWorld() : WorldScript("WowLegendsWorldPvpWorld",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_UPDATE })
    {
    }

    // Cache the config once per (re)load instead of polling on the hot hooks,
    // then reconcile the live state. Silent - a config event is not a scheduled
    // window edge.
    void OnAfterConfigLoad(bool /*reload*/) override
    {
        bool const wasActive = g_active.load();

        g_masterEnabled = sConfigMgr->GetOption<bool>(
            "WowLegends.WorldPvP.Enabled", false);

        std::string mode = sConfigMgr->GetOption<std::string>(
            "WowLegends.WorldPvP.Mode", "always");
        for (char& c : mode)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        g_timedMode = (mode == "timed");

        g_announce = sConfigMgr->GetOption<bool>(
            "WowLegends.WorldPvP.Timed.Announce", true);

        uint32 interval = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldPvP.Timed.IntervalMinutes", 120);
        uint32 duration = sConfigMgr->GetOption<uint32>(
            "WowLegends.WorldPvP.Timed.DurationMinutes", 30);
        interval = std::clamp<uint32>(interval, 1, 10080);  // 1 min .. 1 week
        duration = std::clamp<uint32>(duration, 1, 1440);   // 1 min .. 1 day
        g_intervalMs = interval * 60000;
        g_durationMs = duration * 60000;

        // A queued `.worldpvp` command must not outlive a mode/Enabled change.
        g_pendingOpen = false;
        g_pendingClose = false;
        g_manualDurationMs = 0;

        if (!g_masterEnabled.load())
        {
            if (wasActive)
                SetActive(false, false);
        }
        else if (!g_timedMode.load())
        {
            SetActive(true, false);                 // always-on
        }
        else
        {
            // Timed: start cold and let the scheduler open the first window.
            if (wasActive)
                SetActive(false, false);
            g_nextWindowMs = 0;                     // lazy (re)init in OnUpdate
        }
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_masterEnabled.load())
        {
            if (g_active.load())
                SetActive(false, false);
            g_pendingOpen = false;          // drop any stale queued command
            g_pendingClose = false;
            return;
        }

        if (!g_timedMode.load())
        {
            if (!g_active.load())
                SetActive(true, false);
            g_pendingOpen = false;          // commands are timed-mode only
            g_pendingClose = false;
            Sweep(diff);
            return;
        }

        // --- timed mode ---
        uint64 const now = uint64(GameTime::GetGameTimeMS().count());

        // Only reschedule if a window was actually open (a stop between windows
        // must not push the next one out a full interval).
        if (g_pendingClose.exchange(false) && g_active.load())
        {
            SetActive(false, true);
            g_nextWindowMs = now + g_intervalMs.load();
        }
        if (g_pendingOpen.exchange(false))
        {
            uint32 dur = g_manualDurationMs.exchange(0);
            if (dur == 0)
                dur = g_durationMs.load();
            g_windowEndsMs = now + dur;
            SetActive(true, true);
        }

        if (g_active.load())
        {
            if (now >= g_windowEndsMs.load())
            {
                SetActive(false, true);
                g_nextWindowMs = now + g_intervalMs.load();
            }
            else
                Sweep(diff);
        }
        else
        {
            if (g_nextWindowMs.load() == 0)
                g_nextWindowMs = now + g_intervalMs.load();
            if (now >= g_nextWindowMs.load())
            {
                g_windowEndsMs = now + g_durationMs.load();
                SetActive(true, true);
            }
        }
    }

private:
    // Re-assert the flag on everyone a few times a minute: an idle playerbot in
    // a non-hostile zone lets the core lapse its PvP flag (UpdatePvPState) and
    // turns neutral/unattackable until it fights again. Runs only while active.
    void Sweep(uint32 diff)
    {
        m_sweepTimer += diff;
        if (m_sweepTimer < SWEEP_INTERVAL_MS)
            return;
        m_sweepTimer = 0;
        ApplyToAll();
    }

    static constexpr uint32 SWEEP_INTERVAL_MS = 5000;
    uint32 m_sweepTimer = 0;
};

class WowLegendsWorldPvpPlayer : public PlayerScript
{
public:
    WowLegendsWorldPvpPlayer() : PlayerScript("WowLegendsWorldPvpPlayer",
        { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_UPDATE, PLAYERHOOK_ON_IS_PVP })
    {
    }

    void OnPlayerLogin(Player* player) override
    {
        if (g_active.load())
            ApplyWorldPvP(player);
    }

    void OnPlayerIsPvP(Player* player, bool& result) override
    {
        if (g_active.load() && IsOutdoorPvpEligible(player))
            result = true;
    }

    void OnPlayerUpdate(Player* player, uint32 /*p_time*/) override
    {
        if (g_active.load())
            ApplyWorldPvP(player);
    }
};

class WowLegendsWorldPvpCommand : public CommandScript
{
public:
    WowLegendsWorldPvpCommand() : CommandScript("WowLegendsWorldPvpCommand")
    {
    }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable sub =
        {
            { "start",  HandleStart,  SEC_GAMEMASTER, Console::Yes },
            { "stop",   HandleStop,   SEC_GAMEMASTER, Console::Yes },
            { "status", HandleStatus, SEC_GAMEMASTER, Console::Yes },
        };
        static ChatCommandTable base =
        {
            { "worldpvp", sub },
        };
        return base;
    }

    static bool HandleStart(ChatHandler* handler, Optional<uint32> minutes)
    {
        if (!g_masterEnabled.load())
        {
            handler->SendSysMessage("월드 PvP가 꺼져 있습니다. WowLegends.WorldPvP.Enabled = 1로 설정하고 다시 불러오세요.");
            handler->SetSentErrorMessage(true);
            return true;
        }
        if (!g_timedMode.load())
        {
            handler->SendSysMessage("월드 PvP는 상시 모드(Mode = always)로 이미 활성화되어 있습니다.");
            return true;
        }

        uint32 m = minutes.value_or(0u);
        if (m > 1440)
            m = 1440;
        g_manualDurationMs = m * 60000;     // 0 -> Timed.DurationMinutes
        g_pendingClose = false;
        g_pendingOpen = true;

        if (m)
            handler->PSendSysMessage(
                "월드 PvP를 {}분 동안 엽니다.", m);
        else
            handler->SendSysMessage("월드 PvP를 엽니다.");
        return true;
    }

    static bool HandleStop(ChatHandler* handler)
    {
        if (!g_masterEnabled.load())
        {
            handler->SendSysMessage("월드 PvP가 이미 꺼져 있습니다.");
            return true;
        }
        if (!g_timedMode.load())
        {
            handler->SendSysMessage("월드 PvP가 상시 모드(Mode = always)입니다. 시간제로 운영하려면 Mode = timed로 바꾸세요.");
            return true;
        }

        g_pendingOpen = false;
        if (g_active.load())
        {
            g_pendingClose = true;
            handler->SendSysMessage("현재 월드 PvP 시간을 종료합니다.");
        }
        else
            handler->SendSysMessage("현재 월드 PvP 시간이 아닙니다.");
        return true;
    }

    static bool HandleStatus(ChatHandler* handler)
    {
        if (!g_masterEnabled.load())
        {
            handler->SendSysMessage("월드 PvP: 꺼짐 (WowLegends.WorldPvP.Enabled = 0).");
            return true;
        }

        bool const timed = g_timedMode.load();
        bool const active = g_active.load();
        handler->PSendSysMessage("월드 PvP: 켜짐, 모드 = {}.",
            timed ? "timed" : "always");

        if (!timed)
        {
            handler->SendSysMessage(active
                ? "Currently ACTIVE (always-on)."
                : "Currently inactive (initialising).");
            return true;
        }

        uint64 const now = uint64(GameTime::GetGameTimeMS().count());
        if (active)
        {
            uint64 const ends = g_windowEndsMs.load();
            uint32 const mins =
                ends > now ? uint32((ends - now + 59999) / 60000) : 0;
            handler->PSendSysMessage(
                "진행 중 - 약 {}분 남았습니다.", mins);
        }
        else
        {
            uint64 const next = g_nextWindowMs.load();
            uint32 const mins =
                next > now ? uint32((next - now + 59999) / 60000) : 0;
            handler->PSendSysMessage(
                "대기 중 - 약 {}분 뒤 시작합니다 ({}분 진행, {}분 간격).",
                mins, g_durationMs.load() / 60000, g_intervalMs.load() / 60000);
        }
        return true;
    }
};

void AddWowLegendsWorldPvpScripts()
{
    new WowLegendsWorldPvpWorld();
    new WowLegendsWorldPvpPlayer();
    new WowLegendsWorldPvpCommand();
}
