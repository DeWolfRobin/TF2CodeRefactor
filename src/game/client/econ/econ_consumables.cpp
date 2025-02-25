//========= Copyright Valve Corporation, All rights reserved. ============//

#include "cbase.h"
#include "econ_item_tools.h"

//---------------------------------------------------------------------------------------
// Purpose: Returns the localization token for the use command.
//---------------------------------------------------------------------------------------
const char* IEconTool::GetUseCommandLocalizationToken(const IEconItemInterface* pItem, int i) const
{
    Assert(i == 0); // Default only has 1 use.
    Assert(pItem && pItem->GetItemDefinition());
    Assert(pItem->GetItemDefinition()->GetEconTool() == this);
    return GetUseString();
}

//---------------------------------------------------------------------------------------
// Purpose: Returns the command string for using the item.
//---------------------------------------------------------------------------------------
const char* IEconTool::GetUseCommand(const IEconItemInterface* pItem, int i) const
{
    Assert(i == 0);
    Assert(pItem && pItem->GetItemDefinition());
    Assert(pItem->GetItemDefinition()->GetEconTool() == this);

    // Check if the item is a GC consumable.
    const bool bIsGCConsumable = ((pItem->GetItemDefinition()->GetCapabilities() & ITEM_CAP_USABLE_GC) != 0);
    return bIsGCConsumable ? "Context_UseConsumableItem" : "Context_ApplyOnItem";
}

//---------------------------------------------------------------------------------------
// Purpose: Determines whether the local player is the gifter for a wrapped gift.
//---------------------------------------------------------------------------------------
bool IsLocalPlayerWrappedGift(const IEconItemInterface* pItem)
{
    Assert(pItem && pItem->GetItemDefinition());
    Assert(pItem->GetItemDefinition()->GetTypedEconTool<CEconTool_WrappedGift>());

    static CSchemaAttributeDefHandle pAttr_GifterAccountID("gifter account id");
    uint32 unGifterAccountID;
    if (!pItem->FindAttribute(pAttr_GifterAccountID, &unGifterAccountID))
        return false;

    const uint32 unLocalAccountID = steamapicontext->SteamUser()->GetSteamID().GetAccountID();
    return (unGifterAccountID == unLocalAccountID);
}

//---------------------------------------------------------------------------------------
// Purpose: Determines if a wrapped gift can be used now.
//---------------------------------------------------------------------------------------
bool CEconTool_WrappedGift::CanBeUsedNow(const IEconItemInterface* pItem) const
{
    static CSchemaItemDefHandle pItemDef_WrappedGiftapultPackage("Wrapped Giftapult Package");
    static CSchemaItemDefHandle pItemDef_DeliveredGiftapultPackage("Delivered Giftapult Package");
    static CSchemaItemDefHandle pItemDef_CompetitiveBetaPassGift("Competitive Matchmaking Beta Giftable Invite");

    Assert(pItem && pItem->GetItemDefinition());
    Assert(pItem->GetItemDefinition()->GetEconTool() == this);

    if (pItem->GetItemDefinition() == pItemDef_WrappedGiftapultPackage ||
        pItem->GetItemDefinition() == pItemDef_CompetitiveBetaPassGift ||
        pItem->GetItemDefinition() == pItemDef_DeliveredGiftapultPackage)
    {
        return true;
    }
    return pItem->IsTradable();
}

//---------------------------------------------------------------------------------------
// Purpose: Determines if the contained item panel should be shown for a wrapped gift.
//---------------------------------------------------------------------------------------
bool CEconTool_WrappedGift::ShouldShowContainedItemPanel(const IEconItemInterface* pItem) const
{
    Assert(pItem && pItem->GetItemDefinition());
    Assert(pItem->GetItemDefinition()->GetEconTool() == this);
    return IsLocalPlayerWrappedGift(pItem);
}

//---------------------------------------------------------------------------------------
// Purpose: Returns the localization token for the wrapped gift use command.
//---------------------------------------------------------------------------------------
const char* CEconTool_WrappedGift::GetUseCommandLocalizationToken(const IEconItemInterface* pItem, int i) const
{
    Assert(pItem && pItem->GetItemDefinition());
    Assert(pItem->GetItemDefinition()->GetEconTool() == this);
    Assert(i == 0 || (IsLocalPlayerWrappedGift(pItem) && i == 1));

    // Keep in sync with GetUseCommand below.
    if (BIsDirectGift() || (IsLocalPlayerWrappedGift(pItem) && i == 0))
        return "#DeliverGift";
    return "#UnwrapGift";
}

