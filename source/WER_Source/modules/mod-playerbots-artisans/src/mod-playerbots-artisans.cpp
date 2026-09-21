/*
 * mod-playerbots-artisans  (v0.2)
 *
 * Makes crafter bots act like part of the world: they periodically advertise
 * their trade in Trade/General chat, naming real recipes they actually know.
 * Everything is driven off each bot's LEARNED recipes and current skill, so a
 * level-20 enchanter advertises cheap enchants and a 300 one advertises the good
 * stuff — no hardcoded level assumptions. Leveling itself is already handled by
 * the playerbot factory (skill = level * 5); this is the "be visible" layer.
 *
 * Changes in v0.2
 *   - Ads use real clickable item/enchant links instead of plain [Text].
 *   - A bot advertises its BEST profession, not the first one in the table.
 *   - Whisper a bot that advertised and it answers with its trade and terms.
 *   - _lastAdvert is pruned, so it no longer grows for the life of the process.
 *   - Ad phrasing varies, so a busy Trade channel doesn't read like one script.
 */

#include "ScriptMgr.h"
#include "WorldScript.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "SharedDefines.h"
#include "Config.h"
#include "World.h"
#include "GameTime.h"
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <map>
#include <set>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "BotActivityRegistry.h"

namespace PBArtisans
{
    static bool     Enable = true;
    static uint32   TickSeconds = 45;    // how often a fresh batch of ads goes out
    static uint32   AdsPerTick = 2;     // ads posted per tick (keep low to avoid spam)
    static uint32   MinSkill = 100;   // don't advertise below this skill
    static uint32   AdvertiseCount = 2;     // how many recipes to name per ad
    static uint32   PerBotCooldown = 900;   // seconds before the same bot advertises again
    static bool     UseTradeChannel = true;
    static bool     UseGeneralChannel = false;

    // v0.2
    static bool     UseItemLinks = true;    // clickable links vs plain text
    static bool     AnswerWhispers = true;    // reply when a player whispers an advertiser
    static uint32   WhisperRecipeCount = 5;  // recipes listed in a whisper reply
    static uint32   WhisperMemorySeconds = 3600; // how long a bot remembers it advertised
    static bool     VaryAdWording = true;

    static uint32 _timerMs = 0;

    struct AdMemory
    {
        uint32 when = 0;      // unix time of the ad
        uint32 skillId = 0;   // profession advertised
    };

    static std::unordered_map<uint32, uint32>   _lastAdvert;   // guidLow -> unix time
    static std::unordered_map<uint32, AdMemory> _adMemory;     // guidLow -> what it advertised

    static void LoadConfig()
    {
        Enable = sConfigMgr->GetOption<bool>("PlayerbotArtisans.Enable", true);
        TickSeconds = sConfigMgr->GetOption<uint32>("PlayerbotArtisans.TickSeconds", 45);
        AdsPerTick = sConfigMgr->GetOption<uint32>("PlayerbotArtisans.AdsPerTick", 2);
        MinSkill = sConfigMgr->GetOption<uint32>("PlayerbotArtisans.MinSkill", 100);
        AdvertiseCount = sConfigMgr->GetOption<uint32>("PlayerbotArtisans.AdvertiseCount", 2);
        PerBotCooldown = sConfigMgr->GetOption<uint32>("PlayerbotArtisans.PerBotCooldownSeconds", 900);
        UseTradeChannel = sConfigMgr->GetOption<bool>("PlayerbotArtisans.UseTradeChannel", true);
        UseGeneralChannel = sConfigMgr->GetOption<bool>("PlayerbotArtisans.UseGeneralChannel", false);

        UseItemLinks = sConfigMgr->GetOption<bool>("PlayerbotArtisans.UseItemLinks", true);
        AnswerWhispers = sConfigMgr->GetOption<bool>("PlayerbotArtisans.AnswerWhispers", true);
        WhisperRecipeCount = sConfigMgr->GetOption<uint32>("PlayerbotArtisans.WhisperRecipeCount", 5);
        WhisperMemorySeconds = sConfigMgr->GetOption<uint32>("PlayerbotArtisans.WhisperMemorySeconds", 3600);
        VaryAdWording = sConfigMgr->GetOption<bool>("PlayerbotArtisans.VaryAdWording", true);

        if (TickSeconds < 1)  TickSeconds = 1;
        if (AdsPerTick < 1)   AdsPerTick = 1;
        if (WhisperRecipeCount < 1) WhisperRecipeCount = 1;
    }

