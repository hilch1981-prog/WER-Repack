/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "HireAction.h"
#include "Event.h"
#include "PlayerbotAI.h"
#include "RandomPlayerbotMgr.h"

bool HireAction::Execute(Event /*event*/)
{
    Player* master = GetMaster();
    if (!master)
        return false;

    if (!RandomPlayerbotMgr::instance().IsRandomBot(bot))
        return false;

    uint32 account = master->GetSession()->GetAccountId();
    QueryResult results = CharacterDatabase.Query("SELECT COUNT(*) FROM characters WHERE account = {}", account);

    uint32 charCount = 10;
    if (results)
    {
        Field* fields = results->Fetch();
        charCount = uint32(fields[0].Get<uint64>());
    }

    if (charCount >= 10)
    {
        botAI->TellMaster("이미 최대 캐릭터 수에 도달했습니다.");
        return false;
    }

    if (bot->GetLevel() > master->GetLevel())
    {
        botAI->TellMaster("자신보다 레벨이 높은 캐릭터는 고용할 수 없습니다.");
        return false;
    }

    uint32 discount = RandomPlayerbotMgr::instance().GetTradeDiscount(bot, master);
    uint32 m = 1 + (bot->GetLevel() / 10);
    uint32 moneyReq = m * 5000 * bot->GetLevel();
    if (discount < moneyReq)
    {
        std::ostringstream out;
        out << "You cannot hire me - I barely know you. Make sure you have at least " << chat->formatMoney(moneyReq)
            << " as a trade discount";
        botAI->TellMaster(out.str());
        return false;
    }

    botAI->TellMaster("다시 접속하시면 합류할게요.");

    bot->SetMoney(moneyReq);
    RandomPlayerbotMgr::instance().Remove(bot);
    CharacterDatabase.Execute("UPDATE characters SET account = {} WHERE guid = {}", account,
                              bot->GetGUID().GetCounter());

    return true;
}