//---------------------------------------------------------------------------------------
// Purpose: Returns the number of use commands available for a wrapped gift.
//---------------------------------------------------------------------------------------
int CEconTool_WrappedGift::GetUseCommandCount(const IEconItemInterface* pItem) const
{
    Assert(pItem && pItem->GetItemDefinition());
    Assert(pItem->GetItemDefinition()->GetEconTool() == this);
    return IsLocalPlayerWrappedGift(pItem) ? 2 : 1;
}

//---------------------------------------------------------------------------------------
// Purpose: Returns the use command for a wrapped gift.
//---------------------------------------------------------------------------------------
const char* CEconTool_WrappedGift::GetUseCommand(const IEconItemInterface* pItem, int i) const
{
    Assert(pItem && pItem->GetItemDefinition());
    Assert(pItem->GetItemDefinition()->GetEconTool() == this);
    Assert(i == 0 || (IsLocalPlayerWrappedGift(pItem) && i == 1));

    if (BIsDirectGift() || (IsLocalPlayerWrappedGift(pItem) && i == 0))
        return "Context_DeliverItem";
    return "Context_UnwrapItem";
}

//---------------------------------------------------------------------------------------
// Purpose: Returns the localization token for the wedding ring use command.
//---------------------------------------------------------------------------------------
const char* CEconTool_WeddingRing::GetUseCommandLocalizationToken(const IEconItemInterface* pItem, int i) const
{
    Assert(i == 0);
    Assert(pItem && pItem->GetItemDefinition());
    Assert(pItem->GetItemDefinition()->GetEconTool() == this);

    static CSchemaAttributeDefHandle pAttrDef_GifterAccountID("gifter account id");
    if (!pItem->FindAttribute(pAttrDef_GifterAccountID))
        return nullptr;
    return "#ToolAction_WeddingRing_AcceptReject";
}

#ifndef TF_CLIENT_DLL
//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Noisemaker tool.
//---------------------------------------------------------------------------------------
void CEconTool_Noisemaker::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_Noisemaker::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Wrapped Gift tool.
//---------------------------------------------------------------------------------------
void CEconTool_WrappedGift::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_WrappedGift::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Wedding Ring tool.
//---------------------------------------------------------------------------------------
void CEconTool_WeddingRing::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_WeddingRing::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Backpack Expander tool.
//---------------------------------------------------------------------------------------
void CEconTool_BackpackExpander::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_BackpackExpander::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Account Upgrade To Premium tool.
//---------------------------------------------------------------------------------------
void CEconTool_AccountUpgradeToPremium::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_AccountUpgradeToPremium::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Claim Code tool.
//---------------------------------------------------------------------------------------
void CEconTool_ClaimCode::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_ClaimCode::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Collection tool.
//---------------------------------------------------------------------------------------
void CEconTool_Collection::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_Collection::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for StrangifierBase tool.
//---------------------------------------------------------------------------------------
void CEconTool_StrangifierBase::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_StrangifierBase::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for PaintCan tool.
//---------------------------------------------------------------------------------------
void CEconTool_PaintCan::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_PaintCan::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Gift tool.
//---------------------------------------------------------------------------------------
void CEconTool_Gift::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_Gift::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Dueling Minigame tool.
//---------------------------------------------------------------------------------------
void CEconTool_DuelingMinigame::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_DuelingMinigame::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Duck Token tool.
//---------------------------------------------------------------------------------------
void CEconTool_DuckToken::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_DuckToken::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Grant Operation Pass tool.
//---------------------------------------------------------------------------------------
void CEconTool_GrantOperationPass::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_GrantOperationPass::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Keyless Case tool.
//---------------------------------------------------------------------------------------
void CEconTool_KeylessCase::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_KeylessCase::OnClientUseConsumable() is unimplemented!");
}

//---------------------------------------------------------------------------------------
// Purpose: Unimplemented client use for Default tool.
//---------------------------------------------------------------------------------------
void CEconTool_Default::OnClientUseConsumable(CEconItemView* pItem, vgui::Panel* pParent) const
{
    Assert(!"CEconTool_Default::OnClientUseConsumable() is unimplemented!");
}
#endif // !TF_CLIENT_DLL
