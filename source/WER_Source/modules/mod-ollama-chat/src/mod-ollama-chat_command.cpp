#include "mod-ollama-chat_command.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_sentiment.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_capability.h"
#include "mod-ollama-chat_dispatch.h"
#include "mod-ollama-chat_governor.h"
#include "mod-ollama-chat_response.h"
#include "mod-ollama-chat_roleplay.h"
#include "mod-ollama-chat-utilities.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include <thread>
#include "Chat.h"
#include "Config.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include <fmt/core.h>

using namespace Acore::ChatCommands;

OllamaChatConfigCommand::OllamaChatConfigCommand()
    : CommandScript("OllamaChatConfigCommand")
{
}

ChatCommandTable OllamaChatConfigCommand::GetCommands() const
{
    static ChatCommandTable ollamaSentimentCommandTable =
    {
        { "view",  HandleOllamaSentimentViewCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "set",   HandleOllamaSentimentSetCommand,   SEC_ADMINISTRATOR, Console::Yes },
        { "reset", HandleOllamaSentimentResetCommand, SEC_ADMINISTRATOR, Console::Yes }
    };

    static ChatCommandTable ollamaPersonalityCommandTable =
    {
        { "get",  HandleOllamaPersonalityGetCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "set",  HandleOllamaPersonalitySetCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "list", HandleOllamaPersonalityListCommand, SEC_ADMINISTRATOR, Console::Yes }
    };

    static ChatCommandTable ollamaReloadCommandTable =
    {
        { "reload",      HandleOllamaReloadCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "status",      HandleOllamaStatusCommand,  SEC_ADMINISTRATOR, Console::Yes },
        { "test",        HandleOllamaTestCommand,    SEC_ADMINISTRATOR, Console::Yes },
        { "sentiment",   ollamaSentimentCommandTable },
        { "personality", ollamaPersonalityCommandTable }
    };

    static ChatCommandTable commandTable =
    {
        { "ollama", ollamaReloadCommandTable }
    };

    return commandTable;
}

