//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#include "cbase.h"
#include "econ_entity_creation.h"
#include "vgui/ILocalize.h"
#include "tier3/tier3.h"

#if defined( CLIENT_DLL )
#define UTIL_VarArgs VarArgs
#include "econ_item_inventory.h"
#include "model_types.h"
#include "eventlist.h"
#include "networkstringtable_clientdll.h"
#include "cdll_util.h"
// Include inventory header so that CTFPlayerInventory is defined.
#include "tf_item_inventory.h"
#if defined(TF_CLIENT_DLL)
#include "c_tf_player.h"
#include "tf_gamerules.h"
#include "c_playerresource.h"
#include "tf_shareddefs.h"
#endif
#else
#include "activitylist.h"
#if defined(TF_DLL)
#include "tf_player.h"
#endif
#endif

// Define ARRAYSIZE macro if not defined.
#ifndef ARRAYSIZE
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))
#endif

// Define g_modelWhiteList and Halloween model macros for client builds.
#if defined(TF_CLIENT_DLL)
extern const char* g_modelWhiteList[]; // Forward-declaration; defined elsewhere.
#define HALLOWEEN_KART_MODEL "models/player/items/taunts/bumpercar/parts/bumpercar.mdl"
#define HALLOWEEN_KART_CAGE_MODEL "models/props_halloween/bumpercar_cage.mdl"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

IMPLEMENT_NETWORKCLASS_ALIASED(EconEntity, DT_EconEntity)
IMPLEMENT_NETWORKCLASS_ALIASED(BaseAttributableItem, DT_BaseAttributableItem)

#if defined( CLIENT_DLL )
bool ParseItemKeyvalue(void* pObject, typedescription_t* pFields, int iNumFields, const char* szKeyName, const char* szValue);
#endif

#if defined(_DEBUG)
extern ConVar item_debug;
extern ConVar item_debug_validation;
#endif

#if !defined( CLIENT_DLL )
#define DEFINE_ECON_ENTITY_NETWORK_TABLE() \
        SendPropDataTable( SENDINFO_DT( m_AttributeManager ), &REFERENCE_SEND_TABLE(DT_AttributeContainer) ),
#else
#define DEFINE_ECON_ENTITY_NETWORK_TABLE() \
        RecvPropDataTable( RECVINFO_DT( m_AttributeManager ), 0, &REFERENCE_RECV_TABLE(DT_AttributeContainer) ),
#endif

BEGIN_NETWORK_TABLE(CEconEntity, DT_EconEntity)
DEFINE_ECON_ENTITY_NETWORK_TABLE()
#if defined(TF_DLL)
SendPropBool(SENDINFO(m_bValidatedAttachedEntity)),
#elif defined(TF_CLIENT_DLL)
RecvPropBool(RECVINFO(m_bValidatedAttachedEntity)),
#endif
END_NETWORK_TABLE()

BEGIN_DATADESC(CEconEntity)
END_DATADESC()

BEGIN_NETWORK_TABLE(CBaseAttributableItem, DT_BaseAttributableItem)
DEFINE_ECON_ENTITY_NETWORK_TABLE()
END_NETWORK_TABLE()

BEGIN_DATADESC(CBaseAttributableItem)
END_DATADESC()

#ifdef GAME_DLL
BEGIN_ENT_SCRIPTDESC(CEconEntity, CBaseAnimating, "Econ Entity")
DEFINE_SCRIPTFUNC(AddAttribute, "Add an attribute to the entity")
DEFINE_SCRIPTFUNC(RemoveAttribute, "Remove an attribute to the entity")
DEFINE_SCRIPTFUNC(ReapplyProvision, "Flush any attribute changes we provide onto our owner")
DEFINE_SCRIPTFUNC_NAMED(ScriptGetAttribute, "GetAttribute", "Get an attribute float from the entity")
END_SCRIPTDESC();
#endif

#ifdef TF_CLIENT_DLL
extern ConVar cl_flipviewmodels;
#endif

#ifdef CLIENT_DLL
//-----------------------------------------------------------------------------
// DrawEconEntityAttachedModels: Draws additional models attached to the econ entity.
//-----------------------------------------------------------------------------
void DrawEconEntityAttachedModels(CBaseAnimating* pEnt, CEconEntity* pAttachedModelSource, const ClientModelRenderInfo_t* pInfo, int iMatchDisplayFlags)
{
#ifndef DOTA_DLL
	if (!pEnt || !pAttachedModelSource || !pInfo)
		return;

	IMaterial* pMaterialOverride = nullptr;
	OverrideType_t nMaterialOverrideType = OVERRIDE_NORMAL;
	if (pInfo->flags & STUDIO_NO_OVERRIDE_FOR_ATTACH)
	{
		modelrender->GetMaterialOverride(&pMaterialOverride, &nMaterialOverrideType);
		modelrender->ForcedMaterialOverride(nullptr, nMaterialOverrideType);
	}

	for (int i = 0; i < pAttachedModelSource->m_vecAttachedModels.Size(); i++)
	{
		const AttachedModelData_t& attachedModel = pAttachedModelSource->m_vecAttachedModels[i];
		if (attachedModel.m_pModel && (attachedModel.m_iModelDisplayFlags & iMatchDisplayFlags))
		{
			ClientModelRenderInfo_t infoAttached = *pInfo;
			infoAttached.pRenderable = pEnt;
			infoAttached.instance = MODEL_INSTANCE_INVALID;
			infoAttached.entity_index = pEnt->index;
			infoAttached.pModel = attachedModel.m_pModel;
			infoAttached.pModelToWorld = &infoAttached.modelToWorld;
			AngleMatrix(infoAttached.angles, infoAttached.origin, infoAttached.modelToWorld);
			DrawModelState_t state;
			matrix3x4_t* pBoneToWorld;
			bool bMarkAsDrawn = modelrender->DrawModelSetup(infoAttached, &state, nullptr, &pBoneToWorld);
			pEnt->DoInternalDrawModel(&infoAttached, (bMarkAsDrawn && (infoAttached.flags & STUDIO_RENDER)) ? &state : nullptr, pBoneToWorld);
		}
	}

	if (pMaterialOverride != nullptr)
		modelrender->ForcedMaterialOverride(pMaterialOverride, nMaterialOverrideType);
#endif
}
#endif // CLIENT_DLL

//-----------------------------------------------------------------------------
// CEconEntity constructor.
//-----------------------------------------------------------------------------
CEconEntity::CEconEntity()
{
	m_pAttributes = this;
	EnableDynamicModels();
#ifdef GAME_DLL
	m_iOldOwnerClass = 0;
#endif
#if defined(TF_DLL) || defined(TF_CLIENT_DLL)
	m_bValidatedAttachedEntity = false;
#endif
#ifdef CLIENT_DLL
	m_flFlexDelayTime = 0.0f;
	m_flFlexDelayedWeight = nullptr;
	m_cFlexDelayedWeight = 0;
	m_iNumOwnerValidationRetries = 0;
#endif
}

//-----------------------------------------------------------------------------
// CEconEntity destructor.
//-----------------------------------------------------------------------------
CEconEntity::~CEconEntity()
{
#ifdef CLIENT_DLL
	SetParticleSystemsVisible(PARTICLE_SYSTEM_STATE_NOT_VISIBLE);
	delete[] m_flFlexDelayedWeight;
#endif
}

//-----------------------------------------------------------------------------
// OnNewModel: Called when a new model is set; handles flex and bodygroup updates.
//-----------------------------------------------------------------------------
CStudioHdr* CEconEntity::OnNewModel()
{
	CStudioHdr* hdr = BaseClass::OnNewModel();

#ifdef GAME_DLL
	if (hdr && m_iOldOwnerClass > 0)
	{
		CEconItemView* pItem = GetAttributeContainer()->GetItem();
		if (pItem && pItem->IsValid() && pItem->GetStaticData()->UsesPerClassBodygroups(GetTeamNumber()))
		{
			SetBodygroup(1, m_iOldOwnerClass - 1);
		}
	}
#endif

#ifdef TF_CLIENT_DLL
	m_bValidatedOwner = false;
	C_TFPlayer* pPlayer = ToTFPlayer(GetOwnerEntity());
	if (pPlayer)
	{
		pPlayer->SetBodygroupsDirty();
	}
	delete[] m_flFlexDelayedWeight;
	m_flFlexDelayedWeight = nullptr;
	m_cFlexDelayedWeight = 0;
	if (hdr && hdr->numflexcontrollers())
	{
		m_cFlexDelayedWeight = hdr->numflexcontrollers();
		m_flFlexDelayedWeight = new float[m_cFlexDelayedWeight];
		memset(m_flFlexDelayedWeight, 0, sizeof(float) * m_cFlexDelayedWeight);
		C_BaseFlex::LinkToGlobalFlexControllers(hdr);
	}
#endif

	return hdr;
}

