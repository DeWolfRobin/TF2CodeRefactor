//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: This is a panel which is rendered image on top of an entity
//
// $Revision: $
// $NoKeywords: $
//=============================================================================//
#include "cbase.h"
#include "TeamBitmapImage.h"
#include <KeyValues.h>
#include "vgui_bitmapimage.h"
#include "panelmetaclassmgr.h"
#include "VGuiMatSurface/IMatSystemSurface.h"
#include <vgui_controls/Panel.h>

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
// CTeamBitmapImage: A multiplexer bitmap that chooses a bitmap based on team.
//-----------------------------------------------------------------------------

CTeamBitmapImage::CTeamBitmapImage() : m_Alpha(1.0f), m_pEntity(nullptr), m_bRelativeTeams(false)
{
	// Initialize our bitmap pointer array to nullptr.
	for (int i = 0; i < BITMAP_COUNT; ++i)
	{
		m_ppImage[i] = nullptr;
	}
}

CTeamBitmapImage::~CTeamBitmapImage()
{
	// Delete any allocated BitmapImage objects.
	for (int i = 0; i < BITMAP_COUNT; ++i)
	{
		if (m_ppImage[i])
		{
			delete m_ppImage[i];
			m_ppImage[i] = nullptr;
		}
	}
}


//-----------------------------------------------------------------------------
// Initialization: Sets up team bitmaps based on KeyValues data.
//-----------------------------------------------------------------------------
bool CTeamBitmapImage::Init(vgui::Panel* pParent, KeyValues* pInitData, C_BaseEntity* pEntity)
{
	// Static team names for relative and absolute teams.
	static const char* pRelativeTeamNames[BITMAP_COUNT] =
	{
		"NoTeam",
		"MyTeam",
		"EnemyTeam",
	};

	static const char* pAbsoluteTeamNames[BITMAP_COUNT] =
	{
		"Team0",
		"Team1",
		"Team2",
	};

	m_pEntity = pEntity;
	m_bRelativeTeams = (pInitData->GetInt("relativeteam") != 0);

	// Choose the appropriate set of team names.
	const char** ppTeamNames = m_bRelativeTeams ? pRelativeTeamNames : pAbsoluteTeamNames;

	for (int i = 0; i < BITMAP_COUNT; ++i)
	{
		// Default image pointer to nullptr.
		m_ppImage[i] = nullptr;

		// Look for a team section in the KeyValues data.
		KeyValues* pTeamKV = pInitData->FindKey(ppTeamNames[i]);
		if (!pTeamKV)
			continue;

		// Retrieve the material name for this team.
		const char* pClassImage = pTeamKV->GetString("material");
		if (!pClassImage || !pClassImage[0])
			return false;

		// Get the modulation color; default to white if not provided.
		Color color;
		if (!ParseRGBA(pTeamKV, "color", color))
		{
			color.SetColor(255, 255, 255, 255);
		}

		// Create the bitmap image and set its color.
		m_ppImage[i] = new BitmapImage(pParent->GetVPanel(), pClassImage);
		m_ppImage[i]->SetColor(color);
	}

	return true;
}


//-----------------------------------------------------------------------------
// SetAlpha: Sets the overall alpha modulation (clamped between 0 and 1).
//-----------------------------------------------------------------------------
void CTeamBitmapImage::SetAlpha(float alpha)
{
	m_Alpha = clamp(alpha, 0.0f, 1.0f);
}


//-----------------------------------------------------------------------------
// Paint: Renders the appropriate team bitmap with alpha modulation.
//-----------------------------------------------------------------------------
void CTeamBitmapImage::Paint(float yaw /* = 0.0f */)
{
	if (m_Alpha <= 0.0f)
		return;

	// Cache the entity pointer to avoid redundant calls.
	C_BaseEntity* pEntity = GetEntity();
	int team = 0;
	if (pEntity)
	{
		if (m_bRelativeTeams)
		{
			// If the entity has a team, choose relative team based on local team membership.
			if (pEntity->GetTeamNumber() != 0)
			{
				team = pEntity->InLocalTeam() ? 1 : 2;
			}
		}
		else
		{
			team = pEntity->GetTeamNumber();
		}
	}

	// Paint the image for the current team.
	BitmapImage* pImage = m_ppImage[team];
	if (pImage)
	{
		// Modulate the image color based on the alpha value.
		Color color = pImage->GetColor();
		int originalAlpha = color[3];
		color[3] = originalAlpha * m_Alpha;
		pImage->SetColor(color);

		// If yaw is provided, disable clipping and paint with rotation.
		if (yaw != 0.0f)
		{
			g_pMatSystemSurface->DisableClipping(true);
			pImage->DoPaint(pImage->GetRenderSizePanel(), yaw);
			g_pMatSystemSurface->DisableClipping(false);
		}
		else
		{
			// Standard paint.
			pImage->Paint();
		}

		// Restore the original color.
		color[3] = originalAlpha;
		pImage->SetColor(color);
	}
}


//-----------------------------------------------------------------------------
// InitializeTeamImage: Helper function to initialize a team image from KeyValues.
//-----------------------------------------------------------------------------
bool InitializeTeamImage(KeyValues* pInitData, const char* pSectionName, vgui::Panel* pParent, C_BaseEntity* pEntity, CTeamBitmapImage* pTeamImage)
{
	KeyValues* pTeamImageSection = pInitData;
	if (pSectionName)
	{
		pTeamImageSection = pInitData->FindKey(pSectionName);
		if (!pTeamImageSection)
			return false;
	}

	return pTeamImage->Init(pParent, pTeamImageSection, pEntity);
}
