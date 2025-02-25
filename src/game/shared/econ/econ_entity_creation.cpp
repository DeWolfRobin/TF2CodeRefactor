//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Functions to generate items as full game entities.
//=============================================================================

#include "cbase.h"
#include "econ_entity_creation.h"
#include "utldict.h"
#include "filesystem.h"
#include "gamestringpool.h"
#include "KeyValues.h"
#include "attribute_manager.h"
#include "vgui/ILocalize.h"
#include "tier3/tier3.h"
#include "util_shared.h"

#ifdef TF_CLIENT_DLL
#include "c_tf_player.h"
#endif // TF_CLIENT_DLL

//----------------------------------------------------------------------------------
// Global item generation system instance.
//----------------------------------------------------------------------------------
CItemGeneration g_ItemGenerationSystem;
CItemGeneration* ItemGeneration(void)
{
    return &g_ItemGenerationSystem;
}

//-----------------------------------------------------------------------------
// Constructor
//-----------------------------------------------------------------------------
CItemGeneration::CItemGeneration(void)
{
}

//-----------------------------------------------------------------------------
// GenerateRandomItem: Returns a randomly chosen item entity based on criteria.
//-----------------------------------------------------------------------------
CBaseEntity* CItemGeneration::GenerateRandomItem(CItemSelectionCriteria* pCriteria, const Vector& vecOrigin, const QAngle& vecAngles, const char* pszOverrideClassName)
{
    entityquality_t iQuality;
    int iChosenItem = ItemSystem()->GenerateRandomItem(pCriteria, &iQuality);
    if (iChosenItem == INVALID_ITEM_DEF_INDEX)
        return nullptr;

    return SpawnItem(iChosenItem, vecOrigin, vecAngles, pCriteria->GetItemLevel(), iQuality, pszOverrideClassName);
}

//-----------------------------------------------------------------------------
// GenerateItemFromDefIndex: Spawns an item given its definition index.
//-----------------------------------------------------------------------------
CBaseEntity* CItemGeneration::GenerateItemFromDefIndex(int iDefIndex, const Vector& vecOrigin, const QAngle& vecAngles)
{
    return SpawnItem(iDefIndex, vecOrigin, vecAngles, 1, AE_UNIQUE, nullptr);
}

//-----------------------------------------------------------------------------
// GenerateItemFromScriptData: Spawns an item based on pre-populated item data.
//-----------------------------------------------------------------------------
CBaseEntity* CItemGeneration::GenerateItemFromScriptData(const CEconItemView* pData, const Vector& vecOrigin, const QAngle& vecAngles, const char* pszOverrideClassName)
{
    return SpawnItem(pData, vecOrigin, vecAngles, pszOverrideClassName);
}

//-----------------------------------------------------------------------------
// GenerateBaseItem: Generates the base item for a class's loadout slot.
//-----------------------------------------------------------------------------
CBaseEntity* CItemGeneration::GenerateBaseItem(struct baseitemcriteria_t* pCriteria)
{
    int iChosenItem = ItemSystem()->GenerateBaseItem(pCriteria);
    if (iChosenItem == INVALID_ITEM_DEF_INDEX)
        return nullptr;

    return SpawnItem(iChosenItem, vec3_origin, vec3_angle, 1, AE_NORMAL, nullptr);
}

//-----------------------------------------------------------------------------
// SpawnItem (by definition index): Creates a new entity based on item definition index.
//-----------------------------------------------------------------------------
CBaseEntity* CItemGeneration::SpawnItem(int iChosenItem, const Vector& vecAbsOrigin, const QAngle& vecAbsAngles, int iItemLevel, entityquality_t entityQuality, const char* pszOverrideClassName)
{
    CEconItemDefinition* pData = ItemSystem()->GetStaticDataForItemByDefIndex(iChosenItem);
    if (!pData)
        return nullptr;

    CBaseEntity* pItem = nullptr;
    // Try override classname first.
    if (pszOverrideClassName)
        pItem = CreateEntityByName(pszOverrideClassName);

    // Fall back to the item class from the definition.
    if (!pItem)
    {
        pszOverrideClassName = pData->GetItemClass();
        if (!pszOverrideClassName)
            return nullptr;
        pItem = CreateEntityByName(pszOverrideClassName);
    }
    if (!pItem)
        return nullptr;

    IHasAttributes* pItemInterface = GetAttribInterface(pItem);
    Assert(pItemInterface);
    if (pItemInterface)
    {
        CEconItemView* pScriptItem = pItemInterface->GetAttributeContainer()->GetItem();
        pScriptItem->Init(iChosenItem, entityQuality, iItemLevel, false);
    }
    return PostSpawnItem(pItem, pItemInterface, vecAbsOrigin, vecAbsAngles);
}

//-----------------------------------------------------------------------------
// SpawnItem (by item data): Creates a new entity using pre-initialized item data.
//-----------------------------------------------------------------------------
CBaseEntity* CItemGeneration::SpawnItem(const CEconItemView* pData, const Vector& vecAbsOrigin, const QAngle& vecAbsAngles, const char* pszOverrideClassName)
{
    if (!pData->GetStaticData())
        return nullptr;

    if (!pszOverrideClassName)
        pszOverrideClassName = pData->GetStaticData()->GetItemClass();

    if (!pszOverrideClassName)
        return nullptr;

    CBaseEntity* pItem = CreateEntityByName(pszOverrideClassName);
    if (!pItem)
        return nullptr;

    IHasAttributes* pItemInterface = GetAttribInterface(pItem);
    Assert(pItemInterface);
    if (pItemInterface)
    {
        pItemInterface->GetAttributeContainer()->SetItem(pData);
    }
    return PostSpawnItem(pItem, pItemInterface, vecAbsOrigin, vecAbsAngles);
}

//-----------------------------------------------------------------------------
// PostSpawnItem: Finalizes the spawned item (origin/angles, spawn, activate).
//-----------------------------------------------------------------------------
CBaseEntity* CItemGeneration::PostSpawnItem(CBaseEntity* pItem, IHasAttributes* pItemInterface, const Vector& vecAbsOrigin, const QAngle& vecAbsAngles)
{
#ifdef CLIENT_DLL
    const char* pszPlayerModel = nullptr;
    if (pItemInterface)
    {
        CEconItemView* pScriptItem = pItemInterface->GetAttributeContainer()->GetItem();
        int iClass = 0, iTeam = 0;
#ifdef TF_CLIENT_DLL
        C_TFPlayer* pTFPlayer = ToTFPlayer(GetPlayerByAccountID(pScriptItem->GetAccountID()));
        if (pTFPlayer)
        {
            iClass = pTFPlayer->GetPlayerClass()->GetClassIndex();
            iTeam = pTFPlayer->GetTeamNumber();
        }
#endif
        pszPlayerModel = pScriptItem->GetPlayerDisplayModel(iClass, iTeam);
    }
    if (!pItem->InitializeAsClientEntity(pszPlayerModel, RENDER_GROUP_OPAQUE_ENTITY))
        return nullptr;
#endif

    pItem->SetAbsOrigin(vecAbsOrigin);
    pItem->SetAbsAngles(vecAbsAngles);
    pItem->Spawn();
    pItem->Activate();
    return pItem;
}