    // Crafting professions we advertise, with the "advertiser noun" and a short
    // call-to-action verb. Gathering skills are intentionally excluded here.
    struct Profession { uint32 skill; char const* noun; char const* verb; };
    static std::vector<Profession> const& Professions()
    {
        static std::vector<Profession> const p = {
            { SKILL_ALCHEMY,       "연금술사",       "물약과 변환" },
            { SKILL_BLACKSMITHING, "대장장이",       "대장기술" },
            { SKILL_ENCHANTING,    "마법부여사",     "마법부여" },
            { SKILL_ENGINEERING,   "기계공학자",     "기계공학 제작" },
            { SKILL_TAILORING,     "재봉사",         "재봉" },
            { SKILL_LEATHERWORKING,"가죽세공사",     "가죽세공" },
            { SKILL_JEWELCRAFTING, "보석세공사",     "보석과 반지 제작" },
            { SKILL_INSCRIPTION,   "주문각인사",     "문양과 두루마리 제작" },
            { SKILL_COOKING,       "요리사",         "요리" },
        };
        return p;
    }

    static Profession const* FindProfession(uint32 skillId)
    {
        for (Profession const& p : Professions())
            if (p.skill == skillId)
                return &p;
        return nullptr;
    }

    // ---------------------------------------------------------------------
    // Chat links
    //
    // Kept local rather than pulling in the core's colour table, so this file
    // has no extra link-time dependency. Order matches ItemQualities.
    // ---------------------------------------------------------------------
    static char const* QualityColor(uint32 quality)
    {
        static char const* const colors[] = {
            "ff9d9d9d", // poor
            "ffffffff", // normal
            "ff1eff00", // uncommon
            "ff0070dd", // rare
            "ffa335ee", // epic
            "ffff8000", // legendary
            "ffe6cc80", // artifact
            "ffe6cc80", // heirloom
        };
        return quality < (sizeof(colors) / sizeof(colors[0])) ? colors[quality] : "ffffffff";
    }

    struct RecipeAd
    {
        std::string name;
        uint32 diff = 0;        // TrivialSkillLineRankHigh — "how good is this recipe"
        uint32 itemEntry = 0;   // set for crafted items
        uint32 spellId = 0;     // set for enchants
        uint32 quality = 1;
    };

    // A clickable link the client will resolve, or plain text when disabled.
    static std::string LinkFor(RecipeAd const& r)
    {
        if (!UseItemLinks)
            return "[" + r.name + "]";

        if (r.itemEntry)
            return "|c" + std::string(QualityColor(r.quality)) + "|Hitem:" +
            std::to_string(r.itemEntry) + ":0:0:0:0:0:0:0:0|h[" + r.name + "]|h|r";

        if (r.spellId)
            return "|cffffd000|Henchant:" + std::to_string(r.spellId) +
            "|h[" + r.name + "]|h|r";

        return "[" + r.name + "]";
    }

    // Spell -> trade-skill ability, built once from the DBC.
    static std::map<uint32, SkillLineAbilityEntry const*>& SkillSpellMap()
    {
        static std::map<uint32, SkillLineAbilityEntry const*> m;
        if (m.empty())
            for (SkillLineAbilityEntry const* sla : sSkillLineAbilityStore)
                if (sla)
                    m[sla->Spell] = sla;
        return m;
    }

