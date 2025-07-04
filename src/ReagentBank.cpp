/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "ReagentBank.h"
#include "Guild.h"

struct DepositCandidate {
    Item *item;
    uint32 bag;
    uint32 slot;
    uint32 count;
};

static bool isPlayerGuildMaster(Player * player){

    auto guild = player->GetGuild();
    if(!guild){
        return false;
    }
    return player->GetGUID() == guild->GetLeaderGUID();
}

// Add player scripts
class npc_reagent_banker : public CreatureScript {
private:
    std::string GetItemLink(uint32 entry, WorldSession *session) const {
        int loc_idx = session->GetSessionDbLocaleIndex();
        const ItemTemplate *temp = sObjectMgr->GetItemTemplate(entry);
        std::string name = temp->Name1;
        if (ItemLocale const *il = sObjectMgr->GetItemLocale(temp->ItemId))
            ObjectMgr::GetLocaleString(il->Name, loc_idx, name);

        std::ostringstream oss;
        oss << "|c" << std::hex << ItemQualityColors[temp->Quality] << std::dec <<
            "|Hitem:" << temp->ItemId << ":" <<
            (uint32) 0 << "|h[" << name << "]|h|r";

        return oss.str();
    }

    std::string GetItemIcon(uint32 entry, uint32 width, uint32 height, int x, int y) const {
        std::ostringstream ss;
        ss << "|TInterface";
        const ItemTemplate *temp = sObjectMgr->GetItemTemplate(entry);
        const ItemDisplayInfoEntry *dispInfo = NULL;
        if (temp) {
            dispInfo = sItemDisplayInfoStore.LookupEntry(temp->DisplayInfoID);
            if (dispInfo)
                ss << "/ICONS/" << dispInfo->inventoryIcon;
        }
        if (!dispInfo)
            ss << "/InventoryItems/WoWUnknownItem01";
        ss << ":" << width << ":" << height << ":" << x << ":" << y << "|t";
        return ss.str();
    }

    void WithdrawItem(Player *player, uint32 entry) {
        uint32 guildId = player->GetGuildId();

        // Query current amount first (not required for correctness, just to check limits)
        std::string query = "SELECT amount FROM custom_guild_reagent_bank WHERE guild_id = " +
                            std::to_string(guildId) + " AND item_entry = " + std::to_string(entry);

        QueryResult result = CharacterDatabase.Query(query);
        if (!result)
            return;

        uint32 storedAmount = (*result)[0].Get<uint32>();
        const ItemTemplate *temp = sObjectMgr->GetItemTemplate(entry);
        if (!temp)
            return;

        uint32 stackSize = temp->GetMaxStackSize();
        uint32 amountToWithdraw = std::min(storedAmount, stackSize);

        ItemPosCountVec dest;
        InventoryResult msg = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, entry, amountToWithdraw);
        if (msg != EQUIP_ERR_OK) {
            player->SendEquipError(msg, nullptr, nullptr, entry);
            return;
        }

        auto trans = CharacterDatabase.BeginTransaction();

