#include "ScriptMgr.h"
#include "AuctionHouseMgr.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Item.h"
#include "Mail.h"
#include "Player.h"
#include "Log.h"
#include "Creature.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "Map.h"
#include "DBCStores.h"
#include <fstream>
#include <filesystem>
#include <list>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <sstream>
#include <unordered_set>

namespace
{
std::atomic<bool> ready{false};
std::mutex monitoringMutex;
std::unordered_set<uint32> tracked;
std::unordered_set<uint32> marketBots;

void LoadGuids(std::string const& option, std::unordered_set<uint32>& destination)
{
    std::istringstream stream(sConfigMgr->GetOption<std::string>(option, ""));
    std::string value;
    while (std::getline(stream, value, ','))
    {
        try
        {
            std::size_t used = 0;
            auto guid = std::stoul(value, &used);
            if (used == value.size() && guid > 0 && guid <= UINT32_MAX)
                destination.insert(static_cast<uint32>(guid));
        }
        catch (std::exception const&) { }
    }
}

bool IsTracked(uint32 guid)
{
    std::lock_guard<std::mutex> lock(monitoringMutex);
    return tracked.count(guid) != 0;
}

void ReloadMonitoring()
{
    std::ifstream file(sConfigMgr->GetOption<std::string>("TransactionJournal.MonitoringFile", "/logs/transaction-monitoring.txt"));
    std::unordered_set<uint32> next;
    uint64 guid;
    while (next.size() < 5000 && file >> guid)
        if (guid > 0 && guid <= UINT32_MAX)
            next.insert(static_cast<uint32>(guid));
    // Missing files fail closed. Replacing the control file is atomic in the Hub.
    std::lock_guard<std::mutex> lock(monitoringMutex);
    tracked.swap(next);
}

void ItemEvent(char const* kind, uint32 actor, uint32 item, uint32 count, uint32 amount)
{
    CharacterDatabase.Execute(
        "INSERT INTO transaction_journal "
        "(event_uuid,event_kind,actor_guid,item_id,quantity,gross,proceeds) "
        "VALUES(UUID(),'{}',{},{},{},{},{})",
        kind, actor, item, count, amount, std::string(kind) == "vendor_sale_observed" ? amount : 0);
}

struct LootSource
{
    std::string type = "unknown", guid, name, zoneName, mapName, areaName;
    uint32 entry = 0, map = 0, zone = 0, area = 0;
};

LootSource ResolveLootSource(Player* player, ObjectGuid guid)
{
    LootSource result;
    result.guid = guid.ToString();
    if (guid.IsItem())
    {
        result.type = "item";
        if (Item* item = player->GetItemByGuid(guid))
        {
            result.entry = item->GetEntry();
            result.name = item->GetTemplate()->Name1;
        }
        return result;
    }
    if (WorldObject* source = ObjectAccessor::GetWorldObject(*player, guid))
    {
        result.type = guid.IsCreatureOrVehicle() ? "creature" : guid.IsGameObject() ? "gameobject" : "corpse";
        result.entry = source->GetEntry();
        result.name = source->GetName();
        result.map = source->GetMapId();
        result.mapName = source->GetMap()->GetMapName();
        result.zone = source->GetZoneId();
        result.area = source->GetAreaId();
        if (auto zone = sAreaTableStore.LookupEntry(result.zone))
            result.zoneName = zone->area_name[0];
        if (auto area = sAreaTableStore.LookupEntry(result.area))
            result.areaName = area->area_name[0];
    }
    return result;
}

void LootEvent(char const* kind, uint32 actor, uint32 item, uint32 count, uint32 copper, LootSource source)
{
    for (std::string* value : {&source.type, &source.guid, &source.name, &source.zoneName, &source.mapName, &source.areaName})
        CharacterDatabase.EscapeString(*value);
    CharacterDatabase.Execute(
        "INSERT INTO transaction_journal "
        "(event_uuid,event_kind,actor_guid,item_id,quantity,gross,proceeds,"
        "source_type,source_entry,source_guid,source_name,map_id,map_name,zone_id,zone_name,area_id,area_name) "
        "VALUES(UUID(),'{}',{},{},{},{},{},'{}',{},'{}','{}',{},'{}',{},'{}',{},'{}')",
        kind, actor, item, count, copper, copper, source.type, source.entry, source.guid,
        source.name, source.map, source.mapName, source.zone, source.zoneName, source.area, source.areaName);
}

struct MoneySnapshot
{
    LootSource source;
    std::vector<std::pair<ObjectGuid, uint32>> recipients;
};
// Both callbacks run synchronously within the same loot-money handler.
thread_local std::unordered_map<uint32, MoneySnapshot> moneySnapshots;

struct MailAttachmentSnapshot
{
    uint32 entry, quantity;
};
struct MailSendSnapshot
{
    uint32 receiver, money, cod, balance;
    std::string subject;
    std::unordered_map<uint32, MailAttachmentSnapshot> items;
};
std::mutex mailMutex;
std::unordered_map<uint32, MailSendSnapshot> mailPending;

void AuctionEvent(AuctionEntry* auction, char const* kind, uint32 proceeds = 0)
{
    uint32 owner = auction->owner.GetCounter();
    uint32 buyer = auction->bidder.GetCounter();
    if (!ready || (!IsTracked(owner) && !IsTracked(buyer)))
        return;
    // This identity survives restart and distinguishes native auction-ID reuse.
    std::string key = std::to_string(auction->Id) + "-" +
        std::to_string(auction->item_guid.GetCounter()) + "-" +
        std::to_string(owner) + "-" + std::to_string(auction->expire_time);
    CharacterDatabase.Execute(
        "INSERT IGNORE INTO transaction_journal "
        "(event_uuid,event_kind,listing_key,actor_guid,counterparty_guid,item_id,quantity,"
        "auction_id,house_id,start_bid,buyout,gross,deposit,fee,proceeds,actor_ahbot,counterparty_ahbot) "
        "VALUES(UUID(),'{}','{}',{},{},{},{},{},{},{},{},{},{},{},{},{},{})",
        kind, key, owner, buyer, auction->item_template, auction->itemCount,
        auction->Id, auction->GetHouseId(), auction->startbid, auction->buyout,
        auction->bid, auction->deposit, auction->GetAuctionCut(), proceeds,
        marketBots.count(owner) != 0, marketBots.count(buyer) != 0);
}
}