    // What this bot can actually make in `skillId`: created items for crafts,
    // enchant spells for enchanting. Level-appropriate for free, because the
    // bot only knows recipes its skill has unlocked.
    static std::vector<RecipeAd> KnownRecipes(Player* bot, uint32 skillId)
    {
        std::vector<RecipeAd> out;
        std::map<uint32, SkillLineAbilityEntry const*>& skillSpells = SkillSpellMap();

        for (PlayerSpellMap::const_iterator itr = bot->GetSpellMap().begin(); itr != bot->GetSpellMap().end(); ++itr)
        {
            if (!itr->second || itr->second->State == PLAYERSPELL_REMOVED || !itr->second->Active)
                continue;

            uint32 const spellId = itr->first;
            auto found = skillSpells.find(spellId);
            if (found == skillSpells.end() || !found->second)
                continue;
            SkillLineAbilityEntry const* sla = found->second;
            if (sla->SkillLine != skillId)
                continue;

            SpellInfo const* si = sSpellMgr->GetSpellInfo(spellId);
            if (!si)
                continue;

            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            {
                if (si->Effects[i].Effect == SPELL_EFFECT_CREATE_ITEM)
                {
                    if (ItemTemplate const* p = sObjectMgr->GetItemTemplate(si->Effects[i].ItemType))
                    {
                        RecipeAd r;
                        r.name = p->Name1;
                        if (ItemLocale const* locale = sObjectMgr->GetItemLocale(p->ItemId))
                            ObjectMgr::GetLocaleString(locale->Name, sWorld->GetDefaultDbcLocale(), r.name);
                        r.diff = sla->TrivialSkillLineRankHigh;
                        r.itemEntry = p->ItemId;
                        r.quality = p->Quality;
                        out.push_back(r);
                    }
                    break;
                }
                if (si->Effects[i].Effect == SPELL_EFFECT_ENCHANT_ITEM)
                {
                    uint32 const locale = sWorld->GetDefaultDbcLocale();
                    if (si->SpellName[locale] && si->SpellName[locale][0])
                    {
                        RecipeAd r;
                        r.name = si->SpellName[locale];
                        r.diff = sla->TrivialSkillLineRankHigh;
                        r.spellId = spellId;
                        out.push_back(r);
                    }
                    break;
                }
            }
        }
        return out;
    }

    // Best = highest skill among professions that clear MinSkill and have
    // something to show. v0.1 returned the first match in table order, which
    // meant an Alchemy-110 / Enchanting-300 bot always advertised the alchemy.
    struct BestPick
    {
        Profession const* prof = nullptr;
        uint32 skill = 0;
        std::vector<RecipeAd> recipes;
    };

    static BestPick PickBestProfession(Player* bot)
    {
        BestPick best;
        for (Profession const& prof : Professions())
        {
            uint32 const skill = bot->GetSkillValue(prof.skill);
            if (skill < MinSkill || skill <= best.skill)
                continue;

            std::vector<RecipeAd> recipes = KnownRecipes(bot, prof.skill);
            if (recipes.empty())
                continue;

            best.prof = &prof;
            best.skill = skill;
            best.recipes = std::move(recipes);
        }
        return best;
    }

    // Highest-difficulty recipes first, de-duplicated by name.
    static std::vector<RecipeAd> TopRecipes(std::vector<RecipeAd> recipes, uint32 count)
    {
        std::sort(recipes.begin(), recipes.end(),
            [](RecipeAd const& a, RecipeAd const& b) { return a.diff > b.diff; });

        std::vector<RecipeAd> picks;
        std::set<std::string> seen;
        for (RecipeAd const& r : recipes)
        {
            if (r.name.empty() || !seen.insert(r.name).second)
                continue;
            picks.push_back(r);
            if (picks.size() >= count)
                break;
        }
        return picks;
    }

    static std::string JoinLinks(std::vector<RecipeAd> const& picks)
    {
        std::string s;
        for (size_t i = 0; i < picks.size(); ++i)
        {
            if (i)
                s += ", ";
            s += LinkFor(picks[i]);
        }
        return s;
    }