        if (storedAmount <= stackSize) {
            // Just delete the row (clean)
            trans->Append("DELETE FROM custom_guild_reagent_bank WHERE guild_id = {} AND item_entry = {}", guildId,
                          entry);
        } else {
            // UPSERT pattern: decrement amount atomically
            trans->Append(R"(
            INSERT INTO custom_guild_reagent_bank (guild_id, item_entry, item_subclass, amount)
            VALUES ({}, {}, {}, -{})
            ON DUPLICATE KEY UPDATE amount = amount + VALUES(amount)
        )",
                          guildId, entry, temp->SubClass, amountToWithdraw);
        }

        CharacterDatabase.CommitTransaction(trans);

        Item *item = player->StoreNewItem(dest, entry, true);
        player->SendNewItem(item, amountToWithdraw, true, false);
    }


    void DepositAllReagents(Player *player) {
        uint32 guildId = player->GetGuildId();

        std::map<uint32, uint32> entryToAmountMap;
        std::map<uint32, uint32> entryToSubclassMap;
        std::map<uint32, std::vector<DepositCandidate>> entryToCandidatesMap;

        // Gather eligible items from bags without destroying them
        auto collectItem = [&](Item *pItem, uint32 bag, uint32 slot) {
            if (!pItem)
                return;
            uint32 count = pItem->GetCount();
            ItemTemplate const *itemTemplate = pItem->GetTemplate();
            if (!itemTemplate)
                return;
            if (!(itemTemplate->Class == ITEM_CLASS_TRADE_GOODS || itemTemplate->Class == ITEM_CLASS_GEM) ||
                itemTemplate->GetMaxStackSize() == 1)
                return;

            uint32 itemEntry = itemTemplate->ItemId;
            uint32 itemSubclass = itemTemplate->SubClass;

            if (itemTemplate->Class == ITEM_CLASS_GEM)
                itemSubclass = ITEM_SUBCLASS_JEWELCRAFTING;

            entryToAmountMap[itemEntry] += count;
            entryToSubclassMap[itemEntry] = itemSubclass;
            entryToCandidatesMap[itemEntry].push_back({pItem, bag, slot, count});
        };

        for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
            collectItem(player->GetItemByPos(INVENTORY_SLOT_BAG_0, i), INVENTORY_SLOT_BAG_0, i);

        for (uint32 i = INVENTORY_SLOT_BAG_START; i < INVENTORY_SLOT_BAG_END; i++) {
            Bag *bag = player->GetBagByPos(i);
            if (!bag)
                continue;
            for (uint32 j = 0; j < bag->GetBagSize(); j++)
                collectItem(player->GetItemByPos(i, j), i, j);
        }

        if (entryToAmountMap.empty()) {
            ChatHandler(player->GetSession()).PSendSysMessage("Ihr habt keine Handwerksmaterialien zum Einlagern.");
            CloseGossipMenuFor(player);
            return;
        }

        // Get storage limit
        QueryResult spaceResult = CharacterDatabase.Query(
                "SELECT space FROM custom_guild_reagent_bank_size WHERE guild_id = {}", guildId);
        uint32 spaceLimit = spaceResult ? (*spaceResult)[0].Get<uint32>() : 0;

        if (spaceLimit == 0) {
            ChatHandler(player->GetSession()).PSendSysMessage(
                    "Eure Gilde hat keinen Lagerplatz. Bitte kauft zuerst Speicherplatz.");
            CloseGossipMenuFor(player);
            return;
        }

        // Get used space
        QueryResult usedResult = CharacterDatabase.Query(
                "SELECT SUM(amount) FROM custom_guild_reagent_bank WHERE guild_id = {}", guildId);
        uint32 usedSpace = (usedResult && !(*usedResult)[0].IsNull()) ? (*usedResult)[0].Get<uint32>() : 0;

        uint32 freeSpace = (spaceLimit > usedSpace) ? (spaceLimit - usedSpace) : 0;
        if (freeSpace == 0) {
            ChatHandler(player->GetSession()).PSendSysMessage("Euer Gildenlager ist voll.");
            CloseGossipMenuFor(player);
            return;
        }

        auto trans = CharacterDatabase.BeginTransaction();
        uint32 totalDeposited = 0;

        for (const auto &pair: entryToAmountMap) {
            uint32 itemEntry = pair.first;
            uint32 itemSubclass = entryToSubclassMap[itemEntry];
            const auto &candidates = entryToCandidatesMap[itemEntry];

            uint32 amountToStore = std::min(pair.second, freeSpace);
            if (amountToStore == 0)
                continue;

            trans->Append(R"(
            INSERT INTO custom_guild_reagent_bank (guild_id, item_entry, item_subclass, amount)
            VALUES ({}, {}, {}, {})
            ON DUPLICATE KEY UPDATE amount = amount + VALUES(amount)
        )", guildId, itemEntry, itemSubclass, amountToStore);

            // Destroy deposited items from inventory
            uint32 toRemove = amountToStore;
            for (const DepositCandidate &candidate: candidates) {
                if (toRemove == 0)
                    break;
                uint32 destroyCount = std::min(candidate.count, toRemove);
                player->DestroyItem(candidate.bag, candidate.slot, destroyCount);
                toRemove -= destroyCount;
            }

            totalDeposited += amountToStore;
            freeSpace -= amountToStore;
        }

        if (totalDeposited > 0) {
            CharacterDatabase.CommitTransaction(trans);
            auto message = std::format("Es wurden {} Handwerksmaterialien erfolgreich eingelagert.",totalDeposited);
            ChatHandler(player->GetSession()).PSendSysMessage(message);
        } else {
            ChatHandler(player->GetSession()).PSendSysMessage("Es war kein Platz im Lager verfügbar.");
        }

        CloseGossipMenuFor(player);
    }