class TransactionJournalWorld : public WorldScript
{
public:
    TransactionJournalWorld() : WorldScript("TransactionJournalWorld") { }
    uint32 elapsed = 0;
    std::list<TransactionCallback> deletions;
    void ProcessDeletions()
    {
        for (auto it = deletions.begin(); it != deletions.end();)
            if (it->InvokeIfReady())
                it = deletions.erase(it);
            else
                ++it;
    }
    void ReadDeletionRequests()
    {
        namespace fs = std::filesystem;
        fs::path folder = fs::path(sConfigMgr->GetOption<std::string>(
            "TransactionJournal.MonitoringFile", "/logs/transaction-monitoring.txt")).parent_path()
            / "transaction-journal-requests";
        std::error_code error;
        fs::directory_iterator entries(folder, error);
        if (error)
            return;
        uint32 processed = 0;
        for (auto const& entry : entries)
        {
            if (entry.path().extension() != ".request" || processed++ >= 10)
                continue;
            uint64 guid = 0, cutoff = 0;
            std::ifstream request(entry.path());
            if (!(request >> guid >> cutoff) || !guid || guid > UINT32_MAX || !cutoff)
                continue;
            auto working = entry.path();
            working.replace_extension(".working");
            fs::rename(entry.path(), working, error);
            if (error)
                continue;
            auto done = working;
            done.replace_extension(".done");
            auto transaction = CharacterDatabase.BeginTransaction();
            transaction->Append("DELETE FROM transaction_journal WHERE id <= {} "
                "AND (actor_guid = {} OR counterparty_guid = {})", cutoff, guid, guid);
            deletions.emplace_back(CharacterDatabase.AsyncCommitTransaction(transaction));
            deletions.back().AfterComplete([working, done](bool success)
            {
                auto temporary = done;
                temporary.replace_extension(".result");
                std::ofstream result(temporary);
                result << (success ? "ok" : "failed");
                result.close();
                std::error_code ignored;
                std::filesystem::rename(temporary, done, ignored);
                if (!ignored)
                    std::filesystem::remove(working, ignored);
            });
        }
    }
    void OnUpdate(uint32 diff) override
    {
        ProcessDeletions();
        elapsed += diff;
        if (elapsed >= 5000)
        {
            elapsed = 0;
            ReloadMonitoring();
            if (ready)
                ReadDeletionRequests();
        }
    }
    void OnStartup() override
    {
        namespace fs = std::filesystem;
        fs::path folder = fs::path(sConfigMgr->GetOption<std::string>(
            "TransactionJournal.MonitoringFile", "/logs/transaction-monitoring.txt")).parent_path()
            / "transaction-journal-requests";
        std::error_code error;
        fs::directory_iterator entries(folder, error);
        if (!error)
            for (auto const& entry : entries)
                if (entry.path().extension() == ".working")
                {
                    auto request = entry.path();
                    request.replace_extension(".request");
                    fs::rename(entry.path(), request, error);
                }
        ReloadMonitoring();
        LoadGuids("TransactionJournal.AHBotGUIDs", marketBots);
        ready = sConfigMgr->GetOption<bool>("TransactionJournal.Enable", false)
            && bool(CharacterDatabase.Query("SELECT 1 FROM transaction_journal LIMIT 1")
                || CharacterDatabase.Query("SHOW TABLES LIKE 'transaction_journal'"));
        LOG_INFO("module", "TransactionJournal: enabled={}, tracked characters={}. Records are hook observations.", ready.load(), tracked.size());
    }
};