bool OllamaChatConfigCommand::HandleOllamaReloadCommand(ChatHandler* handler)
{
    sConfigMgr->Reload();
    LoadOllamaChatConfig();
    if (!g_Enable)
    {
        handler->SendSysMessage("OllamaChat 비활성: DB 로딩과 기능 검사를 생략했습니다. 활성화하려면 서버를 재시작하세요.");
        return true;
    }
    Roleplay_Load();

    // Re-probe: the operator may have just pointed the module at a different
    // model, and think-mode support is per-model.
    OllamaCapability_Init(true);

    // Clear personality assignments if RP personalities are disabled
    // This ensures that when re-enabled later, bots get fresh random assignments
    if (!g_EnableRPPersonalities)
    {
        ClearAllBotPersonalities();
    }

    LoadBotPersonalityList();
    LoadBotConversationHistoryFromDB();
    InitializeSentimentTracking();
    handler->SendSysMessage("OllamaChat 설정을 다시 불러왔습니다.");
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaSentimentViewCommand(ChatHandler* handler, Optional<std::string> botName, Optional<std::string> playerName)
{
    if (!g_EnableSentimentTracking)
    {
        handler->SendSysMessage("OllamaChat 감정 추적이 꺼져 있습니다.");
        return true;
    }

    if (!botName && !playerName)
    {
        // Show all sentiment data
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        if (g_BotPlayerSentiments.empty())
        {
            handler->SendSysMessage("OllamaChat 감정 데이터가 없습니다.");
            return true;
        }

        handler->SendSysMessage("OllamaChat 전체 감정 데이터:");
        for (const auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
            std::string botNameStr = bot ? bot->GetName() : std::to_string(botGuid);
            
            for (const auto& [playerGuid, sentiment] : playerMap)
            {
                Player* player = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
                std::string playerNameStr = player ? player->GetName() : std::to_string(playerGuid);
                
                handler->SendSysMessage(fmt::format("  Bot '{}' -> Player '{}': {:.3f}", 
                                        botNameStr, playerNameStr, sentiment));
            }
        }
        return true;
    }

    // Find specific bot or player
    Player* targetBot = nullptr;
    Player* targetPlayer = nullptr;

    if (botName)
    {
        targetBot = ObjectAccessor::FindPlayerByName(*botName);
        if (!targetBot)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", *botName));
            return true;
        }
        if (!PlayerbotsMgr::instance().GetPlayerbotAI(targetBot))
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", *botName));
            return true;
        }
    }

    if (playerName)
    {
        targetPlayer = ObjectAccessor::FindPlayerByName(*playerName);
        if (!targetPlayer)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' not found.", *playerName));
            return true;
        }
    }

    // Show sentiment for specific bot-player pair or all pairs involving a specific bot/player
    if (targetBot && targetPlayer)
    {
        float sentiment = GetBotPlayerSentiment(targetBot->GetGUID().GetRawValue(), targetPlayer->GetGUID().GetRawValue());
        handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' -> Player '{}': {:.3f}", 
                                targetBot->GetName(), targetPlayer->GetName(), sentiment));
    }
    else if (targetBot)
    {
        // Show all sentiments for this bot
        uint64_t botGuid = targetBot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        auto botIt = g_BotPlayerSentiments.find(botGuid);
        if (botIt == g_BotPlayerSentiments.end() || botIt->second.empty())
        {
            handler->SendSysMessage(fmt::format("OllamaChat: No sentiment data found for bot '{}'.", targetBot->GetName()));
            return true;
        }

        handler->SendSysMessage(fmt::format("OllamaChat: Sentiment data for bot '{}':", targetBot->GetName()));
        for (const auto& [playerGuid, sentiment] : botIt->second)
        {
            Player* player = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
            std::string playerNameStr = player ? player->GetName() : std::to_string(playerGuid);
            handler->SendSysMessage(fmt::format("  -> Player '{}': {:.3f}", playerNameStr, sentiment));
        }
    }
    else if (targetPlayer)
    {
        // Show all sentiments involving this player
        uint64_t playerGuid = targetPlayer->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        bool found = false;
        handler->SendSysMessage(fmt::format("OllamaChat: Sentiment data involving player '{}':", targetPlayer->GetName()));
        
        for (const auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            auto playerIt = playerMap.find(playerGuid);
            if (playerIt != playerMap.end())
            {
                Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
                std::string botNameStr = bot ? bot->GetName() : std::to_string(botGuid);
                handler->SendSysMessage(fmt::format("  Bot '{}' -> {:.3f}", botNameStr, playerIt->second));
                found = true;
            }
        }
        
        if (!found)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: No sentiment data found involving player '{}'.", targetPlayer->GetName()));
        }
    }

    return true;
}

