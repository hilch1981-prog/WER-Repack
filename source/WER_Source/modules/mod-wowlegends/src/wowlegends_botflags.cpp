/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - bot behaviour toggles shared with mod-playerbots.
 *
 * Holds the cached on/off switches for the playerbot combat features that are
 * implemented as small LEAF edits inside the (upstream) mod-playerbots module:
 *   - WowLegends.BotTaunts.Enabled          faction-aware combat taunts (SayAction)
 *   - WowLegends.SmartPvpTargeting.Enabled  healer-first PvP target choice
 *   - WowLegends.BotNarration.Enabled       quest-progress party chat (QuestAction)
 *   - WowLegends.BotPitchIn.Enabled/.Radius grouped bots fight their own quest
 *                                           mobs (GrindTargetValue, AiFactory)
 *   - WowLegends.BotIdleLife.Enabled        settled-master ambience: strolls,
 *                                           campfire circle, ambient emotes
 *   - WowLegends.BotChatter.Unsolicited     bots volunteer quest reports
 *                                           (PlayerbotAI::TellMasterNoFacing)
 *
 * The switches are read ONCE per config (re)load into atomics, exactly like
 * wowlegends_worldpvp.cpp. mod-playerbots reads them through the two extern
 * getters below, which the leaf .cpp files forward-declare locally
 * (plain `extern bool ...();`, no cross-module header), so the upstream module
 * needs zero WorldScript / config wiring.
 */

#include "ScriptMgr.h"
#include "Configuration/Config.h"
#include <algorithm>
#include <atomic>

namespace
{
    std::atomic<bool> g_botTauntsEnabled{false};
    std::atomic<bool> g_smartPvpEnabled{true};
    std::atomic<bool> g_botNarrationEnabled{true};
    std::atomic<bool> g_botPitchInEnabled{true};
    std::atomic<float> g_botPitchInRadius{40.0f};
    std::atomic<bool> g_botIdleLifeEnabled{true};
    std::atomic<bool> g_botGuideEnabled{true};
    std::atomic<bool> g_triageHealerEnabled{true};
    std::atomic<bool> g_voiceCardsEnabled{true};
    std::atomic<bool> g_speechGovernorEnabled{true};
    std::atomic<bool> g_botChatterUnsolicited{true};
}

// Exposed to the mod-playerbots leaf edits (SayAction.cpp, EnemyPlayerValue.cpp,
// QuestAction.cpp) via plain `extern bool ...();` forward declarations there.
bool WlBotTauntsEnabled()
{
    return g_botTauntsEnabled.load();
}

bool WlSmartPvpEnabled()
{
    return g_smartPvpEnabled.load();
}

bool WlBotNarrationEnabled()
{
    return g_botNarrationEnabled.load();
}

bool WlBotPitchInEnabled()
{
    return g_botPitchInEnabled.load();
}

float WlBotPitchInRadius()
{
    return g_botPitchInRadius.load();
}

bool WlBotIdleLifeEnabled()
{
    return g_botIdleLifeEnabled.load();
}

bool WlBotGuideEnabled()
{
    return g_botGuideEnabled.load();
}

bool WlTriageHealerEnabled()
{
    return g_triageHealerEnabled.load();
}

bool WlVoiceCardsEnabled()
{
    return g_voiceCardsEnabled.load();
}

bool WlSpeechGovernorEnabled()
{
    return g_speechGovernorEnabled.load();
}

// Read on the WHISPER path in PlayerbotAI::TellMasterNoFacing, which runs on
// every map-update thread. It must stay an atomic read: ConfigMgr's getter
// touches _configOptions without holding _configLock, and `.reload config`
// clears that map, so a per-whisper sConfigMgr lookup from six threads is a
// crash rather than a stale bool.
bool WlBotChatterUnsolicited()
{
    return g_botChatterUnsolicited.load();
}

class WowLegendsBotFlagsWorld : public WorldScript
{
public:
    WowLegendsBotFlagsWorld() : WorldScript("WowLegendsBotFlagsWorld",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        g_botChatterUnsolicited = sConfigMgr->GetOption<bool>(
            "WowLegends.BotChatter.Unsolicited", true);
        g_botTauntsEnabled = sConfigMgr->GetOption<bool>(
            "WowLegends.BotTaunts.Enabled", false);
        g_smartPvpEnabled = sConfigMgr->GetOption<bool>(
            "WowLegends.SmartPvpTargeting.Enabled", true);
        g_botNarrationEnabled = sConfigMgr->GetOption<bool>(
            "WowLegends.BotNarration.Enabled", true);
        g_botPitchInEnabled = sConfigMgr->GetOption<bool>(
            "WowLegends.BotPitchIn.Enabled", true);
        g_botPitchInRadius = std::clamp(sConfigMgr->GetOption<float>(
            "WowLegends.BotPitchIn.Radius", 40.0f), 10.0f, 100.0f);
        g_botIdleLifeEnabled = sConfigMgr->GetOption<bool>(
            "WowLegends.BotIdleLife.Enabled", true);
        g_botGuideEnabled = sConfigMgr->GetOption<bool>(
            "WowLegends.BotGuide.Enabled", true);
        g_triageHealerEnabled = sConfigMgr->GetOption<bool>(
            "WowLegends.TriageHealer.Enabled", true);
        g_voiceCardsEnabled = sConfigMgr->GetOption<bool>(
            "WowLegends.VoiceCards.Enabled", true);
        g_speechGovernorEnabled = sConfigMgr->GetOption<bool>(
            "WowLegends.SpeechGovernor.Enabled", true);
    }
};

void AddWowLegendsBotFlagsScripts()
{
    new WowLegendsBotFlagsWorld();
}