class TransactionJournalAuction : public AuctionHouseScript
{
public:
    TransactionJournalAuction() : AuctionHouseScript("TransactionJournalAuction") { }
    void OnAuctionAdd(AuctionHouseObject*, AuctionEntry* auction) override
    {
        // Core startup loads existing auctions before OnStartup. They are not new listings.
        AuctionEvent(auction, "listing_observed");
    }
    void OnAuctionRemove(AuctionHouseObject*, AuctionEntry* auction) override
    {
        AuctionEvent(auction, "removal_observed");
    }
    void OnBeforeAuctionHouseMgrSendAuctionSuccessfulMail(AuctionHouseMgr*, AuctionEntry* auction,
        Player*, uint32&, uint32& profit, bool&, bool&, bool& sendMail) override
    {
        if (sendMail)
            AuctionEvent(auction, "sale_settlement_observed", profit);
    }
    void OnBeforeAuctionHouseMgrSendAuctionExpiredMail(AuctionHouseMgr*, AuctionEntry* auction,
        Player*, uint32&, bool&, bool& sendMail) override
    {
        if (sendMail)
            AuctionEvent(auction, "expiration_observed");
    }
};

class TransactionJournalPlayer : public PlayerScript
{
    struct VendorState
    {
        std::unordered_set<uint32> buyback;
        uint32 moneyBefore = 0;
        bool purchasing = false;
    };
    std::mutex vendorMutex;
    std::unordered_map<uint32, VendorState> pending;
public:
    TransactionJournalPlayer() : PlayerScript("TransactionJournalPlayer") { }
    void OnPlayerBeforeLootMoney(Player* player, Loot*) override
    {
        if (!ready)
            return;
        MoneySnapshot snapshot;
        snapshot.source = ResolveLootSource(player, player->GetLootGUID());
        auto capture = [&snapshot](Player* recipient)
        {
            if (recipient && IsTracked(recipient->GetGUID().GetCounter()))
                snapshot.recipients.emplace_back(recipient->GetGUID(), recipient->GetMoney());
        };
        if (Group* group = player->GetGroup())
        {
            for (GroupReference* member = group->GetFirstMember(); member; member = member->next())
                if (Player* recipient = member->GetSource())
                    if (player->IsAtLootRewardDistance(recipient))
                        capture(recipient);
        }
        else
            capture(player);
        moneySnapshots[player->GetGUID().GetCounter()] = std::move(snapshot);
    }
    bool OnPlayerCanSendMail(Player* player, ObjectGuid receiver, ObjectGuid, std::string& subject,
        std::string&, uint32 money, uint32 cod, Item* item) override
    {
        uint32 actor = player->GetGUID().GetCounter();
        if (!ready || (!IsTracked(actor) && !IsTracked(receiver.GetCounter())))
            return true;
        std::lock_guard<std::mutex> lock(mailMutex);
        auto found = mailPending.find(actor);
        if (found == mailPending.end() || found->second.receiver != receiver.GetCounter()
            || found->second.subject != subject || found->second.money != money
            || found->second.cod != cod)
        {
            mailPending[actor] = {receiver.GetCounter(), money, cod, player->GetMoney(), subject, {}};
        }
        if (item)
            mailPending[actor].items[item->GetGUID().GetCounter()] = {item->GetEntry(), item->GetCount()};
        return true;
    }
    bool OnPlayerCanSellItem(Player* player, Item*, Creature*) override
    {
        uint32 guid = player->GetGUID().GetCounter();
        if (!ready || !IsTracked(guid))
            return true;
        VendorState state;
        for (uint32 slot = BUYBACK_SLOT_START; slot < BUYBACK_SLOT_END; ++slot)
            if (Item* item = player->GetItemFromBuyBackSlot(slot))
                state.buyback.insert(item->GetGUID().GetCounter());
        std::lock_guard<std::mutex> lock(vendorMutex);
        pending[guid] = std::move(state);
        return true;
    }
    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        uint32 guid = player->GetGUID().GetCounter();
        if (!ready || !IsTracked(guid) || amount <= 0)
            return;
        std::lock_guard<std::mutex> lock(vendorMutex);
        auto state = pending.find(guid);
        if (state == pending.end() || state->second.purchasing)
            return;
        for (uint32 slot = BUYBACK_SLOT_START; slot < BUYBACK_SLOT_END; ++slot)
        {
            Item* item = player->GetItemFromBuyBackSlot(slot);
            if (item && !state->second.buyback.count(item->GetGUID().GetCounter()))
            {
                ItemEvent("vendor_sale_observed", guid, item->GetEntry(), item->GetCount(), amount);
                break;
            }
        }
        pending.erase(state);
    }
    void OnPlayerBeforeBuyItemFromVendor(Player* player, ObjectGuid, uint32, uint32&, uint8, uint8, uint8) override
    {
        uint32 guid = player->GetGUID().GetCounter();
        if (!ready || !IsTracked(guid))
            return;
        VendorState state;
        state.moneyBefore = player->GetMoney();
        state.purchasing = true;
        std::lock_guard<std::mutex> lock(vendorMutex);
        pending[guid] = std::move(state);
    }
    void OnPlayerAfterStoreOrEquipNewItem(Player* player, uint32, Item* item, uint8 count,
        uint8, uint8, ItemTemplate const* prototype, Creature*, VendorItem const*, bool) override
    {
        uint32 guid = player->GetGUID().GetCounter();
        std::lock_guard<std::mutex> lock(vendorMutex);
        auto state = pending.find(guid);
        if (state == pending.end())
            return;
        if (ready && IsTracked(guid) && item && prototype && state->second.purchasing)
        {
            uint32 cost = state->second.moneyBefore >= player->GetMoney()
                ? state->second.moneyBefore - player->GetMoney() : 0;
            ItemEvent("vendor_purchase", guid, item->GetEntry(), prototype->BuyCount * count, cost);
        }
        pending.erase(state);
    }
    void OnPlayerUpdate(Player* player, uint32) override
    {
        {
            std::lock_guard<std::mutex> lock(mailMutex);
            mailPending.erase(player->GetGUID().GetCounter());
        }
        std::lock_guard<std::mutex> lock(vendorMutex);
        pending.erase(player->GetGUID().GetCounter());
    }
    void OnPlayerLogout(Player* player) override
    {
        {
            std::lock_guard<std::mutex> lock(mailMutex);
            mailPending.erase(player->GetGUID().GetCounter());
        }
        std::lock_guard<std::mutex> lock(vendorMutex);
        pending.erase(player->GetGUID().GetCounter());
    }
    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid source) override
    {
        uint32 guid = player->GetGUID().GetCounter();
        if (!ready || !IsTracked(guid) || !item || !count)
            return;
        LootEvent("loot", guid, item->GetEntry(), count, 0, ResolveLootSource(player, source));
    }
};

