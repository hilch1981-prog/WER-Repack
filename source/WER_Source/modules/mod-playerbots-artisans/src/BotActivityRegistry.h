#ifndef MOD_PLAYERBOT_BOT_ACTIVITY_REGISTRY_H
#define MOD_PLAYERBOT_BOT_ACTIVITY_REGISTRY_H

// Prefer the upstream Playerbots reservation bus when it is available.  The
// current official Playerbots master does not provide it, so the WOW Legends
// Batch1 integration falls back to the shared City/PvP Life reservation bus.
#if __has_include("BotActivityMgr.h")
#include "BotActivityMgr.h"

namespace BotActivity
{
    inline void Reserve(uint32 guidLow)
    {
        BotActivityMgr::TryReserve(guidLow, BotActivityMgr::Owner::Artisans);
    }

    inline void Release(uint32 guidLow)
    {
        BotActivityMgr::Release(guidLow, BotActivityMgr::Owner::Artisans);
    }

    inline bool IsReserved(uint32 guidLow)
    {
        return BotActivityMgr::IsReserved(guidLow);
    }
}
#elif __has_include("../../mod-playerbots-city-life/src/LifeBotReservation.h")
#include "../../mod-playerbots-city-life/src/LifeBotReservation.h"

namespace BotActivity
{
    inline void Reserve(uint32 guidLow)
    {
        PlayerbotsLife::TryReserve(guidLow, PlayerbotsLife::ReservationOwner::Artisans);
    }

    inline void Release(uint32 guidLow)
    {
        PlayerbotsLife::Release(guidLow, PlayerbotsLife::ReservationOwner::Artisans);
    }

    inline bool IsReserved(uint32 guidLow)
    {
        return PlayerbotsLife::IsReserved(guidLow);
    }
}
#else
#error "mod-playerbots-artisans requires BotActivityMgr or mod-playerbots-city-life"
#endif

#endif