//-----------------------------------------------------------------------------
// InitializeAttributes: Sets up attributes for the econ entity.
//-----------------------------------------------------------------------------
void CEconEntity::InitializeAttributes(void)
{
	m_AttributeManager.InitializeAttributes(this);
	m_AttributeManager.SetProviderType(PROVIDER_WEAPON);
#ifdef CLIENT_DLL
	CUtlVector<const attachedparticlesystem_t*> vecParticles;
	GetEconParticleSystems(&vecParticles);
	m_bHasParticleSystems = (vecParticles.Count() > 0);
	if (!m_bClientside)
		return;
#else
	m_AttributeManager.GetItem()->InitNetworkedDynamicAttributesForDemos();
#endif
}

//-----------------------------------------------------------------------------
// DebugDescribe: Prints debug information about this econ entity.
//-----------------------------------------------------------------------------
void CEconEntity::DebugDescribe(void)
{
	CEconItemView* pScriptItem = GetAttributeContainer()->GetItem();
	Msg("============================================\n");
	char tempstr[1024];
	// FIXME: ILocalize::ConvertUnicodeToANSI( pScriptItem->GetItemName(), tempstr, sizeof(tempstr) );
	const char* pszQualityString = EconQuality_GetQualityString((EEconItemQuality)pScriptItem->GetItemQuality());
	Msg("%s \"%s\" (level %d)\n", pszQualityString ? pszQualityString : "[unknown]", tempstr, pScriptItem->GetItemLevel());
	// FIXME: ILocalize::ConvertUnicodeToANSI( pScriptItem->GetAttributeDescription(), tempstr, sizeof(tempstr) );
	Msg("%s", tempstr);
	Msg("\n============================================\n");
}

//-----------------------------------------------------------------------------
// UpdateOnRemove: Called when the entity is removed.
//-----------------------------------------------------------------------------
void CEconEntity::UpdateOnRemove(void)
{
	SetOwnerEntity(nullptr);
	ReapplyProvision();
	BaseClass::UpdateOnRemove();
}

//-----------------------------------------------------------------------------
// ReapplyProvision: Updates attribute provider links when the owner changes.
//-----------------------------------------------------------------------------
void CEconEntity::ReapplyProvision(void)
{
#ifdef GAME_DLL
	UpdateModelToClass();
#endif

	CBaseEntity* pNewOwner = GetOwnerEntity();
	if (pNewOwner == m_hOldProvidee.Get())
		return;
	if (m_hOldProvidee.Get())
	{
		GetAttributeManager()->StopProvidingTo(m_hOldProvidee.Get());
	}
	if (pNewOwner)
	{
		GetAttributeManager()->ProvideTo(pNewOwner);
	}
	m_hOldProvidee = pNewOwner;
}

//-----------------------------------------------------------------------------
// ScriptGetAttribute: Returns the float value of an attribute by name.
//-----------------------------------------------------------------------------
float CEconEntity::ScriptGetAttribute(const char* pName, float flFallbackValue)
{
	CEconItemView* pItem = GetAttributeContainer()->GetItem();
	if (pItem)
	{
		CEconItemAttributeDefinition* pDef = GetItemSchema()->GetAttributeDefinitionByName(pName);
		if (pDef)
		{
			CEconGetAttributeIterator it(pDef->GetDefinitionIndex(), flFallbackValue);
			pItem->IterateAttributes(&it);
			return it.m_flValue;
		}
	}
	return flFallbackValue;
}

//-----------------------------------------------------------------------------
// TranslateViewmodelHandActivity: Translates an activity based on whether the item attaches to hands.
//-----------------------------------------------------------------------------
Activity CEconEntity::TranslateViewmodelHandActivity(Activity actBase)
{
	CEconItemView* pItem = GetAttributeContainer()->GetItem();
	if (pItem && pItem->IsValid())
	{
		GameItemDefinition_t* pStaticData = pItem->GetStaticData();
		if (pStaticData && pStaticData->ShouldAttachToHands())
			return TranslateViewmodelHandActivityInternal(actBase);
	}
	return actBase;
}

#if !defined( CLIENT_DLL )
//-----------------------------------------------------------------------------
// OnOwnerClassChange: Updates model if the owner's class has changed.
//-----------------------------------------------------------------------------
void CEconEntity::OnOwnerClassChange(void)
{
#ifdef TF_DLL
	CTFPlayer* pPlayer = ToTFPlayer(GetOwnerEntity());
	if (pPlayer && pPlayer->GetPlayerClass()->GetClassIndex() != m_iOldOwnerClass)
	{
		UpdateModelToClass();
	}
#endif
}

#ifdef GAME_DLL
//-----------------------------------------------------------------------------
// CalculateVisibleClassFor: Returns the visible class index for a given player.
//-----------------------------------------------------------------------------
int CEconEntity::CalculateVisibleClassFor(CBaseCombatCharacter* pPlayer)
{
#ifdef TF_DLL
	CTFPlayer* pTFPlayer = ToTFPlayer(pPlayer);
	return (pTFPlayer ? pTFPlayer->GetPlayerClass()->GetClassIndex() : 0);
#else
	return 0;
#endif
}
#endif

//-----------------------------------------------------------------------------
// UpdateModelToClass: Updates the model and bodygroups based on the owner's class and team.
//-----------------------------------------------------------------------------
void CEconEntity::UpdateModelToClass(void)
{
#ifdef TF_DLL
	MDLCACHE_CRITICAL_SECTION();

	CTFPlayer* pPlayer = ToTFPlayer(GetOwnerEntity());
	m_iOldOwnerClass = CalculateVisibleClassFor(pPlayer);
	if (!pPlayer)
		return;

	CEconItemView* pItem = GetAttributeContainer()->GetItem();
	if (!pItem->IsValid())
		return;

	const char* pszModel = nullptr;
	if (pItem->GetStaticData()->ShouldAttachToHands())
	{
		pszModel = pPlayer->GetPlayerClass()->GetHandModelName(0);
	}
	else
	{
		int nTeam = pPlayer->GetTeamNumber();
		CTFWearable* pWearable = dynamic_cast<CTFWearable*>(this);
		if (pWearable && pWearable->IsDisguiseWearable())
			nTeam = pPlayer->m_Shared.GetDisguiseTeam();
		pszModel = pItem->GetPlayerDisplayModel(m_iOldOwnerClass, nTeam);
	}
	if (pszModel && pszModel[0])
	{
		if (V_stricmp(STRING(GetModelName()), pszModel) != 0)
		{
			if (pItem->GetStaticData()->IsContentStreamable())
			{
				modelinfo->RegisterDynamicModel(pszModel, IsClient());
				const char* pszModelAlt = pItem->GetStaticData()->GetPlayerDisplayModelAlt(m_iOldOwnerClass);
				if (pszModelAlt && pszModelAlt[0])
					modelinfo->RegisterDynamicModel(pszModelAlt, IsClient());
				if (pItem->GetVisionFilteredDisplayModel() && pItem->GetVisionFilteredDisplayModel()[0] != '\0')
					modelinfo->RegisterDynamicModel(pItem->GetVisionFilteredDisplayModel(), IsClient());
			}
			SetModel(pszModel);
		}
	}
	if (GetModelPtr() && pItem->GetStaticData()->UsesPerClassBodygroups(GetTeamNumber()))
	{
		SetBodygroup(1, m_iOldOwnerClass - 1);
	}
#endif
}