public:
    npc_reagent_banker() : CreatureScript("npc_reagent_banker") {}

    bool OnGossipHello(Player *player, Creature *creature) override {
        if (!player->GetGuild()) {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Was könnt ihr für mich tun?", EXPLANATION, 0);
        } else {
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(4359, 30, 30, -18, 0) + "Teile",
                             ITEM_SUBCLASS_PARTS, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(4358, 30, 30, -18, 0) + "Sprengstoffe",
                             ITEM_SUBCLASS_EXPLOSIVES, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(4388, 30, 30, -18, 0) + "Geräte",
                             ITEM_SUBCLASS_DEVICES, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(1206, 30, 30, -18, 0) + "Juwelenschleifen",
                             ITEM_SUBCLASS_JEWELCRAFTING, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(2589, 30, 30, -18, 0) + "Stoff",
                             ITEM_SUBCLASS_CLOTH, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(2318, 30, 30, -18, 0) + "Leder",
                             ITEM_SUBCLASS_LEATHER, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(2772, 30, 30, -18, 0) + "Metall & Stein",
                             ITEM_SUBCLASS_METAL_STONE, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(12208, 30, 30, -18, 0) + "Fleisch",
                             ITEM_SUBCLASS_MEAT, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(2453, 30, 30, -18, 0) + "Kräuter",
                             ITEM_SUBCLASS_HERB, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(7068, 30, 30, -18, 0) + "Elementar",
                             ITEM_SUBCLASS_ELEMENTAL, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(10940, 30, 30, -18, 0) + "Verzauberkunst",
                             ITEM_SUBCLASS_ENCHANTING, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(23572, 30, 30, -18, 0) + "Nethermaterialien",
                             ITEM_SUBCLASS_MATERIAL, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(2604, 30, 30, -18, 0) + "Andere Handelswaren",
                             ITEM_SUBCLASS_TRADE_GOODS_OTHER, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(38682, 30, 30, -18, 0) + "Rüstungspergament",
                             ITEM_SUBCLASS_ARMOR_ENCHANTMENT, 0);
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, GetItemIcon(39349, 30, 30, -18, 0) + "Waffenpergament",
                             ITEM_SUBCLASS_WEAPON_ENCHANTMENT, 0);

            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Alle Handwerksmaterialien einlagern", DEPOSIT_ALL_REAGENTS, 0);
        }


        if (isPlayerGuildMaster(player)) {
            AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "Ich möchte für 1000 Gold 1000 zusätzlichen Lagerplatz erwerben",
                             BUY_MORE_SPACE, 0);
        }

        SendGossipMenuFor(player, NPC_TEXT_ID, creature->GetGUID());
        return true;
    }

    bool OnGossipSelect(Player *player, Creature *creature, uint32 item_subclass, uint32 gossipPageNumber) override {
        player->PlayerTalkClass->ClearMenus();
        if (item_subclass > MAX_PAGE_NUMBER) {
            // item_subclass is actually an item ID to withdraw
            // Get the actual item subclass from the template
            const ItemTemplate *temp = sObjectMgr->GetItemTemplate(item_subclass);
            WithdrawItem(player, item_subclass);
            if (temp->Class == ITEM_CLASS_GEM) {
                // Get back to ITEM_SUBCLASS_JEWELCRAFTING section when withdrawing gems
                ShowReagentItems(player, creature, ITEM_SUBCLASS_JEWELCRAFTING, gossipPageNumber);
            } else {
                ShowReagentItems(player, creature, temp->SubClass, gossipPageNumber);
            }
            return true;
        }
        if (item_subclass == DEPOSIT_ALL_REAGENTS) {
            DepositAllReagents(player);
            return true;
        } else if (item_subclass == MAIN_MENU) {
            OnGossipHello(player, creature);
            return true;
        } else if (item_subclass == EXPLANATION) {
            player->PlayerTalkClass->ClearMenus();
            SendGossipMenuFor(player, NPC_TEXT_ID_FOR_EXPLANATION, creature->GetGUID());
            return true;
        } else if (item_subclass == BUY_MORE_SPACE) {
            HandleBuyStorageOption(player,creature);
            return true;
        }
        else {
            ShowReagentItems(player, creature, item_subclass, gossipPageNumber);
            return true;
        }
    }

    void HandleBuyStorageOption(Player *player,Creature * creature) {
        if (!player->GetGuild() && !isPlayerGuildMaster(player)) {
            ChatHandler(player->GetSession()).PSendSysMessage(
                    "Du musst in einer Gilde (und dessen Gildenmeister) sein, um zusätzlichen Lagerplatz zu kaufen.");
            return;
        }

        if (player->HasEnoughMoney(STORAGE_COST)) {
            player->ModifyMoney(-STORAGE_COST);  // Subtract cost

            uint32 guildId = player->GetGuildId();

            CharacterDatabase.Query(R"(
            INSERT INTO custom_guild_reagent_bank_size (guild_id, space)
            VALUES ({}, 1000)
            ON DUPLICATE KEY UPDATE space = space + 1000
        )", guildId);

            ChatHandler(player->GetSession()).PSendSysMessage(
                    "Du hast erfolgreich 1000 zusätzlichen Lagerplatz für deine Gilde gekauft.");
        } else {
            ChatHandler(player->GetSession()).PSendSysMessage(
                    "Du hast nicht genug Gold. Es werden 1000 Gold zum Erwerben zusätlichen Lagerplatzes benötigt.");
        }

        player->PlayerTalkClass->ClearMenus();
        OnGossipHello(player,creature);
    }

    void ShowReagentItems(Player *player, Creature *creature, uint32 item_subclass, uint16 gossipPageNumber) {
        WorldSession *session = player->GetSession();
        std::string query = "SELECT item_entry, amount FROM custom_guild_reagent_bank WHERE guild_id = " +
                            std::to_string(player->GetGuildId()) + " AND item_subclass = " +
                            std::to_string(item_subclass) + " ORDER BY item_entry";
        session->GetQueryProcessor().AddCallback(
                CharacterDatabase.AsyncQuery(query).WithCallback([=, this](QueryResult result) {
                    uint32 startValue = (gossipPageNumber * (MAX_OPTIONS));
                    uint32 endValue = (gossipPageNumber + 1) * (MAX_OPTIONS) - 1;
                    std::map<uint32, uint32> entryToAmountMap;
                    std::vector<uint32> itemEntries;
                    if (result) {
                        do {
                            uint32 itemEntry = (*result)[0].Get<uint32>();
                            uint32 itemAmount = (*result)[1].Get<uint32>();
                            entryToAmountMap[itemEntry] = itemAmount;
                            itemEntries.push_back(itemEntry);
                        } while (result->NextRow());
                    }
                    for (uint32 i = startValue; i <= endValue; i++) {
                        if (itemEntries.empty() || i > itemEntries.size() - 1) {
                            break;
                        }
                        uint32 itemEntry = itemEntries.at(i);
                        AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG,
                                         GetItemIcon(itemEntry, 30, 30, -18, 0) + GetItemLink(itemEntry, session) +
                                         " (" + std::to_string(entryToAmountMap.find(itemEntry)->second) + ")",
                                         itemEntry, gossipPageNumber);
                    }
                    if (gossipPageNumber > 0) {
                        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Previous Page", item_subclass,
                                         gossipPageNumber - 1);
                    }
                    if (endValue < entryToAmountMap.size()) {
                        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Next Page", item_subclass, gossipPageNumber + 1);
                    }
                    AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG,
                                     "|TInterface/ICONS/Ability_Spy:30:30:-18:0|tBack...", MAIN_MENU, 0);
                    SendGossipMenuFor(player, NPC_TEXT_ID, creature->GetGUID());
                }));
    }
};

// Add all scripts in one
void AddSC_mod_reagent_bank() {
    new npc_reagent_banker();
}