class TransactionJournalLoot : public LootScript
{
public:
    TransactionJournalLoot() : LootScript("TransactionJournalLoot") { }
    void OnLootMoney(Player* player, uint32) override
    {
        auto found = moneySnapshots.find(player->GetGUID().GetCounter());
        if (found == moneySnapshots.end())
            return;
        MoneySnapshot snapshot = std::move(found->second);
        moneySnapshots.erase(found);
        for (auto const& recipient : snapshot.recipients)
            if (Player* member = ObjectAccessor::FindConnectedPlayer(recipient.first))
                if (member->GetMoney() > recipient.second && IsTracked(recipient.first.GetCounter()))
                    LootEvent("loot_money", recipient.first.GetCounter(), 0, 0,
                        member->GetMoney() - recipient.second, snapshot.source);
    }
};

class TransactionJournalMail : public MailScript
{
public:
    TransactionJournalMail() : MailScript("TransactionJournalMail") { }
    void OnBeforeMailDraftSendMailTo(MailDraft* draft, MailReceiver const& receiver,
        MailSender const& sender, MailCheckMask&, uint32&, uint32&, bool&, bool& sendMail) override
    {
        if (!ready || !sendMail || sender.GetMailMessageType() != MAIL_NORMAL)
            return;
        uint32 actor = sender.GetSenderId();
        std::lock_guard<std::mutex> lock(mailMutex);
        auto found = mailPending.find(actor);
        if (found == mailPending.end())
            return;
        MailSendSnapshot snapshot = std::move(found->second);
        mailPending.erase(found);
        Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(actor));
        uint32 postage = 30 * std::max<std::size_t>(1, snapshot.items.size());
        if (!player || snapshot.receiver != receiver.GetPlayerGUIDLow()
            || snapshot.subject != draft->GetSubject() || snapshot.money != draft->GetMoney()
            || snapshot.balance < player->GetMoney()
            || uint64(snapshot.balance - player->GetMoney()) != uint64(postage) + draft->GetMoney())
            return; // Returns and mail generated directly by scripts do not pay this postage.
        std::ostringstream attachments;
        attachments << "[";
        uint32 quantity = 0;
        bool first = true;
        for (auto const& pair : snapshot.items)
        {
            if (!first) attachments << ",";
            first = false;
            attachments << "{\"item_id\":" << pair.second.entry << ",\"quantity\":" << pair.second.quantity << "}";
            quantity += pair.second.quantity;
        }
        attachments << "]";
        CharacterDatabase.EscapeString(snapshot.subject);
        CharacterDatabase.Execute(
            "INSERT INTO transaction_journal "
            "(event_uuid,event_kind,actor_guid,counterparty_guid,item_id,quantity,gross,fee,"
            "mail_postage,mail_money,mail_cod,mail_subject,mail_items) "
            "VALUES(UUID(),'mail_sent_observed',{},{},0,{},{},{},{},{},{},'{}','{}')",
            actor, snapshot.receiver, quantity, uint64(postage) + draft->GetMoney(), postage,
            postage, draft->GetMoney(), draft->GetCOD(), snapshot.subject, attachments.str());
    }
};

void Addmod_transaction_journalScripts()
{
    new TransactionJournalWorld();
    new TransactionJournalAuction();
    new TransactionJournalPlayer();
    new TransactionJournalLoot();
    new TransactionJournalMail();
}