//-----------------------------------------------------------------------------
// PlayAnimForPlaybackEvent: Plays an animation for a wearable playback event.
//-----------------------------------------------------------------------------
void CEconEntity::PlayAnimForPlaybackEvent(wearableanimplayback_t iPlayback)
{
	CEconItemView* pItem = GetAttributeContainer()->GetItem();
	if (!pItem->IsValid() || !GetOwnerEntity())
		return;

	int iTeamNum = GetOwnerEntity()->GetTeamNumber();
	int iActivities = pItem->GetStaticData()->GetNumPlaybackActivities(iTeamNum);
	for (int i = 0; i < iActivities; i++)
	{
		activity_on_wearable_t* pData = pItem->GetStaticData()->GetPlaybackActivityData(iTeamNum, i);
		if (pData && pData->iPlayback == iPlayback && pData->pszActivity)
		{
			if (pData->iActivity == kActivityLookup_Unknown)
				pData->iActivity = ActivityList_IndexForName(pData->pszActivity);
			int sequence = SelectWeightedSequence((Activity)pData->iActivity);
			if (sequence != ACTIVITY_NOT_AVAILABLE)
			{
				ResetSequence(sequence);
				SetCycle(0);
#if !defined( CLIENT_DLL )
				if (IsUsingClientSideAnimation())
					ResetClientsideFrame();
#endif
			}
			return;
		}
	}
}
#endif // !CLIENT_DLL

#if defined( TF_CLIENT_DLL )
//-----------------------------------------------------------------------------
// ValidateEntityAttachedToPlayer: Validates that this econ entity is correctly attached.
//-----------------------------------------------------------------------------
bool CEconEntity::ValidateEntityAttachedToPlayer(bool& bShouldRetry)
{
	bShouldRetry = false;
#if defined( _DEBUG ) || defined( TF_CLIENT_DLL )
	bool bItemDebugValidation = false;
#endif
#ifdef _DEBUG
	bItemDebugValidation = item_debug_validation.GetBool();
	if (!bItemDebugValidation)
		return true;
#endif
#if defined( TF_CLIENT_DLL )
	if (TFGameRules()->IsInItemTestingMode())
		return true;

	C_TFPlayer* pOwner = ToTFPlayer(GetOwnerEntity());
	if (!pOwner)
	{
		bShouldRetry = (m_iNumOwnerValidationRetries < 500);
		m_iNumOwnerValidationRetries++;
		return false;
	}
	C_BaseEntity* pVM = pOwner->GetViewModel();
	bool bPlayerIsParented = false;
	C_BaseEntity* pEntity = this;
	while ((pEntity = pEntity->GetMoveParent()) != nullptr)
	{
		if (pOwner == pEntity || pVM == pEntity)
		{
			bPlayerIsParented = true;
			break;
		}
	}
	if (!bPlayerIsParented)
	{
		bShouldRetry = (m_iNumOwnerValidationRetries < 500);
		m_iNumOwnerValidationRetries++;
		return false;
	}
	m_iNumOwnerValidationRetries = 0;
	bool bOwnerIsBot = pOwner->IsABot();
	if (bOwnerIsBot && TFGameRules()->IsPVEModeActive())
		return true;

	int iClass = pOwner->GetPlayerClass()->GetClassIndex();
	int iTeam = pOwner->GetTeamNumber();

	if (pOwner == C_BasePlayer::GetLocalPlayer())
	{
		bShouldRetry = true;
		return true;
	}

	if ((pOwner->m_Shared.InCond(TF_COND_DISGUISED) || pOwner->m_Shared.InCond(TF_COND_DISGUISING)) && iClass == TF_CLASS_SPY)
	{
		bShouldRetry = true;
		return true;
	}

#if defined(TF_DLL) || defined(TF_CLIENT_DLL)
	if (m_bValidatedAttachedEntity)
		return true;
#endif

	const char* pszClientModel = modelinfo->GetModelName(GetModel());
	if (pszClientModel && g_modelWhiteList[0])
	{
		for (int i = 0; g_modelWhiteList[i] != nullptr; ++i)
		{
			if (FStrEq(pszClientModel, g_modelWhiteList[i]))
				return true;
		}
	}
	if (pOwner->m_Shared.InCond(TF_COND_HALLOWEEN_KART))
	{
		if (FStrEq(pszClientModel, HALLOWEEN_KART_MODEL))
			return true;
		if (FStrEq(pszClientModel, HALLOWEEN_KART_CAGE_MODEL))
			return true;
	}
	CTFPlayerInventory* pInv = pOwner->Inventory();
	if (!pInv)
		return false;
	if (pOwner->m_Shared.InCond(TF_COND_TAUNTING))
	{
		const char* pszCustomTauntProp = nullptr;
		int iClassTaunt = pOwner->GetPlayerClass()->GetClassIndex();
		CEconItemView* pMiscItemView = pInv->GetItemInLoadout(iClassTaunt, pOwner->GetActiveTauntSlot());
		if (pMiscItemView && pMiscItemView->IsValid())
		{
			if (pMiscItemView->GetStaticData()->GetTauntData())
			{
				pszCustomTauntProp = pMiscItemView->GetStaticData()->GetTauntData()->GetProp(iClassTaunt);
				if (pszCustomTauntProp)
					return true;
			}
		}
	}
	bool bSkipInventoryCheck = bItemDebugValidation && bOwnerIsBot;
	if ((!pInv->GetSOC() || !pInv->GetSOC()->BIsInitialized()) && !bSkipInventoryCheck)
	{
		bShouldRetry = true;
		return true;
	}

	CEconItemView* pScriptItem = GetAttributeContainer()->GetItem();
	if (!pScriptItem->IsValid())
	{
		if (pszClientModel && pszClientModel[0] != '?')
		{
			CSteamID steamIDForPlayer;
			pOwner->GetSteamID(&steamIDForPlayer);
			for (int i = 0; i < CLASS_LOADOUT_POSITION_COUNT; i++)
			{
				CEconItemView* pItem = TFInventoryManager()->GetItemInLoadoutForClass(iClass, i, &steamIDForPlayer);
				if (pItem && pItem->IsValid())
				{
					const char* pszAttached = pItem->GetExtraWearableModel();
					if (pszAttached && pszAttached[0] && FStrEq(pszClientModel, pszAttached))
						return true;
					pszAttached = pItem->GetExtraWearableViewModel();
					if (pszAttached && pszAttached[0] && FStrEq(pszClientModel, pszAttached))
						return true;
				}
			}
		}
		else if (pszClientModel && pszClientModel[0] == '?')
		{
			bShouldRetry = true;
		}
		return false;
	}
	if (!pInv->GetInventoryItemByItemID(pScriptItem->GetItemID()) && !bSkipInventoryCheck)
	{
		CEconItemView* pBaseItem = TFInventoryManager()->GetBaseItemForClass(iClass, pScriptItem->GetStaticData()->GetLoadoutSlot(iClass));
		if (*pScriptItem != *pBaseItem)
		{
			const wchar_t* pwzItemName = pScriptItem->GetItemName();
			char szItemName[MAX_ITEM_NAME_LENGTH];
			ILocalize::ConvertUnicodeToANSI(pwzItemName, szItemName, sizeof(szItemName));
#ifdef _DEBUG
			Warning("Item '%s' attached to %s, but it's not in his inventory.\n", szItemName, pOwner->GetPlayerName());
#endif
			return false;
		}
	}
	const char* pszScriptModel = pScriptItem->GetWorldDisplayModel();
	if (!pszScriptModel)
		pszScriptModel = pScriptItem->GetPlayerDisplayModel(iClass, iTeam);
	if (pszClientModel && pszClientModel[0] && pszClientModel[0] != '?')
	{
		if (!pszScriptModel || !pszScriptModel[0])
			return false;
		if (!FStrEq(pszClientModel, pszScriptModel))
		{
			const char* pszScriptModelAlt = pScriptItem->GetStaticData()->GetPlayerDisplayModelAlt(iClass);
			if (!pszScriptModelAlt || !pszScriptModelAlt[0] || !FStrEq(pszClientModel, pszScriptModelAlt))
			{
				pszScriptModel = pScriptItem->GetVisionFilteredDisplayModel();
				if (!pszScriptModel || !pszScriptModel[0])
					return false;
				if (!FStrEq(pszClientModel, pszScriptModel))
					return false;
			}
		}
	}
	else
	{
		if (pszScriptModel && pszScriptModel[0])
		{
			if (pszClientModel[0] == '?')
				bShouldRetry = true;
			return false;
		}
	}
	return true;
}
#endif // TF_CLIENT_DLL

//-----------------------------------------------------------------------------
// SetMaterialOverride: Sets the material override for a given team.
//-----------------------------------------------------------------------------
void CEconEntity::SetMaterialOverride(int team, const char* pszMaterial)
{
	if (team >= 0 && team < TEAM_VISUAL_SECTIONS)
	{
		m_MaterialOverrides[team].Init(pszMaterial, TEXTURE_GROUP_CLIENT_EFFECTS);
	}
}