    // Build one ad for the bot's best profession. Empty => nothing worth saying.
    static std::string ComposeAd(Player* bot, uint32* outSkillId)
    {
        BestPick best = PickBestProfession(bot);
        if (!best.prof)
            return "";

        std::vector<RecipeAd> picks = TopRecipes(best.recipes, AdvertiseCount);
        if (picks.empty())
            return "";

        if (outSkillId)
            *outSkillId = best.prof->skill;

        std::string const noun = best.prof->noun;
        std::string const verb = best.prof->verb;
        std::string const lvl = std::to_string(best.skill);
        std::string const list = JoinLinks(picks);

        if (!VaryAdWording)
            return noun + " (" + lvl + ") 제작 가능합니다: " + list + ". 필요하시면 귓속말 주세요.";

        switch (urand(0, 3))
        {
        case 0:  return noun + " (" + lvl + ") 제작 가능합니다: " + list + ". 필요하시면 귓속말 주세요.";
        case 1:  return noun + " 숙련 " + lvl + "입니다. " + list + " 제작 가능하며 재료는 직접 준비해 주세요.";
        case 2:  return verb + " 가능합니다: " + list + ". " + noun + " (" + lvl + ")에게 귓속말 주세요.";
        default: return noun + " (" + lvl + ")입니다. " + list + " 제작 가능하며 재료는 돌려드립니다.";
        }
    }

    static void Advertise(Player* bot)
    {
        PlayerbotAI* ai = GET_PLAYERBOT_AI(bot);
        if (!ai)
            return;

        uint32 skillId = 0;
        std::string const msg = ComposeAd(bot, &skillId);
        if (msg.empty())
            return;

        if (UseTradeChannel)
            ai->SayToChannel(msg, ChatChannelId::TRADE);
        if (UseGeneralChannel)
            ai->SayToChannel(msg, ChatChannelId::GENERAL);

        uint32 const now = uint32(GameTime::GetGameTime().count());
        uint32 const guidLow = bot->GetGUID().GetCounter();
        _lastAdvert[guidLow] = now;

        AdMemory mem;
        mem.when = now;
        mem.skillId = skillId;
        _adMemory[guidLow] = mem;
    }

    // ---------------------------------------------------------------------
    // Whisper replies
    //
    // Every ad ends with "whisper me" — so answer. This is the conversational
    // half only: the bot states its trade and terms. Actually taking the order
    // (trade window, reagents, craft, hand back) is the next layer.
    // ---------------------------------------------------------------------
    static std::string ComposeWhisperReply(Player* bot)
    {
        uint32 const guidLow = bot->GetGUID().GetCounter();
        uint32 const now = uint32(GameTime::GetGameTime().count());

        uint32 skillId = 0;
        auto mem = _adMemory.find(guidLow);
        if (mem != _adMemory.end() && now < mem->second.when + WhisperMemorySeconds)
            skillId = mem->second.skillId;

        Profession const* prof = skillId ? FindProfession(skillId) : nullptr;
        std::vector<RecipeAd> recipes;
        uint32 skill = 0;

        if (prof)
        {
            skill = bot->GetSkillValue(prof->skill);
            recipes = KnownRecipes(bot, prof->skill);
        }
        else
        {
            // Whispered without a remembered ad — answer with whatever it's best at.
            BestPick best = PickBestProfession(bot);
            if (!best.prof)
                return "";
            prof = best.prof;
            skill = best.skill;
            recipes = std::move(best.recipes);
        }

        std::vector<RecipeAd> picks = TopRecipes(std::move(recipes), WhisperRecipeCount);
        if (picks.empty())
            return "";

        return std::string(prof->noun) + " (숙련 " + std::to_string(skill) + ")입니다. 제작 가능: " +
            JoinLinks(picks) + ". 재료를 준비해 주시면 제작해 드립니다.";
    }

    static bool ShouldAnswer(Player* bot)
    {
        if (!AnswerWhispers || !bot)
            return false;
        if (!GET_PLAYERBOT_AI(bot))
            return false;
        if (!sRandomPlayerbotMgr.IsRandomBot(bot))
            return false;
        return true;
    }