bool OllamaChatConfigCommand::HandleOllamaSentimentSetCommand(ChatHandler* handler, std::string botName, std::string playerName, float sentimentValue)
{
    if (!g_EnableSentimentTracking)
    {
        handler->SendSysMessage("OllamaChat 감정 추적이 꺼져 있습니다.");
        return true;
    }

    Player* bot = ObjectAccessor::FindPlayerByName(botName);
    if (!bot)
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", botName));
        return true;
    }
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", botName));
        return true;
    }

    Player* player = ObjectAccessor::FindPlayerByName(playerName);
    if (!player)
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' not found.", playerName));
        return true;
    }

    if (sentimentValue < 0.0f || sentimentValue > 1.0f)
    {
        handler->SendSysMessage("OllamaChat 감정 값은 0.0~1.0이어야 합니다.");
        return true;
    }

    SetBotPlayerSentiment(bot->GetGUID().GetRawValue(), player->GetGUID().GetRawValue(), sentimentValue);
    handler->SendSysMessage(fmt::format("OllamaChat: Set sentiment between bot '{}' and player '{}' to {:.3f}.", 
                            botName, playerName, sentimentValue));
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaSentimentResetCommand(ChatHandler* handler, Optional<std::string> botName, Optional<std::string> playerName)
{
    if (!g_EnableSentimentTracking)
    {
        handler->SendSysMessage("OllamaChat 감정 추적이 꺼져 있습니다.");
        return true;
    }

    if (!botName && !playerName)
    {
        // Reset all sentiment data
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        uint32_t count = 0;
        for (const auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            count += playerMap.size();
        }
        g_BotPlayerSentiments.clear();
        g_DirtySentiments.clear();
        CharacterDatabase.Execute("DELETE FROM mod_ollama_chat_bot_player_sentiments");
        handler->SendSysMessage(fmt::format("OllamaChat: Reset all sentiment data ({} records).", count));
        return true;
    }

    Player* targetBot = nullptr;
    Player* targetPlayer = nullptr;

    if (botName)
    {
        targetBot = ObjectAccessor::FindPlayerByName(*botName);
        if (!targetBot)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", *botName));
            return true;
        }
        if (!PlayerbotsMgr::instance().GetPlayerbotAI(targetBot))
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", *botName));
            return true;
        }
    }

    if (playerName)
    {
        targetPlayer = ObjectAccessor::FindPlayerByName(*playerName);
        if (!targetPlayer)
        {
            handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' not found.", *playerName));
            return true;
        }
    }

    if (targetBot && targetPlayer)
    {
        // Reset specific bot-player sentiment
        SetBotPlayerSentiment(targetBot->GetGUID().GetRawValue(), targetPlayer->GetGUID().GetRawValue(), g_SentimentDefaultValue);
        handler->SendSysMessage(fmt::format("OllamaChat: Reset sentiment between bot '{}' and player '{}' to default ({:.3f}).", 
                                targetBot->GetName(), targetPlayer->GetName(), g_SentimentDefaultValue));
    }
    else if (targetBot)
    {
        // Reset all sentiments for this bot
        uint64_t botGuid = targetBot->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        auto botIt = g_BotPlayerSentiments.find(botGuid);
        if (botIt != g_BotPlayerSentiments.end())
        {
            uint32_t count = botIt->second.size();
            g_BotPlayerSentiments.erase(botIt);

            for (auto it = g_DirtySentiments.begin(); it != g_DirtySentiments.end(); )
            {
                if (it->first == botGuid)
                    it = g_DirtySentiments.erase(it);
                else
                    ++it;
            }

            CharacterDatabase.Execute(SafeFormat(
                "DELETE FROM mod_ollama_chat_bot_player_sentiments WHERE bot_guid = {}", botGuid));
            handler->SendSysMessage(fmt::format("OllamaChat: Reset all sentiment data for bot '{}' ({} records).", 
                                    targetBot->GetName(), count));
        }
        else
        {
            handler->SendSysMessage(fmt::format("OllamaChat: No sentiment data found for bot '{}'.", targetBot->GetName()));
        }
    }
    else if (targetPlayer)
    {
        // Reset all sentiments involving this player
        uint64_t playerGuid = targetPlayer->GetGUID().GetRawValue();
        std::lock_guard<std::mutex> lock(g_SentimentMutex);
        
        uint32_t count = 0;
        for (auto& [botGuid, playerMap] : g_BotPlayerSentiments)
        {
            auto playerIt = playerMap.find(playerGuid);
            if (playerIt != playerMap.end())
            {
                playerMap.erase(playerIt);
                count++;
            }
        }

        for (auto it = g_DirtySentiments.begin(); it != g_DirtySentiments.end(); )
        {
            if (it->second == playerGuid)
                it = g_DirtySentiments.erase(it);
            else
                ++it;
        }

        CharacterDatabase.Execute(SafeFormat(
            "DELETE FROM mod_ollama_chat_bot_player_sentiments WHERE player_guid = {}", playerGuid));

        handler->SendSysMessage(fmt::format("OllamaChat: Reset all sentiment data involving player '{}' ({} records).", 
                                targetPlayer->GetName(), count));
    }

    return true;
}