void CEconEntity::SetMaterialOverride(int team, CMaterialReference& ref)
{
	if (team >= 0 && team < TEAM_VISUAL_SECTIONS)
	{
		m_MaterialOverrides[team].Init(ref);
	}
}

#ifdef CLIENT_DLL

//-----------------------------------------------------------------------------
// Purpose: Set up flex weights by deferring to the base implementation.
//-----------------------------------------------------------------------------
void CEconEntity::SetupWeights(const matrix3x4_t* pBoneToWorld, int nFlexWeightCount, float* pFlexWeights, float* pFlexDelayedWeights)
{
	BaseClass::SetupWeights(pBoneToWorld, nFlexWeightCount, pFlexWeights, pFlexDelayedWeights);
}

//-----------------------------------------------------------------------------
// InternalFireEvent: Processes custom events and falls back to default handling.
//-----------------------------------------------------------------------------
bool CEconEntity::InternalFireEvent(const Vector& origin, const QAngle& angles, int event, const char* options)
{
	switch (event)
	{
	case AE_CL_BODYGROUP_SET_VALUE_CMODEL_WPN:
		if (m_hViewmodelAttachment)
		{
			m_hViewmodelAttachment->FireEvent(origin, angles, AE_CL_BODYGROUP_SET_VALUE, options);
		}
		return true;
	default:
		break;
	}
	return false;
}


//-----------------------------------------------------------------------------
// Purpose: Fires an event by deferring to the base class.
//-----------------------------------------------------------------------------
void CEconEntity::FireEvent(const Vector& origin, const QAngle& angles, int event, const char* options)
{
	BaseClass::FireEvent(origin, angles, event, options);
}

//-----------------------------------------------------------------------------
// Purpose: Handles fire events by deferring to the base class.
//-----------------------------------------------------------------------------
bool CEconEntity::OnFireEvent(C_BaseViewModel* pViewModel, const Vector& origin, const QAngle& angles, int event, const char* options)
{
	return InternalFireEvent(origin, angles, event, options);
}

//-----------------------------------------------------------------------------
// Purpose: Indicates whether the model uses flex delayed weights.
//-----------------------------------------------------------------------------
bool CEconEntity::UsesFlexDelayedWeights(void)
{
	return (m_flFlexDelayedWeight != nullptr);
}

#endif // CLIENT_DLL


// Definition for ShouldDrawParticleSystems (added around line 540)
bool CEconEntity::ShouldDrawParticleSystems(void)
{
#if defined(TF_CLIENT_DLL) || defined(TF_DLL)
	C_TFPlayer* pPlayer = ToTFPlayer(GetOwnerEntity());
	if (pPlayer)
	{
		bool bStealthed = pPlayer->m_Shared.IsStealthed();
		if (bStealthed)
			return false;
		bool bDisguised = pPlayer->m_Shared.InCond(TF_COND_DISGUISED);
		if (bDisguised)
		{
			CTFWeaponBase* pWeapon = dynamic_cast<CTFWeaponBase*>(this);
			bool bDisguiseWeapon = pWeapon && pWeapon->m_bDisguiseWeapon;
			if (!bDisguiseWeapon)
				return false;
		}
	}
#endif
	C_BasePlayer* pLocalPlayer = C_BasePlayer::GetLocalPlayer();
	if (pLocalPlayer)
	{
		C_BaseEntity* pEffectOwner = this;
		if (pLocalPlayer == GetOwnerEntity() && pLocalPlayer->GetViewModel() && !C_BasePlayer::ShouldDrawLocalPlayer())
		{
			pEffectOwner = pLocalPlayer->GetViewModel();
		}
		if (!pEffectOwner->ShouldDraw())
			return false;
	}
	return true;
}

// Definition for SetParticleSystemsVisible (added around line 600)
void CEconEntity::SetParticleSystemsVisible(ParticleSystemState_t nState)
{
	if (nState == m_nParticleSystemsCreated)
	{
		bool bDirty = false;
#if defined(TF_CLIENT_DLL) || defined(TF_DLL)
		CTFWeaponBase* pWeapon = dynamic_cast<CTFWeaponBase*>(this);
		if (pWeapon)
		{
			if (pWeapon->m_hExtraWearable.Get())
			{
				bDirty = !(pWeapon->m_hExtraWearable->m_nParticleSystemsCreated == nState);
				pWeapon->m_hExtraWearable->m_nParticleSystemsCreated = nState;
			}
			if (pWeapon->m_hExtraWearableViewModel.Get())
			{
				bDirty = !(pWeapon->m_hExtraWearableViewModel->m_nParticleSystemsCreated == nState);
				pWeapon->m_hExtraWearableViewModel->m_nParticleSystemsCreated = nState;
			}
		}
#endif
		if (!bDirty)
			return;
	}

	CUtlVector<const attachedparticlesystem_t*> vecParticleSystems;
	GetEconParticleSystems(&vecParticleSystems);

	FOR_EACH_VEC(vecParticleSystems, i)
	{
		const attachedparticlesystem_t* pSystem = vecParticleSystems[i];
		Assert(pSystem && pSystem->pszSystemName && pSystem->pszSystemName[0]);
		if (pSystem->iCustomType)
			continue;
		ParticleSystemState_t nIndividualParticleState = nState;
		if (nIndividualParticleState == PARTICLE_SYSTEM_STATE_VISIBLE)
		{
			const CEconItemView* pItem = GetAttributeContainer()->GetItem();
			if (pItem)
			{
				GameItemDefinition_t* pDef = pItem->GetStaticData();
				if (pDef && pDef->GetNumStyles())
				{
					style_index_t unStyle = pItem->GetItemStyle();
					if (unStyle != INVALID_STYLE_INDEX)
					{
						const CEconStyleInfo* pStyle = pDef->GetStyleInfo(unStyle);
						if (pStyle && !pStyle->UseSmokeParticleEffect() && FStrEq(pSystem->pszSystemName, "drg_pipe_smoke"))
						{
							nIndividualParticleState = PARTICLE_SYSTEM_STATE_NOT_VISIBLE;
						}
					}
				}
			}
		}
		UpdateSingleParticleSystem(nIndividualParticleState != PARTICLE_SYSTEM_STATE_NOT_VISIBLE, pSystem);
	}
	m_nParticleSystemsCreated = nState;
}

// Definition for GetEconParticleSystems (added around line 650)
void CEconEntity::GetEconParticleSystems(CUtlVector<const attachedparticlesystem_t*>* out_pvecParticleSystems) const
{
	Assert(out_pvecParticleSystems);

	const CEconItemView* pEconItemView = m_AttributeManager.GetItem();
	if (pEconItemView)
	{
		const GameItemDefinition_t* pItemDef = pEconItemView->GetStaticData();
		const int iStaticParticleCount = pItemDef->GetNumAttachedParticles(GetTeamNumber());
		for (int i = 0; i < iStaticParticleCount; i++)
		{
			out_pvecParticleSystems->AddToTail(pItemDef->GetAttachedParticleData(GetTeamNumber(), i));
		}
		const int iQualityParticleType = pEconItemView->GetQualityParticleType();
		if (iQualityParticleType > 0)
		{
			out_pvecParticleSystems->AddToTail(GetItemSchema()->GetAttributeControlledParticleSystem(iQualityParticleType));
		}
	}
	int iStaticParticleEffect = 0;
	CALL_ATTRIB_HOOK_INT(iStaticParticleEffect, set_attached_particle_static);
	if (iStaticParticleEffect > 0)
	{
		out_pvecParticleSystems->AddToTail(GetItemSchema()->GetAttributeControlledParticleSystem(iStaticParticleEffect));
	}
	int iDynamicParticleEffect = 0;
	int iIsThrowableTrail = 0;
	CALL_ATTRIB_HOOK_INT(iDynamicParticleEffect, set_attached_particle);
	CALL_ATTRIB_HOOK_INT(iIsThrowableTrail, throwable_particle_trail_only);
	if (iDynamicParticleEffect > 0 && !iIsThrowableTrail)
	{
		attachedparticlesystem_t* pSystem = GetItemSchema()->GetAttributeControlledParticleSystem(iDynamicParticleEffect);
		if (pSystem)
		{
#if defined(TF_CLIENT_DLL) || defined(TF_DLL)
			static char pszFullname[256];
			if (GetTeamNumber() == TF_TEAM_BLUE && V_stristr(pSystem->pszSystemName, "_teamcolor_red"))
			{
				V_StrSubst(pSystem->pszSystemName, "_teamcolor_red", "_teamcolor_blue", pszFullname, 256);
				pSystem = GetItemSchema()->FindAttributeControlledParticleSystem(pszFullname);
			}
			else if (GetTeamNumber() == TF_TEAM_RED && V_stristr(pSystem->pszSystemName, "_teamcolor_blue"))
			{
				V_StrSubst(pSystem->pszSystemName, "_teamcolor_blue", "_teamcolor_red", pszFullname, 256);
				pSystem = GetItemSchema()->FindAttributeControlledParticleSystem(pszFullname);
			}
#endif
			if (pSystem)
				out_pvecParticleSystems->AddToTail(pSystem);
		}
	}
	for (int i = out_pvecParticleSystems->Count() - 1; i >= 0; i--)
	{
		if (!(*out_pvecParticleSystems)[i] ||
			!(*out_pvecParticleSystems)[i]->pszSystemName ||
			!(*out_pvecParticleSystems)[i]->pszSystemName[0])
		{
			out_pvecParticleSystems->FastRemove(i);
		}
	}
}