    // ---------------------------------------------------------------------

    static bool EligibleNow(Player* p, uint32 now)
    {
        if (!p || !p->IsInWorld() || !p->IsAlive())
            return false;
        if (!sRandomPlayerbotMgr.IsRandomBot(p))
            return false;
        if (BotActivity::IsReserved(p->GetGUID().GetCounter()))
            return false;                                   // busy: in a run or PvP event
        if (p->IsInCombat() || (p->GetMap() && p->GetMap()->IsDungeon()))
            return false;
        auto last = _lastAdvert.find(p->GetGUID().GetCounter());
        if (last != _lastAdvert.end() && now < last->second + PerBotCooldown)
            return false;
        return true;
    }

    // v0.1 never erased from these maps, so they grew for the life of the
    // process as random bots rotated. Sweep anything past its usefulness.
    static void PruneMemory(uint32 now)
    {
        for (auto it = _lastAdvert.begin(); it != _lastAdvert.end(); )
            it = (now >= it->second + PerBotCooldown) ? _lastAdvert.erase(it) : std::next(it);

        for (auto it = _adMemory.begin(); it != _adMemory.end(); )
            it = (now >= it->second.when + WhisperMemorySeconds) ? _adMemory.erase(it) : std::next(it);
    }

    static void Tick()
    {
        uint32 const now = uint32(GameTime::GetGameTime().count());
        PruneMemory(now);

        std::vector<Player*> pool;

        {
            std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
            for (auto const& kv : ObjectAccessor::GetPlayers())
            {
                Player* p = kv.second;
                if (EligibleNow(p, now))
                    pool.push_back(p);
            }
        }

        if (pool.empty())
            return;

        for (uint32 n = 0; n < AdsPerTick && !pool.empty(); ++n)
        {
            uint32 const idx = urand(0, uint32(pool.size() - 1));
            Advertise(pool[idx]);
            pool[idx] = pool.back();
            pool.pop_back();
        }
    }
}

class PlayerbotsArtisansWorldScript : public WorldScript
{
public:
    PlayerbotsArtisansWorldScript() : WorldScript("PlayerbotsArtisansWorldScript") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        PBArtisans::LoadConfig();
    }

    void OnUpdate(uint32 diff) override
    {
        using namespace PBArtisans;
        if (!Enable)
            return;
        _timerMs += diff;
        if (_timerMs < TickSeconds * IN_MILLISECONDS)
            return;
        _timerMs = 0;
        Tick();
    }
};

// AzerothCore has no OnPlayerChat hook. The only chat hook that receives the
// whisper target (Player* receiver) is OnPlayerCanUseChat — a bool permission
// hook that fires as the whisper is sent. We only observe (to fire the bot's
// reply) and always return true so the player's whisper is never blocked.
class PlayerbotsArtisansChatScript : public PlayerScript
{
public:
    PlayerbotsArtisansChatScript() : PlayerScript("PlayerbotsArtisansChatScript") {}

    bool OnPlayerCanUseChat(Player* player, uint32 type, uint32 /*lang*/, std::string& /*msg*/, Player* receiver) override
    {
        if (type != CHAT_MSG_WHISPER || !player || !receiver)
            return true;
        if (!PBArtisans::ShouldAnswer(receiver))
            return true;
        if (GET_PLAYERBOT_AI(player))
            return true;                       // bot-to-bot whispers stay quiet

        std::string const reply = PBArtisans::ComposeWhisperReply(receiver);
        if (reply.empty())
            return true;

        receiver->Whisper(reply, LANG_UNIVERSAL, player);
        return true;
    }
};

void AddSC_mod_playerbots_artisans()
{
    new PlayerbotsArtisansWorldScript();
    new PlayerbotsArtisansChatScript();
}

// Loader entry point (called from the generated module script loader).
void Addmod_playerbots_artisansScripts()
{
    AddSC_mod_playerbots_artisans();
}
