/*
 * Copyright (C) 2025+ WOW Legends <https://wow-legends.eu>, released under GNU AGPL v3 license, you may
 * redistribute it and/or modify it under version 3 of the License, or (at your option), any later version.
 *
 * WOW Legends - "The Sage": fact-grounded bot chat.
 *
 * When a player asks a bot a question, the answer is grounded in SERVER
 * DATA: a facts block is appended to the AI persona ON THE WORLD THREAD at
 * enqueue time (wowlegends_aichat.cpp EnqueueAi), so every provider - a
 * player's own Ollama included - answers from the same truth. No tool
 * calling, no round trips: one enriched prompt.
 *
 * SECURITY LINE (foundational): every resolver reads STARTUP-LOADED,
 * READ-ONLY in-process stores (item/quest/creature templates, spawns,
 * vendor lists, quest relations). No SQL text is ever built, no RA, no
 * character/account data is reachable. Entity names are resolved from ids
 * or matched against server-side name indexes - the FACTS block only ever
 * contains CANONICAL DB names, never the player's own text (a hand-crafted
 * link bracket cannot smuggle instructions into the prompt).
 */

#include "ScriptMgr.h"
#include "Log.h"
#include "Player.h"
#include "ObjectMgr.h"
#include "Configuration/Config.h"
#include "SharedDefines.h"
#include "ItemTemplate.h"
#include "QuestDef.h"
#include "Configuration/Config.h"