bool OllamaChatConfigCommand::HandleOllamaPersonalityGetCommand(ChatHandler* handler, std::string botName)
{
    Player* bot = ObjectAccessor::FindPlayerByName(botName);
    if (!bot)
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", botName));
        return true;
    }
    
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", botName));
        return true;
    }
    
    std::string personality = GetBotPersonality(bot);
    std::string prompt = GetPersonalityPromptAddition(personality);
    
    handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' has personality '{}'", botName, personality));
    handler->SendSysMessage(fmt::format("  Prompt: {}", prompt));
    
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaPersonalitySetCommand(ChatHandler* handler, std::string botName, std::string personality)
{
    Player* bot = ObjectAccessor::FindPlayerByName(botName);
    if (!bot)
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Bot '{}' not found.", botName));
        return true;
    }
    
    if (!PlayerbotsMgr::instance().GetPlayerbotAI(bot))
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Player '{}' is not a bot.", botName));
        return true;
    }
    
    if (!PersonalityExists(personality))
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Personality '{}' does not exist. Use '.ollama personality list' to see available personalities.", personality));
        return true;
    }
    
    if (SetBotPersonality(bot, personality))
    {
        std::string prompt = GetPersonalityPromptAddition(personality);
        handler->SendSysMessage(fmt::format("OllamaChat: Set bot '{}' personality to '{}'", botName, personality));
        handler->SendSysMessage(fmt::format("  Prompt: {}", prompt));
    }
    else
    {
        handler->SendSysMessage(fmt::format("OllamaChat: Failed to set personality for bot '{}'.", botName));
    }
    
    return true;
}

bool OllamaChatConfigCommand::HandleOllamaPersonalityListCommand(ChatHandler* handler)
{
    std::vector<std::string> personalities = GetAllPersonalityKeys();
    
    if (personalities.empty())
    {
        handler->SendSysMessage("OllamaChat 성격 데이터를 불러오지 못했습니다.");
        return true;
    }
    
    handler->SendSysMessage(fmt::format("OllamaChat: Available personalities ({} total, {} random-assignable):", 
                            personalities.size(), g_PersonalityKeysRandomOnly.size()));
    
    for (const auto& personality : personalities)
    {
        std::string prompt = GetPersonalityPromptAddition(personality);
        
        // Check if this personality is manual-only
        bool isManualOnly = (std::find(g_PersonalityKeysRandomOnly.begin(), g_PersonalityKeysRandomOnly.end(), personality) 
                            == g_PersonalityKeysRandomOnly.end());
        
        std::string manualTag = isManualOnly ? " [MANUAL ONLY]" : "";
        
        handler->SendSysMessage(fmt::format("  - {}{}", personality, manualTag));
        handler->SendSysMessage(fmt::format("    {}", prompt));
    }
    
    return true;
}


// ---------------------------------------------------------------------------
// Diagnostics
// ---------------------------------------------------------------------------