// Also, add a definition for the external variable g_modelWhiteList
#if defined(TF_CLIENT_DLL)
const char* g_modelWhiteList[] = {
	"models/example/model1.mdl",
	"models/example/model2.mdl",
	nullptr
};
#endif


//-----------------------------------------------------------------------------
// GetToolRecordingState: Adds material override info to a KeyValues message.
//-----------------------------------------------------------------------------
void CEconEntity::GetToolRecordingState(KeyValues* msg)
{
#ifndef _XBOX
	BaseClass::GetToolRecordingState(msg);
	bool bUseOverride = (GetTeamNumber() >= 0 && GetTeamNumber() < TEAM_VISUAL_SECTIONS) && m_MaterialOverrides[GetTeamNumber()].IsValid();
	if (bUseOverride)
	{
		msg->SetString("materialOverride", m_MaterialOverrides[GetTeamNumber()]->GetName());
	}
#endif
}

#ifndef DOTA_DLL
//-----------------------------------------------------------------------------
// C_ViewmodelAttachmentModel implementation (non-DOTA client).
//-----------------------------------------------------------------------------
void C_ViewmodelAttachmentModel::SetOuter(CEconEntity* pOuter)
{
	m_hOuter = pOuter;
	SetOwnerEntity(pOuter);
	CEconItemView* pItem = pOuter->GetAttributeContainer()->GetItem();
	if (pItem->IsValid())
	{
		m_bAlwaysFlip = pItem->GetStaticData()->ShouldFlipViewmodels();
	}
}

bool C_ViewmodelAttachmentModel::InitializeAsClientEntity(const char* pszModelName, RenderGroup_t renderGroup)
{
	if (!BaseClass::InitializeAsClientEntity(pszModelName, renderGroup))
		return false;
	AddEffects(EF_BONEMERGE);
	AddEffects(EF_BONEMERGE_FASTCULL);
	AddEffects(EF_NODRAW);
	return true;
}

int C_ViewmodelAttachmentModel::InternalDrawModel(int flags)
{
#ifdef TF_CLIENT_DLL
	CMatRenderContextPtr pRenderContext(materials);
	if (cl_flipviewmodels.GetBool() != m_bAlwaysFlip)
	{
		pRenderContext->CullMode(MATERIAL_CULLMODE_CW);
	}
#endif
	int r = BaseClass::InternalDrawModel(flags);
#ifdef TF_CLIENT_DLL
	pRenderContext->CullMode(MATERIAL_CULLMODE_CCW);
#endif
	return r;
}

bool C_ViewmodelAttachmentModel::OnPostInternalDrawModel(ClientModelRenderInfo_t* pInfo)
{
	if (!BaseClass::OnPostInternalDrawModel(pInfo))
		return false;
	if (!m_hOuter)
		return true;
	if (!m_hOuter->GetAttributeContainer())
		return true;
	if (!m_hOuter->GetAttributeContainer()->GetItem())
		return true;
	DrawEconEntityAttachedModels(this, GetOuter(), pInfo, kAttachedModelDisplayFlag_ViewModel);
	return true;
}

void C_ViewmodelAttachmentModel::StandardBlendingRules(CStudioHdr* hdr, Vector pos[], Quaternion q[], float currentTime, int boneMask)
{
	BaseClass::StandardBlendingRules(hdr, pos, q, currentTime, boneMask);
	if (m_hOuter)
		m_hOuter->ViewModelAttachmentBlending(hdr, pos, q, currentTime, boneMask);
}

void FormatViewModelAttachment(Vector& vOrigin, bool bInverse);
void C_ViewmodelAttachmentModel::FormatViewModelAttachment(int nAttachment, matrix3x4_t& attachmentToWorld)
{
	Vector vecOrigin;
	MatrixPosition(attachmentToWorld, vecOrigin);
	::FormatViewModelAttachment(vecOrigin, false);
	PositionMatrix(vecOrigin, attachmentToWorld);
}

int C_ViewmodelAttachmentModel::GetSkin(void)
{
	if (m_hOuter != nullptr)
	{
		CBaseCombatWeapon* pWeapon = m_hOuter->MyCombatWeaponPointer();
		if (pWeapon)
		{
			int nSkin = pWeapon->GetSkinOverride();
			if (nSkin != -1)
				return nSkin;
		}
		else
		{
			if (m_hOuter->GetAttributeContainer())
			{
				CEconItemView* pItem = m_hOuter->GetAttributeContainer()->GetItem();
				if (pItem && pItem->IsValid() && GetOwnerViaInterface())
					return pItem->GetSkin(GetOwnerViaInterface()->GetTeamNumber(), true);
			}
		}
	}
	return BaseClass::GetSkin();
}
#endif // !DOTA_DLL

#endif // CLIENT_DLL

//-----------------------------------------------------------------------------
// Release: Clean up particle systems and viewmodel attachment.
//-----------------------------------------------------------------------------
void CEconEntity::Release(void)
{
#ifdef CLIENT_DLL
	SetParticleSystemsVisible(PARTICLE_SYSTEM_STATE_NOT_VISIBLE);
	C_BaseEntity* pEffectOwnerWM = this;
	C_BaseEntity* pEffectOwnerVM = nullptr;
	bool bExtraWearable = false, bExtraWearableVM = false;
	CTFWeaponBase* pWeapon = dynamic_cast<CTFWeaponBase*>(this);
	if (pWeapon)
	{
		pEffectOwnerVM = pWeapon->GetPlayerOwner() ? pWeapon->GetPlayerOwner()->GetViewModel() : nullptr;
		if (pWeapon->m_hExtraWearable.Get())
		{
			pEffectOwnerWM = pWeapon->m_hExtraWearable.Get();
			bExtraWearable = true;
		}
		if (pWeapon->m_hExtraWearableViewModel.Get())
		{
			pEffectOwnerVM = pWeapon->m_hExtraWearableViewModel.Get();
			bExtraWearableVM = true;
		}
		if (pEffectOwnerVM)
			pEffectOwnerVM->ParticleProp()->StopEmission(nullptr, false, true);
	}
	pEffectOwnerWM->ParticleProp()->StopEmission(nullptr, false, true);
#endif
	if (m_hViewmodelAttachment)
		m_hViewmodelAttachment->Release();
	BaseClass::Release();
}

//-----------------------------------------------------------------------------
// SetDormant: Hides particle systems if needed, then calls base.
//-----------------------------------------------------------------------------
void CEconEntity::SetDormant(bool bDormant)
{
	if (!IsDormant() && bDormant && m_nParticleSystemsCreated != PARTICLE_SYSTEM_STATE_NOT_VISIBLE)
	{
		SetParticleSystemsVisible(PARTICLE_SYSTEM_STATE_NOT_VISIBLE);
	}
	BaseClass::SetDormant(bDormant);
}

#ifndef DOTA_DLL
//-----------------------------------------------------------------------------
// OnPreDataChanged: Called before network data changes.
//-----------------------------------------------------------------------------
void CEconEntity::OnPreDataChanged(DataUpdateType_t type)
{
	BaseClass::OnPreDataChanged(type);
	m_iOldTeam = m_iTeamNum;
}