#include "ChatHelper.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    bool CfgSageEnabled()
    { return sConfigMgr->GetOption<bool>("WowLegends.AiChat.Sage.Enabled", true); }

    // same normalization the order matcher uses: lowercase, strip
    // punctuation, collapse whitespace - index keys and probes must agree
    std::string SageNorm(std::string const& msg)
    {
        std::string norm;
        norm.reserve(msg.size());
        bool space = true;
        for (unsigned char c : msg)
        {
            if (std::ispunct(c))
                continue;
            if (std::isspace(c))
            {
                if (!space)
                    norm += ' ';
                space = true;
                continue;
            }
            norm += char(std::tolower(c));
            space = false;
        }
        if (!norm.empty() && norm.back() == ' ')
            norm.pop_back();
        return norm;
    }

    // -------- lazy name indexes (built once on first question) -----------
    struct SpawnPoint
    {
        uint32 mapId;
        float x, y, z;
    };

    struct SageIndexes
    {
        bool built = false;
        std::unordered_map<std::string, uint32> items;      // name -> entry
        std::unordered_map<std::string, uint32> quests;     // title -> id
        std::unordered_map<std::string, uint32> creatures;  // name -> entry
        std::unordered_map<uint32, std::vector<SpawnPoint>> spawnsByEntry;
        std::unordered_map<uint32, std::vector<uint32>> vendorsByItem;
        std::unordered_map<uint32, uint32> questGiver;      // quest -> entry
    };

    SageIndexes& Indexes()
    {
        static SageIndexes idx;
        if (idx.built)
            return idx;
        idx.built = true;

        // spawns FIRST: name-collision winners below prefer variants that
        // actually exist in the world (id2/id3 rotation slots included)
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            SpawnPoint const p{ data.mapid, data.posX, data.posY, data.posZ };
            idx.spawnsByEntry[data.id].push_back(p);
            if (data.id2)
                idx.spawnsByEntry[data.id2].push_back(p);
            if (data.id3)
                idx.spawnsByEntry[data.id3].push_back(p);
        }

        // placeholder rows must never win a name (587 "Deprecated..." items
        // alone); vocatives would hijack casual chat ("thanks buddy?")
        auto placeholder = [](std::string const& raw)
        {
            return raw.rfind("Deprecated", 0) == 0 || raw.rfind("OLD", 0) == 0
                || raw.find("[PH]") != std::string::npos;
        };
        auto stopped = [](std::string const& key)
        {
            for (char const* s : { "buddy", "honey", "mate", "dude", "friend" })
                if (key == s)
                    return true;
            return false;
        };

        for (auto const& [entry, tmpl] : *sObjectMgr->GetItemTemplateStore())
        {
            if (placeholder(tmpl.Name1))
                continue;
            std::string key = SageNorm(tmpl.Name1);
            if (key.size() < 4 || stopped(key))
                continue;
            auto [it, inserted] = idx.items.emplace(std::move(key), entry);
            if (!inserted && entry < it->second)
                it->second = entry;   // deterministic: lowest id wins
        }

        for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
        {
            std::string key = SageNorm(quest->GetTitle());
            if (key.size() < 4)
                continue;
            auto [it, inserted] = idx.quests.emplace(std::move(key), questId);
            if (!inserted && questId < it->second)
                it->second = questId;
        }

        for (auto const& [entry, tmpl] : *sObjectMgr->GetCreatureTemplates())
        {
            if (placeholder(tmpl.Name))
                continue;
            std::string key = SageNorm(tmpl.Name);
            if (key.size() < 4 || stopped(key))
                continue;
            auto [it, inserted] = idx.creatures.emplace(std::move(key), entry);
            if (!inserted)
            {
                // a SPAWNED variant beats an unspawned event/trigger copy
                // (e.g. the level-55 Acherus Deathcharger with 24 spawns vs
                // its level-1 zero-spawn twin), ties to the lowest entry
                bool const newSpawned = idx.spawnsByEntry.count(entry) != 0;
                bool const oldSpawned = idx.spawnsByEntry.count(it->second) != 0;
                if ((newSpawned && !oldSpawned)
                    || (newSpawned == oldSpawned && entry < it->second))
                    it->second = entry;
            }
        }

        // vendor reverse index: which NPCs sell an item (first few only)
        for (auto const& [entry, tmpl] : *sObjectMgr->GetCreatureTemplates())
        {
            if (!(tmpl.npcflag & UNIT_NPC_FLAG_VENDOR))
                continue;
            if (VendorItemData const* list = sObjectMgr->GetNpcVendorItemList(entry))
                for (VendorItem const* vi : list->m_items)
                {
                    auto& sellers = idx.vendorsByItem[vi->item];
                    if (sellers.size() < 3)
                        sellers.push_back(entry);
                }
        }

        // quest giver reverse index (creature givers; lowest entry wins)
        for (auto const& [entry, questId] : *sObjectMgr->GetCreatureQuestRelationMap())
        {
            auto [it, inserted] = idx.questGiver.emplace(questId, entry);
            if (!inserted && entry < it->second)
                it->second = entry;
        }

        LOG_INFO("server",
            "[sage] fact indexes built: {} items, {} quests, {} creatures, {} vendor items",
            idx.items.size(), idx.quests.size(), idx.creatures.size(),
            idx.vendorsByItem.size());
        return idx;
    }

    // ------------- formatting helpers ------------------------------------
    std::string Money(uint32 copper)
    {
        uint32 const g = copper / 10000;
        uint32 const s = (copper % 10000) / 100;
        uint32 const c = copper % 100;
        std::string out;
        if (g)
            out += std::to_string(g) + "g ";
        if (s || g)
            out += std::to_string(s) + "s ";
        out += std::to_string(c) + "c";
        return out;
    }

    char const* QualityName(uint32 q)
    {
        switch (q)
        {
            case ITEM_QUALITY_POOR: return "Poor";
            case ITEM_QUALITY_NORMAL: return "Common";
            case ITEM_QUALITY_UNCOMMON: return "Uncommon";
            case ITEM_QUALITY_RARE: return "Rare";
            case ITEM_QUALITY_EPIC: return "Epic";
            case ITEM_QUALITY_LEGENDARY: return "Legendary";
            case ITEM_QUALITY_ARTIFACT: return "Artifact";
            case ITEM_QUALITY_HEIRLOOM: return "Heirloom";
            default: return "Unknown";
        }
    }

    char const* Compass(float angle)
    {
        static char const* const dirs[] =
            { "north", "northwest", "west", "southwest",
              "south", "southeast", "east", "northeast" };
        int const idx = int(std::round(angle / (M_PI / 4.0f))) & 7;
        return dirs[idx];
    }

    // nearest spawn of `entry` on the speaker's map -> " - nearest one ~120
    // yds to the northwest" ("" when none here)
    std::string NearestSpawnLine(Player* speaker, uint32 entry)
    {
        auto const it = Indexes().spawnsByEntry.find(entry);
        if (it == Indexes().spawnsByEntry.end())
            return "";

        SpawnPoint const* best = nullptr;
        float bestDist = 0.0f;
        for (SpawnPoint const& p : it->second)
        {
            if (p.mapId != speaker->GetMapId())
                continue;
            float const d = speaker->GetDistance(p.x, p.y, p.z);
            if (!best || d < bestDist)
            {
                best = &p;
                bestDist = d;
            }
        }
        if (!best)
            return " (none anywhere near here)";

        float const angle = speaker->GetAngle(best->x, best->y);
        return " - nearest one ~" + std::to_string(uint32(bestDist)) + " yds "
            + Compass(angle) + " of the player";
    }

    void ItemFacts(Player* speaker, uint32 entry, std::string& out)
    {
        ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(entry);
        if (!tmpl)
            return;

        out += "\n- Item [" + tmpl->Name1 + "]: " + QualityName(tmpl->Quality)
            + ", item level " + std::to_string(tmpl->ItemLevel);
        if (tmpl->RequiredLevel > 1)
            out += ", requires level " + std::to_string(tmpl->RequiredLevel);
        if (tmpl->Bonding == BIND_WHEN_PICKED_UP)
            out += ", binds when picked up";
        else if (tmpl->Bonding == BIND_WHEN_EQUIPPED)
            out += ", binds when equipped";
        if (tmpl->SellPrice)
            out += ", sells to vendors for " + Money(tmpl->SellPrice);

        auto const vit = Indexes().vendorsByItem.find(entry);
        if (vit != Indexes().vendorsByItem.end() && !vit->second.empty())
        {
            if (tmpl->BuyPrice)
                out += ", vendor price " + Money(tmpl->BuyPrice);
            out += ", sold by";
            for (std::size_t i = 0; i < vit->second.size(); ++i)
            {
                CreatureTemplate const* seller = sObjectMgr->GetCreatureTemplate(vit->second[i]);
                if (!seller)
                    continue;
                out += (i ? ", " : " ") + seller->Name;
                if (i == 0)
                    out += NearestSpawnLine(speaker, vit->second[i]);
            }
        }
        out += ".";
    }

    void QuestFacts(Player* speaker, uint32 questId, std::string& out)
    {
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            return;

        out += "\n- Quest \"" + quest->GetTitle() + "\": level "
            + std::to_string(quest->GetQuestLevel()) + " (requires level "
            + std::to_string(quest->GetMinLevel()) + ")";

        auto const git = Indexes().questGiver.find(questId);
        if (git != Indexes().questGiver.end())
            if (CreatureTemplate const* giver = sObjectMgr->GetCreatureTemplate(git->second))
                out += ", started at " + giver->Name
                    + NearestSpawnLine(speaker, git->second);

        std::string const& objectives = quest->GetObjectives();
        if (!objectives.empty())
        {
            std::string trimmed = objectives.substr(0, 160);
            out += ". Objective: " + trimmed
                + (objectives.size() > 160 ? "..." : "");
        }

        if (int32 const money = quest->GetRewOrReqMoney(speaker->GetLevel()); money > 0)
            out += " Rewards " + Money(uint32(money));
        for (uint32 rewId : quest->RewardItemId)
            if (rewId)
                if (ItemTemplate const* rew = sObjectMgr->GetItemTemplate(rewId))
                    out += ", rewards [" + rew->Name1 + "]";
        out += ".";
    }

    void CreatureFacts(Player* speaker, uint32 entry, std::string& out)
    {
        CreatureTemplate const* tmpl = sObjectMgr->GetCreatureTemplate(entry);
        if (!tmpl)
            return;

        out += "\n- NPC " + tmpl->Name;
        if (!tmpl->SubName.empty())
            out += " <" + tmpl->SubName + ">";
        out += ": level " + std::to_string(tmpl->minlevel);
        if (tmpl->maxlevel != tmpl->minlevel)
            out += "-" + std::to_string(tmpl->maxlevel);
        if (tmpl->npcflag & UNIT_NPC_FLAG_VENDOR)
            out += ", a vendor";
        if (tmpl->npcflag & UNIT_NPC_FLAG_TRAINER)
            out += ", a trainer";
        if (tmpl->npcflag & UNIT_NPC_FLAG_INNKEEPER)
            out += ", an innkeeper";
        if (tmpl->npcflag & UNIT_NPC_FLAG_FLIGHTMASTER)
            out += ", a flight master";
        if (tmpl->npcflag & UNIT_NPC_FLAG_QUESTGIVER)
            out += ", a quest giver";
        out += NearestSpawnLine(speaker, entry);
        out += ".";
    }
}