bool OllamaChatConfigCommand::HandleOllamaStatusCommand(ChatHandler* handler)
{
    const OllamaDispatchStats dispatch = OllamaDispatch_GetStats();
    const GovernorStats       gov      = Governor_GetStats();

    handler->PSendSysMessage("|cff00ff00[Ollama Chat] 상태|r");
    handler->PSendSysMessage("모듈: {}   접속 주소: {}   모델: {}",
                             g_Enable ? "enabled" : "DISABLED",
                             g_OllamaUrl, g_OllamaModel);
    handler->PSendSysMessage("추론: {}", OllamaCapability_StatusText());

    handler->PSendSysMessage("처리기: 작업자 {}개, 대기 {}개, 요청 중 {}개, 전달 대기 {}개",
                             dispatch.workers, dispatch.queuedRequests,
                             dispatch.inFlight, dispatch.pendingDeliveries);
    handler->PSendSysMessage("누계: 요청 {}, 전달 {}, 실패 {}",
                             (unsigned long long)dispatch.totalSubmitted,
                             (unsigned long long)dispatch.totalDelivered,
                             (unsigned long long)dispatch.totalFailed);
    handler->PSendSysMessage("제외: 대기열 초과 {}, 정리 후 빈 응답 {}, 발화 제한 {}",
                             (unsigned long long)dispatch.totalDroppedQueueFull,
                             (unsigned long long)dispatch.totalDroppedEmpty,
                             (unsigned long long)dispatch.totalDroppedGovernor);

    handler->PSendSysMessage("발화 관리: 봇 {}명, 범위 {}개, 최근 1분 발화 {}회",
                             gov.trackedBots, gov.trackedScopes, gov.sendsLastMinute);
    handler->PSendSysMessage("제한: 재사용 대기 {}, 빈도 {}, 반복 {}, 연쇄 깊이 {}, 청자 없음 {}",
                             gov.blockedCooldown, gov.blockedRate, gov.blockedRepetition,
                             gov.blockedChainDepth, gov.blockedNoAudience);

    handler->PSendSysMessage("역할 연기: {} (엄격도 {})   감정표현 반응: {}",
                             g_RoleplayEnable ? "on" : "off",
                             (uint32)g_RoleplayStrictness,
                             g_EnableEmoteReactions ? "on" : "off");
    handler->PSendSysMessage("주제 가중치: 사람 {} / 세계 {} / 활동 {} / 자신 {} / 길드 {}",
                             g_TopicWeightPeople, g_TopicWeightWorld, g_TopicWeightActivity,
                             g_TopicWeightSelf, g_TopicWeightGuild);

    if (!dispatch.lastError.empty())
        handler->PSendSysMessage("|cffff0000마지막 오류:|r {}", dispatch.lastError);
    else
        handler->PSendSysMessage("마지막 오류: 없음");

    return true;
}

bool OllamaChatConfigCommand::HandleOllamaTestCommand(ChatHandler* handler, Acore::ChatCommands::Tail prompt)
{
    std::string text(prompt);
    if (text.empty())
    {
        handler->SendSysMessage("사용법: .ollama test <질문>");
        handler->SetSentErrorMessage(true);
        return false;
    }

    handler->PSendSysMessage("[Ollama Chat] 시험 질문을 전송합니다. 잠시 기다려 주세요.");

    // Blocking HTTP must not run on the world thread, so do the round trip on
    // a scratch thread and report from there. Turns "the bots are quiet" into
    // a one-command diagnosis: you see the raw output and the cleaned output
    // side by side.
    std::thread([text]()
    {
        OllamaApiResult api = QueryOllama(text, OllamaRequestKind::ChatReply);

        if (!api.ok)
        {
            LOG_INFO("module.ollamachat", "[Ollama Chat] TEST FAILED after {}ms: {}",
                     api.latencyMs, api.error.empty() ? "unknown error" : api.error);
            return;
        }

        uint32_t emote = 0;
        const std::string cleaned = ProcessLlmResponse(api.text, "Tester", &emote);

        LOG_INFO("module.ollamachat", "[Ollama Chat] TEST ok in {}ms (think={}).",
                 api.latencyMs, api.thinkUsed ? "yes" : "no");
        LOG_INFO("module.ollamachat", "[Ollama Chat] TEST raw     : {}", api.text);
        LOG_INFO("module.ollamachat", "[Ollama Chat] TEST cleaned : {}", cleaned);
        if (emote)
            LOG_INFO("module.ollamachat", "[Ollama Chat] TEST emote   : {}", emote);
        if (!api.thinking.empty())
            LOG_INFO("module.ollamachat", "[Ollama Chat] TEST thinking: {}", api.thinking);
    }).detach();

    handler->PSendSysMessage("[Ollama Chat] 결과는 서버 로그(module.ollamachat)에 기록됩니다.");
    return true;
}