IMaterial* CreateTempMaterialForPlayerLogo(int iPlayerIndex, player_info_t* info, char* texname, int nchars);

//-----------------------------------------------------------------------------
// OnDataChanged: Called after network data changes.
//-----------------------------------------------------------------------------
void CEconEntity::OnDataChanged(DataUpdateType_t updateType)
{
	if (updateType == DATA_UPDATE_CREATED)
	{
		InitializeAttributes();
		m_nParticleSystemsCreated = PARTICLE_SYSTEM_STATE_NOT_VISIBLE;
		m_bAttachmentDirty = true;
	}
	BaseClass::OnDataChanged(updateType);
	GetAttributeContainer()->OnDataChanged(updateType);
	if (updateType == DATA_UPDATE_CREATED)
	{
		CEconItemView* pItem = m_AttributeManager.GetItem();
#if defined(_DEBUG)
		if (item_debug.GetBool())
			DebugDescribe();
#endif
		const char* pszPaintKitMaterialOverride = GetPaintKitMaterialOverride(pItem);
		for (int team = 0; team < TEAM_VISUAL_SECTIONS; team++)
		{
			const char* pszMaterial = pszPaintKitMaterialOverride ? pszPaintKitMaterialOverride : pItem->GetStaticData()->GetMaterialOverride(team);
			if (pszMaterial)
			{
				m_MaterialOverrides[team].Init(pszMaterial, TEXTURE_GROUP_CLIENT_EFFECTS);
			}
		}
#ifdef TF_CLIENT_DLL
		C_TFPlayer* pPlayer = ToTFPlayer(GetOwnerEntity());
		if (pPlayer)
			pPlayer->SetBodygroupsDirty();
		m_bValidatedOwner = false;
		m_iNumOwnerValidationRetries = 0;
		UpdateVisibility();
#endif
	}
	UpdateAttachmentModels();
}

//-----------------------------------------------------------------------------
// UpdateAttachmentModels: Updates attached models based on item definition and state.
//-----------------------------------------------------------------------------
void CEconEntity::UpdateAttachmentModels(void)
{
#ifndef DOTA_DLL
	CEconItemView* pItem = GetAttributeContainer()->GetItem();
	GameItemDefinition_t* pItemDef = (pItem && pItem->IsValid()) ? pItem->GetStaticData() : nullptr;
	m_vecAttachedModels.Purge();
	if (pItemDef && AttachmentModelsShouldBeVisible())
	{
		int iTeamNumber = GetTeamNumber();
		int iAttachedModels = pItemDef->GetNumAttachedModels(iTeamNumber);
		for (int i = 0; i < iAttachedModels; i++)
		{
			attachedmodel_t* pModel = pItemDef->GetAttachedModelData(iTeamNumber, i);
			int iModelIndex = modelinfo->GetModelIndex(pModel->m_pszModelName);
			if (iModelIndex >= 0)
			{
				AttachedModelData_t attachedModelData;
				attachedModelData.m_pModel = modelinfo->GetModel(iModelIndex);
				attachedModelData.m_iModelDisplayFlags = pModel->m_iModelDisplayFlags;
				m_vecAttachedModels.AddToTail(attachedModelData);
			}
		}
		int iAttachedModelsFestivized = pItemDef->GetNumAttachedModelsFestivized(iTeamNumber);
		if (iAttachedModelsFestivized)
		{
			int iFestivized = 0;
			CALL_ATTRIB_HOOK_INT(iFestivized, is_festivized);
			if (iFestivized)
			{
				for (int i = 0; i < iAttachedModelsFestivized; i++)
				{
					attachedmodel_t* pModel = pItemDef->GetAttachedModelDataFestivized(iTeamNumber, i);
					int iModelIndex = modelinfo->GetModelIndex(pModel->m_pszModelName);
					if (iModelIndex >= 0)
					{
						AttachedModelData_t attachedModelData;
						attachedModelData.m_pModel = modelinfo->GetModel(iModelIndex);
						attachedModelData.m_iModelDisplayFlags = pModel->m_iModelDisplayFlags;
						m_vecAttachedModels.AddToTail(attachedModelData);
					}
				}
			}
		}
	}
	bool bItemNeedsAttachment = pItemDef && (pItemDef->ShouldAttachToHands() || pItemDef->ShouldAttachToHandsVMOnly());
	if (bItemNeedsAttachment)
	{
		bool bShouldShowAttachment = false;
		CBasePlayer* pOwner = ToBasePlayer(GetOwnerEntity());
		if (pOwner && !pOwner->ShouldDrawThisPlayer())
			bShouldShowAttachment = true;
		if (bShouldShowAttachment && AttachmentModelsShouldBeVisible())
		{
			if (!m_hViewmodelAttachment)
			{
				C_BaseViewModel* vm = pOwner->GetViewModel(0);
				if (vm)
				{
					C_ViewmodelAttachmentModel* pEnt = new C_ViewmodelAttachmentModel;
					if (!pEnt)
						return;
					pEnt->SetOuter(this);
					int iClass = 0;
#if defined( TF_DLL ) || defined( TF_CLIENT_DLL )
					CTFPlayer* pTFPlayer = ToTFPlayer(pOwner);
					if (pTFPlayer)
						iClass = pTFPlayer->GetPlayerClass()->GetClassIndex();
#endif
					if (!pEnt->InitializeAsClientEntity(pItem->GetPlayerDisplayModel(iClass, pOwner->GetTeamNumber()), RENDER_GROUP_VIEW_MODEL_OPAQUE))
						return;
					m_hViewmodelAttachment = pEnt;
					m_hViewmodelAttachment->SetParent(vm);
					m_hViewmodelAttachment->SetLocalOrigin(vec3_origin);
					m_hViewmodelAttachment->UpdatePartitionListEntry();
					m_hViewmodelAttachment->CollisionProp()->UpdatePartition();
					m_hViewmodelAttachment->UpdateVisibility();
					m_bAttachmentDirty = true;
				}
			}
			else if (m_hViewmodelAttachment)
			{
				if (m_iOldTeam != m_iTeamNum)
					m_bAttachmentDirty = true;
			}
			if (m_bAttachmentDirty && m_hViewmodelAttachment)
			{
				pOwner = ToBasePlayer(GetOwnerEntity());
				C_BaseViewModel* vm = pOwner->GetViewModel(0);
				if (vm && vm->GetWeapon() == this)
				{
					m_hViewmodelAttachment->m_nSkin = vm->GetSkin();
					m_bAttachmentDirty = false;
				}
			}
			return;
		}
	}
	if (m_hViewmodelAttachment)
		m_hViewmodelAttachment->Release();
}
#endif // !DOTA_DLL

//-----------------------------------------------------------------------------
// HasCustomParticleSystems: Returns whether this entity has custom particle systems.
//-----------------------------------------------------------------------------
bool CEconEntity::HasCustomParticleSystems(void) const
{
	return m_bHasParticleSystems;
}

//-----------------------------------------------------------------------------
// UpdateParticleSystems: Determines correct visibility state and applies it.
//-----------------------------------------------------------------------------
void CEconEntity::UpdateParticleSystems(void)
{
	if (!HasCustomParticleSystems())
		return;

	ParticleSystemState_t nVisible = PARTICLE_SYSTEM_STATE_NOT_VISIBLE;
	if (IsEffectActive(EF_NODRAW) || !ShouldDraw())
		nVisible = PARTICLE_SYSTEM_STATE_NOT_VISIBLE;
	else if (!GetOwnerEntity() && !IsDormant())
		nVisible = PARTICLE_SYSTEM_STATE_VISIBLE;
	else if (GetOwnerEntity() && !GetOwnerEntity()->IsDormant() && GetOwnerEntity()->IsPlayer() && GetOwnerEntity()->IsAlive())
		nVisible = PARTICLE_SYSTEM_STATE_VISIBLE;

#if defined(TF_CLIENT_DLL) || defined(TF_DLL)
	if (nVisible == PARTICLE_SYSTEM_STATE_NOT_VISIBLE)
	{
		CTFWeaponBase* pWeapon = dynamic_cast<CTFWeaponBase*>(this);
		C_BasePlayer* pLocalPlayer = C_BasePlayer::GetLocalPlayer();
		if (pLocalPlayer && pLocalPlayer == GetOwnerEntity() && pLocalPlayer->GetViewModel() && pLocalPlayer->GetViewModel()->GetWeapon() == pWeapon && !C_BasePlayer::ShouldDrawLocalPlayer())
			nVisible = PARTICLE_SYSTEM_STATE_VISIBLE_VM;
	}
#endif

	if (nVisible != PARTICLE_SYSTEM_STATE_NOT_VISIBLE && !ShouldDrawParticleSystems())
		nVisible = PARTICLE_SYSTEM_STATE_NOT_VISIBLE;

	SetParticleSystemsVisible(nVisible);
}