// Warm the indexes at server startup (called from the aichat OnStartup) so
// the first question never hitches the world tick with a 130k-name build.
// Unconditional: a later ".reload config" enable must not pay the build
// mid-tick either; the per-message toggle in WlBuildSageFacts is the gate.
void WlSageWarmup()
{
    Indexes();
}

// The one exported entry point, spliced into the AI persona at enqueue time
// (wowlegends_aichat.cpp). WORLD THREAD ONLY. Returns "" when the message
// is not question-shaped or resolves no known entity.
std::string WlBuildSageFacts(Player* speaker, std::string const& msg)
{
    if (!CfgSageEnabled() || !speaker)
        return "";

    // tokenize once: the question gate and the n-gram probes both need it
    std::string const norm = SageNorm(msg);
    std::vector<std::string> words;
    {
        std::size_t pos = 0;
        while (pos < norm.size())
        {
            std::size_t next = norm.find(' ', pos);
            if (next == std::string::npos)
                next = norm.size();
            if (next > pos)
                words.push_back(norm.substr(pos, next - pos));
            pos = next + 1;
        }
    }

    // question gate: a '?', a game link, an interrogative WORD (whole-token
    // match - "show me" must not gate via the "how" substring), or a strong
    // lookup cue
    bool const linked = msg.find("|H") != std::string::npos;
    bool strongCue = norm.find("price") != std::string::npos
        || norm.find("cost") != std::string::npos
        || norm.find("sells") != std::string::npos
        || norm.find("drop") != std::string::npos
        || norm.find("reward") != std::string::npos
        || norm.find("what is") != std::string::npos
        || norm.find("who is") != std::string::npos
        || norm.find("where") != std::string::npos;
    bool question = msg.find('?') != std::string::npos || linked || strongCue;
    if (!question)
        for (std::string const& w : words)
            if (w == "what" || w == "where" || w == "who" || w == "which"
                || w == "how")
            {
                question = true;
                break;
            }
    if (!question)
        return "";

    SageIndexes& idx = Indexes();

    // entities: exact game links first (ids are authoritative), then
    // longest-first n-gram probes of the name indexes
    std::string facts;
    uint32 found = 0;
    std::unordered_set<uint64> seen;   // (type<<32)|id
    auto take = [&](uint32 type, uint32 id)
    {
        if (found >= 3 || !seen.insert((uint64(type) << 32) | id).second)
            return;

        // count a slot ONLY when a fact was actually appended - a dead id
        // typed as plain text ("Hitem:999999") must not burn the cap and
        // silence the name probes
        std::size_t const before = facts.size();
        if (type == 0)
            ItemFacts(speaker, id, facts);
        else if (type == 1)
            QuestFacts(speaker, id, facts);
        else
            CreatureFacts(speaker, id, facts);
        if (facts.size() > before)
            ++found;
    };

    for (uint32 id : ChatHelper::ExtractAllItemIds(msg))
        take(0, id);
    for (uint32 id : ChatHelper::ExtractAllQuestIds(msg))
        take(1, id);

    if (found < 3)
    {
        std::vector<bool> used(words.size(), false);
        for (std::size_t len = std::min<std::size_t>(5, words.size());
             len >= 1 && found < 3; --len)
        {
            for (std::size_t start = 0; start + len <= words.size(); ++start)
            {
                bool overlap = false;
                for (std::size_t k = start; k < start + len; ++k)
                    if (used[k])
                        overlap = true;
                if (overlap)
                    continue;

                // single common words hijack casual chat ("what boots should
                // i buy" - "boots" is an item): a 1-word window only counts
                // when a determiner precedes it AND the message carries a
                // strong lookup cue
                if (len == 1)
                {
                    if (!strongCue)
                        continue;
                    if (start == 0)
                        continue;
                    std::string const& prev = words[start - 1];
                    if (prev != "the" && prev != "a" && prev != "an"
                        && prev != "this" && prev != "that" && prev != "my"
                        && prev != "your" && prev != "is")
                        continue;
                }

                std::string window = words[start];
                for (std::size_t k = start + 1; k < start + len; ++k)
                    window += " " + words[k];

                uint32 type = 3;
                uint32 id = 0;
                if (auto it = idx.items.find(window); it != idx.items.end())
                {
                    type = 0;
                    id = it->second;
                }
                else if (auto qt = idx.quests.find(window); qt != idx.quests.end())
                {
                    type = 1;
                    id = qt->second;
                }
                else if (auto ct = idx.creatures.find(window); ct != idx.creatures.end())
                {
                    type = 2;
                    id = ct->second;
                }
                if (type == 3)
                    continue;

                take(type, id);
                for (std::size_t k = start; k < start + len; ++k)
                    used[k] = true;
                if (found >= 3)
                    break;
            }
            if (len == 1)
                break;
        }
    }

    if (facts.empty())
        return "";

    return "\nSERVER FACTS (true on this server - answer FROM these; if they "
           "do not cover the question, say you are not sure instead of "
           "guessing):" + facts;
}
