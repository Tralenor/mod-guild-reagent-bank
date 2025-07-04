
#ifndef AZEROTHCORE_REAGENTBANK_H
#define AZEROTHCORE_REAGENTBANK_H
#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "Item.h"
#include "ItemTemplate.h"
#include <map>

#define MAX_OPTIONS 23
#define MAX_PAGE_NUMBER 700 // Values higher than this are considered Item IDs
#define NPC_TEXT_ID 4259 // Pre-existing NPC text
#define NPC_TEXT_ID_FOR_EXPLANATION 90000001 // Pre-existing NPC text
#define STORAGE_COST 10000000

enum GossipItemType : uint8
{
    DEPOSIT_ALL_REAGENTS = 16,
    MAIN_MENU = 17,
    EXPLANATION = 18,
    BUY_MORE_SPACE= 19,
};


#endif //AZEROTHCORE_REAGENTBANK_H