//-----------------------------------------------------------------------------
// UpdateSingleParticleSystem: Stops previous particles and creates a new system if needed.
//-----------------------------------------------------------------------------
void CEconEntity::UpdateSingleParticleSystem(bool bVisible, const attachedparticlesystem_t* pSystem)
{
	Assert(pSystem);
	C_BasePlayer* pLocalPlayer = C_BasePlayer::GetLocalPlayer();
	if (!pLocalPlayer)
		return;

	C_BaseEntity* pEffectOwnerWM = this;
	C_BaseEntity* pEffectOwnerVM = nullptr;
	bool bExtraWearable = false, bExtraWearableVM = false;
	CTFWeaponBase* pWeapon = dynamic_cast<CTFWeaponBase*>(this);
	if (pWeapon)
	{
		pEffectOwnerVM = pWeapon->GetPlayerOwner() ? pWeapon->GetPlayerOwner()->GetViewModel() : nullptr;
		if (pWeapon->m_hExtraWearable.Get())
		{
			pEffectOwnerWM = pWeapon->m_hExtraWearable.Get();
			bExtraWearable = true;
		}
		if (pWeapon->m_hExtraWearableViewModel.Get())
		{
			pEffectOwnerVM = pWeapon->m_hExtraWearableViewModel.Get();
			bExtraWearableVM = true;
		}
	}

	C_BaseEntity* pEffectOwner = pEffectOwnerWM;
	bool bIsVM = false;
	C_BasePlayer* pOwner = ToBasePlayer(GetOwnerEntity());
	bool bDrawThisEffect = true;
	if (!pOwner->ShouldDrawThisPlayer())
	{
		if (!pSystem->bDrawInViewModel)
			bDrawThisEffect = false;
		C_BaseViewModel* pLocalPlayerVM = pLocalPlayer->GetViewModel();
		if (pLocalPlayerVM && pLocalPlayerVM->GetOwningWeapon() == this)
		{
			bIsVM = true;
			pEffectOwner = pEffectOwnerVM;
		}
	}

	const char* pszAttachmentName = pSystem->pszControlPoints[0];
	if (bIsVM && bExtraWearableVM)
		pszAttachmentName = "attach_fob_v";
	if (!bIsVM && bExtraWearable)
		pszAttachmentName = "attach_fob";

	int iAttachment = INVALID_PARTICLE_ATTACHMENT;
	if (pszAttachmentName && pszAttachmentName[0] && pEffectOwner->GetBaseAnimating())
		iAttachment = pEffectOwner->GetBaseAnimating()->LookupAttachment(pszAttachmentName);

	const CEconItemView* pEconItemView = m_AttributeManager.GetItem();
	static char pszTempName[256] = { 0 };
	static char pszTempNameVM[256] = { 0 };
	const char* pszSystemName = pSystem->pszSystemName;
	if (pSystem->bUseSuffixName && pEconItemView && pEconItemView->GetItemDefinition()->GetParticleSuffix())
	{
		V_strcpy_safe(pszTempName, pszSystemName);
		V_strcat_safe(pszTempName, "_");
		V_strcat_safe(pszTempName, pEconItemView->GetItemDefinition()->GetParticleSuffix());
		pszSystemName = pszTempName;
	}

	bool bHasUniqueVMEffect = true;
	if (pSystem->bDrawInViewModel)
	{
		V_strcpy_safe(pszTempNameVM, pszSystemName);
		V_strcat_safe(pszTempNameVM, "_vm");
		if (g_pParticleSystemMgr->FindParticleSystem(pszTempNameVM) == nullptr)
		{
			V_strcpy_safe(pszTempNameVM, pszSystemName);
			bHasUniqueVMEffect = false;
		}
		if (bIsVM)
			pszSystemName = pszTempNameVM;
	}

	if (g_pParticleSystemMgr->FindParticleSystem(pszSystemName) == nullptr)
		return;

	if (iAttachment != INVALID_PARTICLE_ATTACHMENT)
	{
		pEffectOwnerWM->ParticleProp()->StopParticlesWithNameAndAttachment(pszSystemName, iAttachment, true);
		if (pEffectOwnerVM)
		{
			if (bHasUniqueVMEffect)
				pEffectOwnerVM->ParticleProp()->StopParticlesWithNameAndAttachment(pszTempNameVM, iAttachment, true);
			pEffectOwnerVM->ParticleProp()->StopParticlesWithNameAndAttachment(pszSystemName, iAttachment, true);
		}
	}
	else
	{
		pEffectOwnerWM->ParticleProp()->StopParticlesNamed(pszSystemName, true);
		if (pEffectOwnerVM)
		{
			if (bHasUniqueVMEffect)
				pEffectOwnerVM->ParticleProp()->StopParticlesNamed(pszTempNameVM, true);
			pEffectOwnerVM->ParticleProp()->StopParticlesNamed(pszSystemName, true);
		}
	}
	if (!bDrawThisEffect)
		return;

	if (!pWeapon && bIsVM)
	{
		Assert(0);
		Warning("Cannot create a Viewmodel Particle Effect [%s] when there is no Viewmodel Weapon", pszSystemName);
		return;
	}
	if (bVisible && pEffectOwner)
	{
		HPARTICLEFFECT pEffect = nullptr;
		RemoveEffects(EF_BONEMERGE_FASTCULL);
		if (iAttachment != INVALID_PARTICLE_ATTACHMENT)
			pEffect = pEffectOwner->ParticleProp()->Create(pszSystemName, PATTACH_POINT_FOLLOW, pszAttachmentName);
		else
			pEffect = pSystem->bFollowRootBone ? pEffectOwner->ParticleProp()->Create(pszSystemName, PATTACH_ROOTBONE_FOLLOW) : pEffectOwner->ParticleProp()->Create(pszSystemName, PATTACH_ABSORIGIN_FOLLOW);
		if (pEffect)
		{
			for (int i = 1; i < ARRAYSIZE(pSystem->pszControlPoints); ++i)
			{
				const char* pszControlPointName = pSystem->pszControlPoints[i];
				if (pszControlPointName && pszControlPointName[0] != '\0')
				{
					pEffectOwner->ParticleProp()->AddControlPoint(pEffect, i, this, PATTACH_POINT_FOLLOW, pszControlPointName);
				}
			}
			if (bIsVM)
			{
				pEffect->SetIsViewModelEffect(true);
				ClientLeafSystem()->SetRenderGroup(pEffect->RenderHandle(), RENDER_GROUP_VIEW_MODEL_TRANSLUCENT);
			}
		}
	}
}

//-----------------------------------------------------------------------------
// InitializeAsClientEntity: Marks the econ entity as a client entity.
//-----------------------------------------------------------------------------
bool CEconEntity::InitializeAsClientEntity(const char* pszModelName, RenderGroup_t renderGroup)
{
	m_bClientside = true;
	return BaseClass::InitializeAsClientEntity(pszModelName, renderGroup);
}

//-----------------------------------------------------------------------------
// GetEconWeaponMaterialOverride: Returns the material override for a given team.
//-----------------------------------------------------------------------------
IMaterial* CEconEntity::GetEconWeaponMaterialOverride(int iTeam)
{
	if (iTeam >= 0 && iTeam < TEAM_VISUAL_SECTIONS && m_MaterialOverrides[iTeam].IsValid())
		return m_MaterialOverrides[iTeam];
	return nullptr;
}

bool CEconEntity::ShouldDraw()
{
	return !ShouldHideForVisionFilterFlags() && BaseClass::ShouldDraw();
}

bool CEconEntity::ShouldHideForVisionFilterFlags(void)
{
	CEconItemView* pItem = GetAttributeContainer()->GetItem();
	if (pItem && pItem->IsValid())
	{
		CEconItemDefinition* pData = pItem->GetStaticData();
		if (pData)
		{
			int nVisionFilterFlags = pData->GetVisionFilterFlags();
			if (nVisionFilterFlags != 0 && !IsLocalPlayerUsingVisionFilterFlags(nVisionFilterFlags, true))
				return true;
		}
	}
	return false;
}

bool CEconEntity::IsTransparent(void)
{
#ifdef TF_CLIENT_DLL
	C_TFPlayer* pPlayer = ToTFPlayer(GetOwnerEntity());
	if (pPlayer && pPlayer->IsTransparent())
		return true;
#endif
	return BaseClass::IsTransparent();
}

bool CEconEntity::ViewModel_IsTransparent(void)
{
	return (m_hViewmodelAttachment != nullptr && m_hViewmodelAttachment->IsTransparent()) || IsTransparent();
}

bool CEconEntity::ViewModel_IsUsingFBTexture(void)
{
	return (m_hViewmodelAttachment != nullptr && m_hViewmodelAttachment->UsesPowerOfTwoFrameBufferTexture()) || UsesPowerOfTwoFrameBufferTexture();
}

bool CEconEntity::IsOverridingViewmodel(void)
{
	bool bUseOverride = (GetTeamNumber() >= 0 && GetTeamNumber() < TEAM_VISUAL_SECTIONS && m_MaterialOverrides[GetTeamNumber()].IsValid());
	bUseOverride = bUseOverride || (m_hViewmodelAttachment != nullptr) || (m_AttributeManager.GetItem()->GetStaticData()->GetNumAttachedModels(GetTeamNumber()) > 0);
	return bUseOverride;
}

int CEconEntity::DrawOverriddenViewmodel(C_BaseViewModel* pViewmodel, int flags)
{
	int ret = 0;
#ifndef DOTA_DLL
	bool bIsAttachmentTranslucent = (m_hViewmodelAttachment != nullptr && m_hViewmodelAttachment->IsTransparent());
	bool bUseOverride = false;
	CEconItemView* pItem = GetAttributeContainer()->GetItem();
	bool bAttachesToHands = (pItem->IsValid() && (pItem->GetStaticData()->ShouldAttachToHands() || pItem->GetStaticData()->ShouldAttachToHandsVMOnly()));
	if (bIsAttachmentTranslucent)
		ret = pViewmodel->DrawOverriddenViewmodel(flags);
	if (flags & STUDIO_RENDER)
	{
		IMaterial* pOverrideMaterial = nullptr;
		OverrideType_t nDontcare = OVERRIDE_NORMAL;
		modelrender->GetMaterialOverride(&pOverrideMaterial, &nDontcare);
		bool bIgnoreOverride = (pOverrideMaterial != nullptr);
		bUseOverride = !bIgnoreOverride && (GetTeamNumber() >= 0 && GetTeamNumber() < TEAM_VISUAL_SECTIONS && m_MaterialOverrides[GetTeamNumber()].IsValid());
		if (bUseOverride)
		{
			modelrender->ForcedMaterialOverride(m_MaterialOverrides[GetTeamNumber()]);
			flags |= STUDIO_NO_OVERRIDE_FOR_ATTACH;
		}
		if (m_hViewmodelAttachment)
		{
			m_hViewmodelAttachment->RemoveEffects(EF_NODRAW);
			m_hViewmodelAttachment->DrawModel(flags);
			m_hViewmodelAttachment->AddEffects(EF_NODRAW);
		}
		if (bAttachesToHands && bUseOverride)
		{
			modelrender->ForcedMaterialOverride(nullptr);
			bUseOverride = false;
		}
	}
	if (!bIsAttachmentTranslucent)
		ret = pViewmodel->DrawOverriddenViewmodel(flags);
	if (bUseOverride)
		modelrender->ForcedMaterialOverride(nullptr);
#endif
	return ret;
}

bool CEconEntity::OnInternalDrawModel(ClientModelRenderInfo_t* pInfo)
{
	if (!BaseClass::OnInternalDrawModel(pInfo))
		return false;
	if (GetOwnerEntity() && pInfo)
		pInfo->pLightingOrigin = &(GetOwnerEntity()->WorldSpaceCenter());
	DrawEconEntityAttachedModels(this, this, pInfo, kAttachedModelDisplayFlag_WorldModel);
	return true;
}

int CEconEntity::LookupAttachment(const char* pAttachmentName)
{
	return (m_hViewmodelAttachment ? m_hViewmodelAttachment->LookupAttachment(pAttachmentName) : BaseClass::LookupAttachment(pAttachmentName));
}

bool CEconEntity::GetAttachment(int number, matrix3x4_t& matrix)
{
	return (m_hViewmodelAttachment ? m_hViewmodelAttachment->GetAttachment(number, matrix) : BaseClass::GetAttachment(number, matrix));
}

bool CEconEntity::GetAttachment(int number, Vector& origin)
{
	return (m_hViewmodelAttachment ? m_hViewmodelAttachment->GetAttachment(number, origin) : BaseClass::GetAttachment(number, origin));
}

bool CEconEntity::GetAttachment(int number, Vector& origin, QAngle& angles)
{
	return (m_hViewmodelAttachment ? m_hViewmodelAttachment->GetAttachment(number, origin, angles) : BaseClass::GetAttachment(number, origin, angles));
}

bool CEconEntity::GetAttachmentVelocity(int number, Vector& originVel, Quaternion& angleVel)
{
	return (m_hViewmodelAttachment ? m_hViewmodelAttachment->GetAttachmentVelocity(number, originVel, angleVel) : BaseClass::GetAttachmentVelocity(number, originVel, angleVel));
}

bool CEconEntity::UpdateBodygroups(CBaseCombatCharacter* pOwner, int iState)
{
	if (!pOwner)
		return false;
	CAttributeContainer* pCont = GetAttributeContainer();
	if (!pCont)
		return false;
	CEconItemView* pItem = pCont->GetItem();
	if (!pItem)
		return false;
	const CEconItemDefinition* pItemDef = pItem->GetItemDefinition();
	if (!pItemDef)
		return false;

	int iNumBodyGroups = pItemDef->GetNumModifiedBodyGroups(0);
	for (int i = 0; i < iNumBodyGroups; ++i)
	{
		int iBody = 0;
		const char* pszBodyGroup = pItemDef->GetModifiedBodyGroup(0, i, iBody);
		if (iBody != iState)
			continue;
		int iBodyGroup = pOwner->FindBodygroupByName(pszBodyGroup);
		if (iBodyGroup == -1)
			continue;
		pOwner->SetBodygroup(iBodyGroup, iState);
	}

	const CEconStyleInfo* pStyle = pItemDef->GetStyleInfo(pItem->GetStyle());
	if (pStyle)
	{
		FOR_EACH_VEC(pStyle->GetAdditionalHideBodygroups(), i)
		{
			int iBodyGroup = pOwner->FindBodygroupByName(pStyle->GetAdditionalHideBodygroups()[i]);
			if (iBodyGroup == -1)
				continue;
			pOwner->SetBodygroup(iBodyGroup, iState);
		}
		if (pStyle->GetBodygroupName() != nullptr)
		{
			int iBodyGroup = pOwner->FindBodygroupByName(pStyle->GetBodygroupName());
			if (iBodyGroup != -1)
			{
				SetBodygroup(iBodyGroup, pStyle->GetBodygroupSubmodelIndex());
			}
		}
	}

	int iBodyOverride = pItemDef->GetWorldmodelBodygroupOverride(pOwner->GetTeamNumber());
	int iBodyStateOverride = pItemDef->GetWorldmodelBodygroupStateOverride(pOwner->GetTeamNumber());
	if (iBodyOverride > -1 && iBodyStateOverride > -1)
	{
		pOwner->SetBodygroup(iBodyOverride, iBodyStateOverride);
	}

	iBodyOverride = pItemDef->GetViewmodelBodygroupOverride(pOwner->GetTeamNumber());
	iBodyStateOverride = pItemDef->GetViewmodelBodygroupStateOverride(pOwner->GetTeamNumber());
	if (iBodyOverride > -1 && iBodyStateOverride > -1)
	{
		CBasePlayer* pPlayer = ToBasePlayer(pOwner);
		if (pPlayer)
		{
			CBaseViewModel* pVM = pPlayer->GetViewModel();
			if (pVM && pVM->GetModelPtr())
				pVM->SetBodygroup(iBodyOverride, iBodyStateOverride);
		}
	}
	return true;
}

//-----------------------------------------------------------------------------
// CBaseAttributableItem constructor.
//-----------------------------------------------------------------------------
CBaseAttributableItem::CBaseAttributableItem()
{
}

#endif // ECON_ENTITY_H