//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
// 
// Interface to the client system responsible for dealing with shadows.
//
// This module manages client shadows via a shadow manager, shadow texture
// allocator, and various helper functions. It supports both simple “blobby” 
// shadows as well as render‐to‐texture (projected) shadows and flashlights.
//
//=============================================================================//

#include "cbase.h"
#include "engine/ishadowmgr.h"
#include "model_types.h"
#include "bitmap/imageformat.h"
#include "materialsystem/imaterialproxy.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imesh.h"
#include "materialsystem/itexture.h"
#include "bsptreedata.h"
#include "utlmultilist.h"
#include "collisionutils.h"
#include "iviewrender.h"
#include "ivrenderview.h"
#include "tier0/vprof.h"
#include "engine/ivmodelinfo.h"
#include "view_shared.h"
#include "engine/ivdebugoverlay.h"
#include "engine/IStaticPropMgr.h"
#include "datacache/imdlcache.h"
#include "viewrender.h"
#include "tier0/icommandline.h"
#include "vstdlib/jobthread.h"
#include "toolframework_client.h"
#include "bonetoworldarray.h"
#include "cmodel.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
// ConVars
//-----------------------------------------------------------------------------
static ConVar r_flashlightdrawfrustum("r_flashlightdrawfrustum", "0");
static ConVar r_flashlightmodels("r_flashlightmodels", "1");
static ConVar r_shadowrendertotexture("r_shadowrendertotexture", "0");
static ConVar r_flashlight_version2("r_flashlight_version2", "0", FCVAR_CHEAT | FCVAR_DEVELOPMENTONLY);
ConVar r_flashlightdepthtexture("r_flashlightdepthtexture", "1", FCVAR_ALLOWED_IN_COMPETITIVE);
#if defined( _X360 )
ConVar r_flashlightdepthres("r_flashlightdepthres", "512");
#else
ConVar r_flashlightdepthres("r_flashlightdepthres", "1024");
#endif
ConVar r_threaded_client_shadow_manager("r_threaded_client_shadow_manager", "0");

#ifdef _WIN32
#pragma warning( disable: 4701 )
#endif

// Forward declarations
void ToolFramework_RecordMaterialParams(IMaterial* pMaterial);

//-----------------------------------------------------------------------------
// Texture Allocator: Batches textures together into pages.
//-----------------------------------------------------------------------------

typedef unsigned short TextureHandle_t;
enum
{
	INVALID_TEXTURE_HANDLE = (TextureHandle_t)~0
};

class CTextureAllocator
{
public:
	// Initializes the allocator and its render target.
	void Init();
	void Shutdown();

	// Resets allocator state.
	void Reset();

	// Completely deallocates all textures.
	void DeallocateAllTextures();

	// Texture allocation routines.
	TextureHandle_t AllocateTexture(int w, int h);
	void DeallocateTexture(TextureHandle_t h);

	// Use a texture (and optionally force a redraw if necessary).
	bool UseTexture(TextureHandle_t h, bool bWillRedraw, float flArea);
	bool HasValidTexture(TextureHandle_t h);

	// Advance frame (to help manage LRU caching).
	void AdvanceFrame();

	// Retrieve the placement rectangle for a given texture.
	void GetTextureRect(TextureHandle_t handle, int& x, int& y, int& w, int& h);

	// Get the underlying texture page and total texture size.
	ITexture* GetTexture();
	void GetTotalTextureSize(int& w, int& h);

	// Debug dump of the cache.
	void DebugPrintCache(void);

private:
	typedef unsigned short FragmentHandle_t;
	enum
	{
		INVALID_FRAGMENT_HANDLE = (FragmentHandle_t)~0,
		TEXTURE_PAGE_SIZE = 1024,
		MAX_TEXTURE_POWER = 8,
#if !defined( _X360 )
		MIN_TEXTURE_POWER = 4,
#else
		MIN_TEXTURE_POWER = 5,	// per resolve requirements to ensure 32x32 aligned offsets
#endif
		MAX_TEXTURE_SIZE = (1 << MAX_TEXTURE_POWER),
		MIN_TEXTURE_SIZE = (1 << MIN_TEXTURE_POWER),
		BLOCK_SIZE = MAX_TEXTURE_SIZE,
		BLOCKS_PER_ROW = (TEXTURE_PAGE_SIZE / MAX_TEXTURE_SIZE),
		BLOCK_COUNT = (BLOCKS_PER_ROW * BLOCKS_PER_ROW),
	};

	struct TextureInfo_t
	{
		FragmentHandle_t	m_Fragment;
		unsigned short		m_Size;
		unsigned short		m_Power;
	};

	struct FragmentInfo_t
	{
		unsigned short	m_Block;
		unsigned short	m_Index;
		TextureHandle_t	m_Texture;
		unsigned int	m_FrameUsed;
	};

	struct BlockInfo_t
	{
		unsigned short	m_FragmentPower;
	};

	struct Cache_t
	{
		unsigned short	m_List;
	};

	// Internal helper routines.
	void AddBlockToLRU(int block);
	void UnlinkFragmentFromCache(Cache_t& cache, FragmentHandle_t fragment);
	void MarkUsed(FragmentHandle_t fragment);
	void MarkUnused(FragmentHandle_t fragment);
	void DisconnectTextureFromFragment(FragmentHandle_t f);
	int GetFragmentPower(FragmentHandle_t f) const;

	// Members.
	CTextureReference			m_TexturePage;
	CUtlLinkedList<TextureInfo_t, TextureHandle_t> m_Textures;
	CUtlMultiList<FragmentInfo_t, FragmentHandle_t> m_Fragments;
	Cache_t		m_Cache[MAX_TEXTURE_POWER + 1];
	BlockInfo_t	m_Blocks[BLOCK_COUNT];
	unsigned int m_CurrentFrame;
};

//-----------------------------------------------------------------------------
// CTextureAllocator implementation
//-----------------------------------------------------------------------------
void CTextureAllocator::Init()
{
	for (int i = 0; i <= MAX_TEXTURE_POWER; ++i)
	{
		m_Cache[i].m_List = m_Fragments.InvalidIndex();
	}

#if !defined( _X360 )
	m_TexturePage.InitRenderTarget(TEXTURE_PAGE_SIZE, TEXTURE_PAGE_SIZE, RT_SIZE_NO_CHANGE, IMAGE_FORMAT_ARGB8888, MATERIAL_RT_DEPTH_NONE, false, "_rt_Shadows");
#else
	m_TexturePage.InitRenderTargetTexture(TEXTURE_PAGE_SIZE, TEXTURE_PAGE_SIZE, RT_SIZE_NO_CHANGE, IMAGE_FORMAT_ARGB8888, MATERIAL_RT_DEPTH_NONE, false, "_rt_Shadows");
	m_TexturePage.InitRenderTargetSurface(MAX_TEXTURE_SIZE, MAX_TEXTURE_SIZE, IMAGE_FORMAT_ARGB8888, false);
	m_TexturePage->ClearTexture(0, 0, 0, 0);
#endif
}

void CTextureAllocator::Shutdown()
{
	m_TexturePage.Shutdown();
}

void CTextureAllocator::Reset()
{
	DeallocateAllTextures();
	m_Textures.EnsureCapacity(256);
	m_Fragments.EnsureCapacity(256);

	// Set up block sizes heuristically.
#if !defined( _X360 )
	m_Blocks[0].m_FragmentPower = MAX_TEXTURE_POWER - 4;
#else
	m_Blocks[0].m_FragmentPower = MAX_TEXTURE_POWER - 3;
#endif
	m_Blocks[1].m_FragmentPower = MAX_TEXTURE_POWER - 3;
	m_Blocks[2].m_FragmentPower = MAX_TEXTURE_POWER - 2;
	m_Blocks[3].m_FragmentPower = MAX_TEXTURE_POWER - 2;
	m_Blocks[4].m_FragmentPower = MAX_TEXTURE_POWER - 1;
	m_Blocks[5].m_FragmentPower = MAX_TEXTURE_POWER - 1;
	m_Blocks[6].m_FragmentPower = MAX_TEXTURE_POWER - 1;
	m_Blocks[7].m_FragmentPower = MAX_TEXTURE_POWER - 1;
	m_Blocks[8].m_FragmentPower = MAX_TEXTURE_POWER - 1;
	m_Blocks[9].m_FragmentPower = MAX_TEXTURE_POWER - 1;
	m_Blocks[10].m_FragmentPower = MAX_TEXTURE_POWER;
	m_Blocks[11].m_FragmentPower = MAX_TEXTURE_POWER;
	m_Blocks[12].m_FragmentPower = MAX_TEXTURE_POWER;
	m_Blocks[13].m_FragmentPower = MAX_TEXTURE_POWER;
	m_Blocks[14].m_FragmentPower = MAX_TEXTURE_POWER;
	m_Blocks[15].m_FragmentPower = MAX_TEXTURE_POWER;

	// Initialize LRUs for each power.
	for (int i = 0; i <= MAX_TEXTURE_POWER; ++i)
	{
		m_Cache[i].m_List = m_Fragments.CreateList();
	}

	for (int i = 0; i < BLOCK_COUNT; ++i)
	{
		AddBlockToLRU(i);
	}

	m_CurrentFrame = 0;
}

void CTextureAllocator::DeallocateAllTextures()
{
	m_Textures.Purge();
	m_Fragments.Purge();
	for (int i = 0; i <= MAX_TEXTURE_POWER; ++i)
	{
		m_Cache[i].m_List = m_Fragments.InvalidIndex();
	}
}

void CTextureAllocator::DebugPrintCache(void)
{
	int nNumFragments = m_Fragments.TotalCount();
	int nNumInvalidFragments = 0;
	Warning("Fragments (%d):\n===============\n", nNumFragments);
	for (int f = 0; f < nNumFragments; f++)
	{
		if ((m_Fragments[f].m_FrameUsed != 0) && (m_Fragments[f].m_Texture != INVALID_TEXTURE_HANDLE))
			Warning("Fragment %d, Block: %d, Index: %d, Texture: %d Frame Used: %d\n", f, m_Fragments[f].m_Block, m_Fragments[f].m_Index, m_Fragments[f].m_Texture, m_Fragments[f].m_FrameUsed);
		else
			nNumInvalidFragments++;
	}
	Warning("Invalid Fragments: %d\n", nNumInvalidFragments);
}

void CTextureAllocator::AddBlockToLRU(int block)
{
	int power = m_Blocks[block].m_FragmentPower;
	int size = 1 << power;
	int fragmentCount = (MAX_TEXTURE_SIZE / size) * (MAX_TEXTURE_SIZE / size);
	while (--fragmentCount >= 0)
	{
		FragmentHandle_t f = m_Fragments.Alloc();
		m_Fragments[f].m_Block = block;
		m_Fragments[f].m_Index = fragmentCount;
		m_Fragments[f].m_Texture = INVALID_TEXTURE_HANDLE;
		m_Fragments[f].m_FrameUsed = 0xFFFFFFFF;
		m_Fragments.LinkToHead(m_Cache[power].m_List, f);
	}
}

void CTextureAllocator::UnlinkFragmentFromCache(Cache_t& cache, FragmentHandle_t fragment)
{
	m_Fragments.Unlink(cache.m_List, fragment);
}

void CTextureAllocator::MarkUsed(FragmentHandle_t fragment)
{
	int block = m_Fragments[fragment].m_Block;
	int power = m_Blocks[block].m_FragmentPower;
	Cache_t& cache = m_Cache[power];
	m_Fragments.LinkToTail(cache.m_List, fragment);
	m_Fragments[fragment].m_FrameUsed = m_CurrentFrame;
}

void CTextureAllocator::MarkUnused(FragmentHandle_t fragment)
{
	int block = m_Fragments[fragment].m_Block;
	int power = m_Blocks[block].m_FragmentPower;
	Cache_t& cache = m_Cache[power];
	m_Fragments.LinkToHead(cache.m_List, fragment);
}

void CTextureAllocator::DisconnectTextureFromFragment(FragmentHandle_t f)
{
	FragmentInfo_t& info = m_Fragments[f];
	if (info.m_Texture != INVALID_TEXTURE_HANDLE)
	{
		m_Textures[info.m_Texture].m_Fragment = INVALID_FRAGMENT_HANDLE;
		info.m_Texture = INVALID_TEXTURE_HANDLE;
	}
}

int CTextureAllocator::GetFragmentPower(FragmentHandle_t f) const
{
	return m_Blocks[m_Fragments[f].m_Block].m_FragmentPower;
}

TextureHandle_t CTextureAllocator::AllocateTexture(int w, int h)
{
	Assert(w == h);
	if (w < MIN_TEXTURE_SIZE)
		w = MIN_TEXTURE_SIZE;
	else if (w > MAX_TEXTURE_SIZE)
		w = MAX_TEXTURE_SIZE;
	TextureHandle_t handle = m_Textures.AddToTail();
	m_Textures[handle].m_Fragment = INVALID_FRAGMENT_HANDLE;
	m_Textures[handle].m_Size = w;
	int power = 0, size = 1;
	while (size < w)
	{
		size <<= 1;
		++power;
	}
	Assert(size == w);
	m_Textures[handle].m_Power = power;
	return handle;
}

void CTextureAllocator::DeallocateTexture(TextureHandle_t h)
{
	if (m_Textures[h].m_Fragment != INVALID_FRAGMENT_HANDLE)
	{
		MarkUnused(m_Textures[h].m_Fragment);
		m_Fragments[m_Textures[h].m_Fragment].m_FrameUsed = 0xFFFFFFFF;
		DisconnectTextureFromFragment(m_Textures[h].m_Fragment);
	}
	m_Textures.Remove(h);
}

bool CTextureAllocator::HasValidTexture(TextureHandle_t h)
{
	TextureInfo_t& info = m_Textures[h];
	return (info.m_Fragment != INVALID_FRAGMENT_HANDLE);
}

bool CTextureAllocator::UseTexture(TextureHandle_t h, bool bWillRedraw, float flArea)
{
	TextureInfo_t& info = m_Textures[h];
	int nDesiredPower = MIN_TEXTURE_POWER;
	int nDesiredWidth = MIN_TEXTURE_SIZE;
	while ((nDesiredWidth * nDesiredWidth) < flArea)
	{
		if (nDesiredPower >= info.m_Power)
		{
			nDesiredPower = info.m_Power;
			break;
		}
		++nDesiredPower;
		nDesiredWidth <<= 1;
	}
	int nCurrentPower = -1;
	FragmentHandle_t currentFragment = info.m_Fragment;
	if (currentFragment != INVALID_FRAGMENT_HANDLE)
	{
		nCurrentPower = GetFragmentPower(currentFragment);
		Assert(nCurrentPower <= info.m_Power);
		bool bShouldKeepTexture = (!bWillRedraw) && (nDesiredPower < 8) && ((nDesiredPower - nCurrentPower) <= 1);
		if ((nCurrentPower == nDesiredPower) || bShouldKeepTexture)
		{
			MarkUsed(currentFragment);
			return false;
		}
	}
	int power = nDesiredPower;
	FragmentHandle_t f = INVALID_FRAGMENT_HANDLE;
	while (power >= 0)
	{
		f = m_Fragments.Head(m_Cache[power].m_List);
		if ((f != m_Fragments.InvalidIndex()) && (m_Fragments[f].m_FrameUsed != m_CurrentFrame))
			break;
		--power;
	}
	if (currentFragment != INVALID_FRAGMENT_HANDLE)
	{
		if (power <= nCurrentPower)
		{
			MarkUsed(currentFragment);
			return false;
		}
		else
		{
			DisconnectTextureFromFragment(currentFragment);
		}
	}
	if (f == INVALID_FRAGMENT_HANDLE)
		return false;
	DisconnectTextureFromFragment(f);
	info.m_Fragment = f;
	m_Fragments[f].m_Texture = h;
	MarkUsed(f);
	return true;
}

void CTextureAllocator::AdvanceFrame()
{
	++m_CurrentFrame;
}

ITexture* CTextureAllocator::GetTexture()
{
	return m_TexturePage;
}

void CTextureAllocator::GetTotalTextureSize(int& w, int& h)
{
	w = h = TEXTURE_PAGE_SIZE;
}

void CTextureAllocator::GetTextureRect(TextureHandle_t handle, int& x, int& y, int& w, int& h)
{
	TextureInfo_t& info = m_Textures[handle];
	Assert(info.m_Fragment != INVALID_FRAGMENT_HANDLE);
	FragmentInfo_t& fragment = m_Fragments[info.m_Fragment];
	int blockY = fragment.m_Block / BLOCKS_PER_ROW;
	int blockX = fragment.m_Block - blockY * BLOCKS_PER_ROW;
	int fragmentSize = (1 << m_Blocks[fragment.m_Block].m_FragmentPower);
	int fragmentsPerRow = BLOCK_SIZE / fragmentSize;
	int fragmentY = fragment.m_Index / fragmentsPerRow;
	int fragmentX = fragment.m_Index - fragmentY * fragmentsPerRow;
	x = blockX * BLOCK_SIZE + fragmentX * fragmentSize;
	y = blockY * BLOCK_SIZE + fragmentY * fragmentSize;
	w = fragmentSize;
	h = fragmentSize;
}

//-----------------------------------------------------------------------------
// End of CTextureAllocator
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// Shadow constants
//-----------------------------------------------------------------------------
#define TEXEL_SIZE_PER_CASTER_SIZE	2.0f 
#define MAX_FALLOFF_AMOUNT 240
#define MAX_CLIP_PLANE_COUNT 4
#define SHADOW_CULL_TOLERANCE 0.5f

static ConVar r_shadows("r_shadows", "1");
static ConVar r_shadowmaxrendered("r_shadowmaxrendered", "32");
static ConVar r_shadows_gamecontrol("r_shadows_gamecontrol", "-1", FCVAR_CHEAT);

//-----------------------------------------------------------------------------
// CClientShadowMgr: Client-side shadow manager class
//-----------------------------------------------------------------------------
class CClientShadowMgr : public IClientShadowMgr
{
public:
	CClientShadowMgr();

	virtual char const* Name() override { return "CCLientShadowMgr"; }

	// IClientShadowMgr overrides
	virtual bool Init() override;
	virtual void PostInit() override {}
	virtual void Shutdown() override;
	virtual void LevelInitPreEntity() override;
	virtual void LevelInitPostEntity() override {}
	virtual void LevelShutdownPreEntity() override;
	virtual void LevelShutdownPostEntity() override;
	virtual bool IsPerFrame() override { return true; }
	virtual void PreRender() override;
	virtual void Update(float frametime) override {}
	virtual void PostRender() override {}
	virtual void OnSave() override {}
	virtual void OnRestore() override {}
	virtual void SafeRemoveIfDesired() override {}

	virtual ClientShadowHandle_t CreateShadow(ClientEntityHandle_t entity, int flags) override;
	virtual void DestroyShadow(ClientShadowHandle_t handle) override;
	virtual ClientShadowHandle_t CreateFlashlight(const FlashlightState_t& lightState) override;
	virtual void UpdateFlashlightState(ClientShadowHandle_t shadowHandle, const FlashlightState_t& lightState) override;
	virtual void DestroyFlashlight(ClientShadowHandle_t shadowHandle) override;
	virtual void UpdateProjectedTexture(ClientShadowHandle_t handle, bool force) override;

	// Shadow update helpers
	void ComputeBoundingSphere(IClientRenderable* pRenderable, Vector& origin, float& radius);
	virtual void AddToDirtyShadowList(ClientShadowHandle_t handle, bool bForce) override;
	virtual void AddToDirtyShadowList(IClientRenderable* pRenderable, bool force) override;
	virtual void MarkRenderToTextureShadowDirty(ClientShadowHandle_t handle) override;
	void AddShadowToReceiver(ClientShadowHandle_t handle, IClientRenderable* pRenderable, ShadowReceiver_t type);
	void RemoveAllShadowsFromReceiver(IClientRenderable* pRenderable, ShadowReceiver_t type);
	void ComputeShadowTextures(const CViewSetup& view, int leafCount, LeafIndex_t* pLeafList);
	void ComputeShadowDepthTextures(const CViewSetup& view);
	ITexture* GetShadowTexture(unsigned short h);
	const ShadowInfo_t& GetShadowInfo(ClientShadowHandle_t h);
	void RenderShadowTexture(int w, int h);
	virtual void SetShadowDirection(const Vector& dir) override;
	const Vector& GetShadowDirection() const;
	virtual void SetShadowColor(unsigned char r, unsigned char g, unsigned char b) override;
	void GetShadowColor(unsigned char* r, unsigned char* g, unsigned char* b) const;
	virtual void SetShadowDistance(float flMaxDistance) override;
	float GetShadowDistance() const;
	virtual void SetShadowBlobbyCutoffArea(float flMinArea) override;
	float GetBlobbyCutoffArea() const;
	virtual void SetFalloffBias(ClientShadowHandle_t handle, unsigned char ucBias) override;
	void RestoreRenderState();
	void ComputeShadowBBox(IClientRenderable* pRenderable, const Vector& vecAbsCenter, float flRadius, Vector* pAbsMins, Vector* pAbsMaxs);
	bool WillParentRenderBlobbyShadow(IClientRenderable* pRenderable);
	bool ShouldUseParentShadow(IClientRenderable* pRenderable);
	void SetShadowsDisabled(bool bDisabled) { r_shadows_gamecontrol.SetValue(bDisabled ? 0 : -1); }

private:
	enum
	{
		SHADOW_FLAGS_TEXTURE_DIRTY = (CLIENT_SHADOW_FLAGS_LAST_FLAG << 1),
		SHADOW_FLAGS_BRUSH_MODEL = (CLIENT_SHADOW_FLAGS_LAST_FLAG << 2),
		SHADOW_FLAGS_USING_LOD_SHADOW = (CLIENT_SHADOW_FLAGS_LAST_FLAG << 3),
		SHADOW_FLAGS_LIGHT_WORLD = (CLIENT_SHADOW_FLAGS_LAST_FLAG << 4),
	};

	struct ClientShadow_t
	{
		ClientEntityHandle_t	m_Entity;
		ShadowHandle_t			m_ShadowHandle;
		ClientLeafShadowHandle_t m_ClientLeafShadowHandle;
		unsigned short			m_Flags;
		VMatrix					m_WorldToShadow;
		Vector2D				m_WorldSize;
		Vector					m_LastOrigin;
		QAngle					m_LastAngles;
		TextureHandle_t			m_ShadowTexture;
		CTextureReference		m_ShadowDepthTexture;
		int						m_nRenderFrame;
		EHANDLE					m_hTargetEntity;
	};

	// Private helper functions.
	void UpdateStudioShadow(IClientRenderable* pRenderable, ClientShadowHandle_t handle);
	void UpdateBrushShadow(IClientRenderable* pRenderable, ClientShadowHandle_t handle);
	void UpdateShadow(ClientShadowHandle_t handle, bool force);
	IClientRenderable* GetParentShadowEntity(ClientShadowHandle_t handle);
	void AddChildBounds(matrix3x4_t& matWorldToBBox, IClientRenderable* pParent, Vector& vecMins, Vector& vecMaxs);
	void ComputeHierarchicalBounds(IClientRenderable* pRenderable, Vector& vecMins, Vector& vecMaxs);
	void BuildGeneralWorldToShadowMatrix(VMatrix& matWorldToShadow, const Vector& origin, const Vector& dir, const Vector& xvec, const Vector& yvec);
	void BuildWorldToShadowMatrix(VMatrix& matWorldToShadow, const Vector& origin, const Quaternion& quatOrientation);
	void BuildPerspectiveWorldToFlashlightMatrix(VMatrix& matWorldToShadow, const FlashlightState_t& flashlightState);
	void UpdateProjectedTextureInternal(ClientShadowHandle_t handle, bool force);
	float ComputeLocalShadowOrigin(IClientRenderable* pRenderable, const Vector& mins, const Vector& maxs, const Vector& localShadowDir, float backupFactor, Vector& origin);
	void RemoveShadowFromDirtyList(ClientShadowHandle_t handle);
	ShadowType_t GetActualShadowCastType(ClientShadowHandle_t handle) const;
	ShadowType_t GetActualShadowCastType(IClientRenderable* pRenderable) const;
	void BuildOrthoShadow(IClientRenderable* pRenderable, ClientShadowHandle_t handle, const Vector& mins, const Vector& maxs);
	void BuildRenderToTextureShadow(IClientRenderable* pRenderable, ClientShadowHandle_t handle, const Vector& mins, const Vector& maxs);
	void BuildFlashlight(ClientShadowHandle_t handle);
	void SetupRenderToTextureShadow(ClientShadowHandle_t h);
	void CleanUpRenderToTextureShadow(ClientShadowHandle_t h);
	void ComputeExtraClipPlanes(IClientRenderable* pRenderable, ClientShadowHandle_t handle, const Vector* vec, const Vector& mins, const Vector& maxs, const Vector& localShadowDir);
	void ClearExtraClipPlanes(ClientShadowHandle_t h);
	void AddExtraClipPlane(ClientShadowHandle_t h, const Vector& normal, float dist);
	bool CullReceiver(ClientShadowHandle_t handle, IClientRenderable* pRenderable, IClientRenderable* pSourceRenderable);
	bool ComputeSeparatingPlane(IClientRenderable* pRend1, IClientRenderable* pRend2, cplane_t* pPlane);
	void UpdateAllShadows();
	bool DrawRenderToTextureShadow(unsigned short clientShadowHandle, float flArea);
	void DrawRenderToTextureShadowLOD(unsigned short clientShadowHandle);
	bool BuildSetupListForRenderToTextureShadow(unsigned short clientShadowHandle, float flArea);
	void SetRenderToTextureShadowTexCoords(ShadowHandle_t handle, int x, int y, int w, int h);
	void DrawRenderToTextureDebugInfo(IClientRenderable* pRenderable, const Vector& mins, const Vector& maxs);
	void AdvanceFrame();
	float GetShadowDistance(IClientRenderable* pRenderable) const;
	const Vector& GetShadowDirection(IClientRenderable* pRenderable) const;
	void InitDepthTextureShadows();
	void ShutdownDepthTextureShadows();
	void InitRenderToTextureShadows();
	void ShutdownRenderToTextureShadows();
	static bool ShadowHandleCompareFunc(const ClientShadowHandle_t& lhs, const ClientShadowHandle_t& rhs)
	{
		return lhs < rhs;
	}
	ClientShadowHandle_t CreateProjectedTexture(ClientEntityHandle_t entity, int flags);
	bool LockShadowDepthTexture(CTextureReference* shadowDepthTexture);
	void UnlockAllShadowDepthTextures();
	void SetFlashlightTarget(ClientShadowHandle_t shadowHandle, EHANDLE targetEntity);
	void SetFlashlightLightWorld(ClientShadowHandle_t shadowHandle, bool bLightWorld);
	bool IsFlashlightTarget(ClientShadowHandle_t shadowHandle, IClientRenderable* pRenderable);
	int BuildActiveShadowDepthList(const CViewSetup& viewSetup, int nMaxDepthShadows, ClientShadowHandle_t* pActiveDepthShadows);
	void SetViewFlashlightState(int nActiveFlashlightCount, ClientShadowHandle_t* pActiveFlashlights);

	// Private members.
	Vector	m_SimpleShadowDir;
	color32	m_AmbientLightColor;
	CMaterialReference m_SimpleShadow;
	CMaterialReference m_RenderShadow;
	CMaterialReference m_RenderModelShadow;
	CTextureReference m_DummyColorTexture;
	CUtlLinkedList<ClientShadow_t, ClientShadowHandle_t>	m_Shadows;
	CTextureAllocator m_ShadowAllocator;
	bool m_RenderToTextureActive;
	bool m_bRenderTargetNeedsClear;
	bool m_bUpdatingDirtyShadows;
	bool m_bThreaded;
	float m_flShadowCastDist;
	float m_flMinShadowArea;
	CUtlRBTree<ClientShadowHandle_t, unsigned short>	m_DirtyShadows;
	CUtlVector<ClientShadowHandle_t> m_TransparentShadows;
	bool m_bDepthTextureActive;
	int m_nDepthTextureResolution;
	CUtlVector<CTextureReference> m_DepthTextureCache;
	CUtlVector<bool> m_DepthTextureCacheLocks;
	int	m_nMaxDepthTextureShadows;

	friend class CVisibleShadowList;
	friend class CVisibleShadowFrustumList;
};

//-----------------------------------------------------------------------------
// Singleton instance
//-----------------------------------------------------------------------------
static CClientShadowMgr s_ClientShadowMgr;
IClientShadowMgr* g_pClientShadowMgr = &s_ClientShadowMgr;

//-----------------------------------------------------------------------------
// Visible shadow list helper structures
//-----------------------------------------------------------------------------
struct VisibleShadowInfo_t
{
	ClientShadowHandle_t	m_hShadow;
	float					m_flArea;
	Vector					m_vecAbsCenter;
};

class CVisibleShadowList : public IClientLeafShadowEnum
{
public:
	CVisibleShadowList();
	int FindShadows(const CViewSetup* pView, int nLeafCount, LeafIndex_t* pLeafList);
	int GetVisibleShadowCount() const;
	const VisibleShadowInfo_t& GetVisibleShadow(int i) const;
private:
	void EnumShadow(unsigned short clientShadowHandle);
	float ComputeScreenArea(const Vector& vecCenter, float r) const;
	void PrioritySort();
	CUtlVector<VisibleShadowInfo_t> m_ShadowsInView;
	CUtlVector<int>	m_PriorityIndex;
};

static CVisibleShadowList s_VisibleShadowList;
static CUtlVector<C_BaseAnimating*> s_NPCShadowBoneSetups;
static CUtlVector<C_BaseAnimating*> s_NonNPCShadowBoneSetups;

//-----------------------------------------------------------------------------
// CVisibleShadowList implementation
//-----------------------------------------------------------------------------
CVisibleShadowList::CVisibleShadowList() : m_ShadowsInView(0, 64), m_PriorityIndex(0, 64) {}

int CVisibleShadowList::GetVisibleShadowCount() const { return m_ShadowsInView.Count(); }

const VisibleShadowInfo_t& CVisibleShadowList::GetVisibleShadow(int i) const { return m_ShadowsInView[m_PriorityIndex[i]]; }

float CVisibleShadowList::ComputeScreenArea(const Vector& vecCenter, float r) const
{
	CMatRenderContextPtr pRenderContext(materials);
	float flScreenDiameter = pRenderContext->ComputePixelDiameterOfSphere(vecCenter, r);
	return flScreenDiameter * flScreenDiameter;
}

void CVisibleShadowList::EnumShadow(unsigned short clientShadowHandle)
{
	CClientShadowMgr::ClientShadow_t& shadow = s_ClientShadowMgr.m_Shadows[clientShadowHandle];
	if (shadow.m_nRenderFrame == gpGlobals->framecount)
		return;
	if (s_ClientShadowMgr.GetActualShadowCastType(clientShadowHandle) != SHADOWS_RENDER_TO_TEXTURE)
		return;
	const ShadowInfo_t& shadowInfo = shadowmgr->GetInfo(shadow.m_ShadowHandle);
	if (shadowInfo.m_FalloffBias == 255)
		return;
	IClientRenderable* pRenderable = ClientEntityList().GetClientRenderableFromHandle(shadow.m_Entity);
	Assert(pRenderable);
	if (s_ClientShadowMgr.ShouldUseParentShadow(pRenderable) || s_ClientShadowMgr.WillParentRenderBlobbyShadow(pRenderable))
		return;
	Vector vecAbsCenter;
	float flRadius;
	s_ClientShadowMgr.ComputeBoundingSphere(pRenderable, vecAbsCenter, flRadius);
	Vector vecAbsMins, vecAbsMaxs;
	s_ClientShadowMgr.ComputeShadowBBox(pRenderable, vecAbsCenter, flRadius, &vecAbsMins, &vecAbsMaxs);
	if (engine->CullBox(vecAbsMins, vecAbsMaxs))
		return;
	int i = m_ShadowsInView.AddToTail();
	VisibleShadowInfo_t& info = m_ShadowsInView[i];
	info.m_hShadow = clientShadowHandle;
	info.m_flArea = ComputeScreenArea(vecAbsCenter, flRadius);
	shadow.m_nRenderFrame = gpGlobals->framecount;
}

void CVisibleShadowList::PrioritySort()
{
	int nCount = m_ShadowsInView.Count();
	m_PriorityIndex.EnsureCapacity(nCount);
	m_PriorityIndex.RemoveAll();
	for (int i = 0; i < nCount; ++i)
		m_PriorityIndex.AddToTail(i);
	for (int i = 0; i < nCount - 1; ++i)
	{
		int nLargestInd = i;
		float flLargestArea = m_ShadowsInView[m_PriorityIndex[i]].m_flArea;
		for (int j = i + 1; j < nCount; ++j)
		{
			int nIndex = m_PriorityIndex[j];
			if (m_ShadowsInView[nIndex].m_flArea > flLargestArea)
			{
				nLargestInd = j;
				flLargestArea = m_ShadowsInView[nIndex].m_flArea;
			}
		}
		::V_swap(m_PriorityIndex[i], m_PriorityIndex[nLargestInd]);
	}
}

int CVisibleShadowList::FindShadows(const CViewSetup* pView, int nLeafCount, LeafIndex_t* pLeafList)
{
	VPROF_BUDGET("CVisibleShadowList::FindShadows", VPROF_BUDGETGROUP_SHADOW_RENDERING);
	m_ShadowsInView.RemoveAll();
	ClientLeafSystem()->EnumerateShadowsInLeaves(nLeafCount, pLeafList, this);
	int nCount = m_ShadowsInView.Count();
	if (nCount != 0)
		PrioritySort();
	return nCount;
}

//-----------------------------------------------------------------------------
// CVisibleShadowList End
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// CClientShadowMgr implementation
//-----------------------------------------------------------------------------
CClientShadowMgr::CClientShadowMgr() :
	m_DirtyShadows(0, 0, ShadowHandleCompareFunc),
	m_RenderToTextureActive(false),
	m_bDepthTextureActive(false)
{
	m_nDepthTextureResolution = r_flashlightdepthres.GetInt();
	m_bThreaded = false;
}

CON_COMMAND_F(r_shadowdir, "Set shadow direction", FCVAR_CHEAT)
{
	if (args.ArgC() == 1)
	{
		Vector dir = s_ClientShadowMgr.GetShadowDirection();
		Msg("%.2f %.2f %.2f\n", dir.x, dir.y, dir.z);
		return;
	}
	if (args.ArgC() == 4)
	{
		Vector dir;
		dir.x = atof(args[1]);
		dir.y = atof(args[2]);
		dir.z = atof(args[3]);
		s_ClientShadowMgr.SetShadowDirection(dir);
	}
}

CON_COMMAND_F(r_shadowangles, "Set shadow angles", FCVAR_CHEAT)
{
	if (args.ArgC() == 1)
	{
		Vector dir = s_ClientShadowMgr.GetShadowDirection();
		QAngle angles;
		VectorAngles(dir, angles);
		Msg("%.2f %.2f %.2f\n", angles.x, angles.y, angles.z);
		return;
	}
	if (args.ArgC() == 4)
	{
		Vector dir;
		QAngle angles;
		angles.x = atof(args[1]);
		angles.y = atof(args[2]);
		angles.z = atof(args[3]);
		AngleVectors(angles, &dir);
		s_ClientShadowMgr.SetShadowDirection(dir);
	}
}

CON_COMMAND_F(r_shadowcolor, "Set shadow color", FCVAR_CHEAT)
{
	if (args.ArgC() == 1)
	{
		unsigned char r, g, b;
		s_ClientShadowMgr.GetShadowColor(&r, &g, &b);
		Msg("Shadow color %d %d %d\n", r, g, b);
		return;
	}
	if (args.ArgC() == 4)
	{
		int r = atoi(args[1]);
		int g = atoi(args[2]);
		int b = atoi(args[3]);
		s_ClientShadowMgr.SetShadowColor(r, g, b);
	}
}

CON_COMMAND_F(r_shadowdist, "Set shadow distance", FCVAR_CHEAT)
{
	if (args.ArgC() == 1)
	{
		float flDist = s_ClientShadowMgr.GetShadowDistance();
		Msg("Shadow distance %.2f\n", flDist);
		return;
	}
	if (args.ArgC() == 2)
	{
		float flDistance = atof(args[1]);
		s_ClientShadowMgr.SetShadowDistance(flDistance);
	}
}

CON_COMMAND_F(r_shadowblobbycutoff, "Set shadow blobby cutoff area", FCVAR_CHEAT)
{
	if (args.ArgC() == 1)
	{
		float flArea = s_ClientShadowMgr.GetBlobbyCutoffArea();
		Msg("Cutoff area %.2f\n", flArea);
		return;
	}
	if (args.ArgC() == 2)
	{
		float flArea = atof(args[1]);
		s_ClientShadowMgr.SetShadowBlobbyCutoffArea(flArea);
	}
}

static void ShadowRestoreFunc(int nChangeFlags)
{
	s_ClientShadowMgr.RestoreRenderState();
}

bool CClientShadowMgr::Init()
{
	m_bRenderTargetNeedsClear = false;
	m_SimpleShadow.Init("decals/simpleshadow", TEXTURE_GROUP_DECAL);
	Vector dir(0.1f, 0.1f, -1.f);
	SetShadowDirection(dir);
	SetShadowDistance(50.f);
	SetShadowBlobbyCutoffArea(0.005f);
	bool bTools = CommandLine()->CheckParm("-tools") != NULL;
	m_nMaxDepthTextureShadows = bTools ? 4 : 1;
	bool bLowEnd = (g_pMaterialSystemHardwareConfig->GetDXSupportLevel() < 80);
	if (!bLowEnd && r_shadowrendertotexture.GetBool())
	{
		InitRenderToTextureShadows();
	}
	if (r_flashlightdepthtexture.GetBool() && !materials->SupportsShadowDepthTextures())
	{
		r_flashlightdepthtexture.SetValue(0);
		ShutdownDepthTextureShadows();
	}
	if (!bLowEnd && r_flashlightdepthtexture.GetBool())
	{
		InitDepthTextureShadows();
	}
	materials->AddRestoreFunc(ShadowRestoreFunc);
	return true;
}

void CClientShadowMgr::Shutdown()
{
	m_SimpleShadow.Shutdown();
	m_Shadows.RemoveAll();
	ShutdownRenderToTextureShadows();
	ShutdownDepthTextureShadows();
	materials->RemoveRestoreFunc(ShadowRestoreFunc);
}

void CClientShadowMgr::InitDepthTextureShadows()
{
	VPROF_BUDGET("CClientShadowMgr::InitDepthTextureShadows", VPROF_BUDGETGROUP_SHADOW_DEPTH_TEXTURING);
	if (!m_bDepthTextureActive)
	{
		m_bDepthTextureActive = true;
		ImageFormat dstFormat = materials->GetShadowDepthTextureFormat();
#if !defined( _X360 )
		ImageFormat nullFormat = materials->GetNullTextureFormat();
#endif
		materials->BeginRenderTargetAllocation();
#if defined( _X360 )
		m_DummyColorTexture.InitRenderTargetTexture(r_flashlightdepthres.GetInt(), r_flashlightdepthres.GetInt(), RT_SIZE_OFFSCREEN, IMAGE_FORMAT_BGR565, MATERIAL_RT_DEPTH_SHARED, false, "_rt_ShadowDummy");
		m_DummyColorTexture.InitRenderTargetSurface(r_flashlightdepthres.GetInt(), r_flashlightdepthres.GetInt(), IMAGE_FORMAT_BGR565, true);
#else
		m_DummyColorTexture.InitRenderTarget(r_flashlightdepthres.GetInt(), r_flashlightdepthres.GetInt(), RT_SIZE_OFFSCREEN, nullFormat, MATERIAL_RT_DEPTH_NONE, false, "_rt_ShadowDummy");
#endif
		m_DepthTextureCache.Purge();
		m_DepthTextureCacheLocks.Purge();
		for (int i = 0; i < m_nMaxDepthTextureShadows; i++)
		{
			CTextureReference depthTex;
			bool bFalse = false;
			char strRTName[64];
			Q_snprintf(strRTName, ARRAYSIZE(strRTName), "_rt_ShadowDepthTexture_%d", i);
#if defined( _X360 )
			depthTex.InitRenderTargetTexture(m_nDepthTextureResolution, m_nDepthTextureResolution, RT_SIZE_OFFSCREEN, dstFormat, MATERIAL_RT_DEPTH_NONE, false, strRTName);
			depthTex.InitRenderTargetSurface(1, 1, dstFormat, false);
#else
			depthTex.InitRenderTarget(m_nDepthTextureResolution, m_nDepthTextureResolution, RT_SIZE_OFFSCREEN, dstFormat, MATERIAL_RT_DEPTH_NONE, false, strRTName);
#endif
			if (i == 0)
			{
				m_nDepthTextureResolution = depthTex->GetActualWidth();
				r_flashlightdepthres.SetValue(m_nDepthTextureResolution);
			}
			m_DepthTextureCache.AddToTail(depthTex);
			m_DepthTextureCacheLocks.AddToTail(bFalse);
		}
		materials->EndRenderTargetAllocation();
	}
}

void CClientShadowMgr::ShutdownDepthTextureShadows()
{
	if (m_bDepthTextureActive)
	{
		m_DummyColorTexture.Shutdown();
		while (m_DepthTextureCache.Count())
		{
			m_DepthTextureCache[m_DepthTextureCache.Count() - 1].Shutdown();
			m_DepthTextureCacheLocks.Remove(m_DepthTextureCache.Count() - 1);
			m_DepthTextureCache.Remove(m_DepthTextureCache.Count() - 1);
		}
		m_bDepthTextureActive = false;
	}
}

void CClientShadowMgr::InitRenderToTextureShadows()
{
	if (!m_RenderToTextureActive)
	{
		m_RenderToTextureActive = true;
		m_RenderShadow.Init("decals/rendershadow", TEXTURE_GROUP_DECAL);
		m_RenderModelShadow.Init("decals/rendermodelshadow", TEXTURE_GROUP_DECAL);
		m_ShadowAllocator.Init();
		m_ShadowAllocator.Reset();
		m_bRenderTargetNeedsClear = true;
		float fr = m_AmbientLightColor.r / 255.0f;
		float fg = m_AmbientLightColor.g / 255.0f;
		float fb = m_AmbientLightColor.b / 255.0f;
		m_RenderShadow->ColorModulate(fr, fg, fb);
		m_RenderModelShadow->ColorModulate(fr, fg, fb);
		for (ClientShadowHandle_t i = m_Shadows.Head(); i != m_Shadows.InvalidIndex(); i = m_Shadows.Next(i))
		{
			ClientShadow_t& shadow = m_Shadows[i];
			if (shadow.m_Flags & SHADOW_FLAGS_USE_RENDER_TO_TEXTURE)
			{
				SetupRenderToTextureShadow(i);
				MarkRenderToTextureShadowDirty(i);
				shadowmgr->SetShadowMaterial(shadow.m_ShadowHandle, m_RenderShadow, m_RenderModelShadow, (void*)(uintp)i);
			}
		}
	}
}

void CClientShadowMgr::ShutdownRenderToTextureShadows()
{
	if (m_RenderToTextureActive)
	{
		for (ClientShadowHandle_t i = m_Shadows.Head(); i != m_Shadows.InvalidIndex(); i = m_Shadows.Next(i))
		{
			CleanUpRenderToTextureShadow(i);
			ClientShadow_t& shadow = m_Shadows[i];
			shadowmgr->SetShadowMaterial(shadow.m_ShadowHandle, m_SimpleShadow, m_SimpleShadow, (void*)CLIENTSHADOW_INVALID_HANDLE);
			shadowmgr->SetShadowTexCoord(shadow.m_ShadowHandle, 0, 0, 1, 1);
			ClearExtraClipPlanes(i);
		}
		m_RenderShadow.Shutdown();
		m_RenderModelShadow.Shutdown();
		m_ShadowAllocator.DeallocateAllTextures();
		m_ShadowAllocator.Shutdown();
		materials->UncacheUnusedMaterials();
		m_RenderToTextureActive = false;
	}
}

void CClientShadowMgr::SetShadowColor(unsigned char r, unsigned char g, unsigned char b)
{
	float fr = r / 255.0f;
	float fg = g / 255.0f;
	float fb = b / 255.0f;
	m_SimpleShadow->ColorModulate(fr, fg, fb);
	if (m_RenderToTextureActive)
	{
		m_RenderShadow->ColorModulate(fr, fg, fb);
		m_RenderModelShadow->ColorModulate(fr, fg, fb);
	}
	m_AmbientLightColor.r = r;
	m_AmbientLightColor.g = g;
	m_AmbientLightColor.b = b;
}

void CClientShadowMgr::GetShadowColor(unsigned char* r, unsigned char* g, unsigned char* b) const
{
	*r = m_AmbientLightColor.r;
	*g = m_AmbientLightColor.g;
	*b = m_AmbientLightColor.b;
}

void CClientShadowMgr::LevelInitPreEntity()
{
	m_bUpdatingDirtyShadows = false;
	Vector ambientColor;
	engine->GetAmbientLightColor(ambientColor);
	ambientColor *= 3;
	ambientColor += Vector(0.3f, 0.3f, 0.3f);
	unsigned char r = (ambientColor[0] > 1.0f) ? 255 : 255 * ambientColor[0];
	unsigned char g = (ambientColor[1] > 1.0f) ? 255 : 255 * ambientColor[1];
	unsigned char b = (ambientColor[2] > 1.0f) ? 255 : 255 * ambientColor[2];
	SetShadowColor(r, g, b);
	if (m_RenderToTextureActive)
	{
		m_ShadowAllocator.Reset();
		m_bRenderTargetNeedsClear = true;
	}
}

void CClientShadowMgr::LevelShutdownPostEntity()
{
	Assert(m_Shadows.Count() == 0);
	ClientShadowHandle_t h = m_Shadows.Head();
	while (h != CLIENTSHADOW_INVALID_HANDLE)
	{
		ClientShadowHandle_t next = m_Shadows.Next(h);
		DestroyShadow(h);
		h = next;
	}
	if (m_RenderToTextureActive)
		m_ShadowAllocator.DeallocateAllTextures();
	r_shadows_gamecontrol.SetValue(-1);
}

void CClientShadowMgr::RestoreRenderState()
{
	for (ClientShadowHandle_t h = m_Shadows.Head(); h != m_Shadows.InvalidIndex(); h = m_Shadows.Next(h))
	{
		m_Shadows[h].m_Flags |= SHADOW_FLAGS_TEXTURE_DIRTY;
	}
	SetShadowColor(m_AmbientLightColor.r, m_AmbientLightColor.g, m_AmbientLightColor.b);
	m_bRenderTargetNeedsClear = true;
}

void CClientShadowMgr::SetupRenderToTextureShadow(ClientShadowHandle_t h)
{
	ClientShadow_t& shadow = m_Shadows[h];
	IClientRenderable* pRenderable = ClientEntityList().GetClientRenderableFromHandle(shadow.m_Entity);
	if (!pRenderable)
		return;
	Vector mins, maxs;
	pRenderable->GetShadowRenderBounds(mins, maxs, GetActualShadowCastType(h));
	Vector size;
	VectorSubtract(maxs, mins, size);
	float maxSize = MAX(MAX(size.x, size.y), size.z);
	int texelCount = TEXEL_SIZE_PER_CASTER_SIZE * maxSize;
	int textureSize = 1;
	while (textureSize < texelCount)
		textureSize <<= 1;
	shadow.m_ShadowTexture = m_ShadowAllocator.AllocateTexture(textureSize, textureSize);
}

void CClientShadowMgr::CleanUpRenderToTextureShadow(ClientShadowHandle_t h)
{
	ClientShadow_t& shadow = m_Shadows[h];
	if (m_RenderToTextureActive && (shadow.m_Flags & SHADOW_FLAGS_USE_RENDER_TO_TEXTURE))
	{
		m_ShadowAllocator.DeallocateTexture(shadow.m_ShadowTexture);
		shadow.m_ShadowTexture = INVALID_TEXTURE_HANDLE;
	}
}

void CClientShadowMgr::UpdateAllShadows()
{
	for (ClientShadowHandle_t i = m_Shadows.Head(); i != m_Shadows.InvalidIndex(); i = m_Shadows.Next(i))
	{
		ClientShadow_t& shadow = m_Shadows[i];
		if (shadow.m_Flags & SHADOW_FLAGS_FLASHLIGHT)
			continue;
		IClientRenderable* pRenderable = ClientEntityList().GetClientRenderableFromHandle(shadow.m_Entity);
		if (!pRenderable)
			continue;
		Assert(pRenderable->GetShadowHandle() == i);
		AddToDirtyShadowList(pRenderable, true);
	}
}

void CClientShadowMgr::SetShadowDirection(const Vector& dir)
{
	VectorCopy(dir, m_SimpleShadowDir);
	VectorNormalize(m_SimpleShadowDir);
	if (m_RenderToTextureActive)
		UpdateAllShadows();
}

const Vector& CClientShadowMgr::GetShadowDirection() const
{
	static Vector s_vecDown(0, 0, -1);
	if (!m_RenderToTextureActive)
		return s_vecDown;
	return m_SimpleShadowDir;
}

float CClientShadowMgr::GetShadowDistance(IClientRenderable* pRenderable) const
{
	float flDist = m_flShadowCastDist;
	pRenderable->GetShadowCastDistance(&flDist, GetActualShadowCastType(pRenderable));
	return flDist;
}

const Vector& CClientShadowMgr::GetShadowDirection(IClientRenderable* pRenderable) const
{
	Vector& vecResult = AllocTempVector();
	vecResult = GetShadowDirection();
	pRenderable->GetShadowCastDirection(&vecResult, GetActualShadowCastType(pRenderable));
	return vecResult;
}

void CClientShadowMgr::SetShadowDistance(float flMaxDistance)
{
	m_flShadowCastDist = flMaxDistance;
	UpdateAllShadows();
}

float CClientShadowMgr::GetShadowDistance() const
{
	return m_flShadowCastDist;
}

void CClientShadowMgr::SetShadowBlobbyCutoffArea(float flMinArea)
{
	m_flMinShadowArea = flMinArea;
}

float CClientShadowMgr::GetBlobbyCutoffArea() const
{
	return m_flMinShadowArea;
}

void CClientShadowMgr::SetFalloffBias(ClientShadowHandle_t handle, unsigned char ucBias)
{
	shadowmgr->SetFalloffBias(m_Shadows[handle].m_ShadowHandle, ucBias);
}

ITexture* CClientShadowMgr::GetShadowTexture(unsigned short h)
{
	return m_ShadowAllocator.GetTexture();
}

const ShadowInfo_t& CClientShadowMgr::GetShadowInfo(ClientShadowHandle_t h)
{
	return shadowmgr->GetInfo(m_Shadows[h].m_ShadowHandle);
}

void CClientShadowMgr::RenderShadowTexture(int w, int h)
{
	if (m_RenderToTextureActive)
	{
		CMatRenderContextPtr pRenderContext(materials);
		pRenderContext->Bind(m_RenderShadow);
		IMesh* pMesh = pRenderContext->GetDynamicMesh(true);
		CMeshBuilder meshBuilder;
		meshBuilder.Begin(pMesh, MATERIAL_QUADS, 1);
		meshBuilder.Position3f(0.0f, 0.0f, 0.0f);
		meshBuilder.TexCoord2f(0, 0.0f, 0.0f);
		meshBuilder.Color4ub(0, 0, 0, 0);
		meshBuilder.AdvanceVertex();
		meshBuilder.Position3f(w, 0.0f, 0.0f);
		meshBuilder.TexCoord2f(0, 1.0f, 0.0f);
		meshBuilder.Color4ub(0, 0, 0, 0);
		meshBuilder.AdvanceVertex();
		meshBuilder.Position3f(w, h, 0.0f);
		meshBuilder.TexCoord2f(0, 1.0f, 1.0f);
		meshBuilder.Color4ub(0, 0, 0, 0);
		meshBuilder.AdvanceVertex();
		meshBuilder.Position3f(0.0f, h, 0.0f);
		meshBuilder.TexCoord2f(0, 0.0f, 1.0f);
		meshBuilder.Color4ub(0, 0, 0, 0);
		meshBuilder.AdvanceVertex();
		meshBuilder.End();
		pMesh->Draw();
	}
}

ClientShadowHandle_t CClientShadowMgr::CreateProjectedTexture(ClientEntityHandle_t entity, int flags)
{
	if (!(flags & SHADOW_FLAGS_FLASHLIGHT))
	{
		IClientRenderable* pRenderable = ClientEntityList().GetClientRenderableFromHandle(entity);
		if (!pRenderable)
			return m_Shadows.InvalidIndex();
		int modelType = modelinfo->GetModelType(pRenderable->GetModel());
		if (modelType == mod_brush)
			flags |= SHADOW_FLAGS_BRUSH_MODEL;
	}
	ClientShadowHandle_t h = m_Shadows.AddToTail();
	ClientShadow_t& shadow = m_Shadows[h];
	shadow.m_Entity = entity;
	shadow.m_ClientLeafShadowHandle = ClientLeafSystem()->AddShadow(h, flags);
	shadow.m_Flags = flags;
	shadow.m_nRenderFrame = -1;
	shadow.m_LastOrigin.Init(FLT_MAX, FLT_MAX, FLT_MAX);
	shadow.m_LastAngles.Init(FLT_MAX, FLT_MAX, FLT_MAX);
	Assert(((shadow.m_Flags & SHADOW_FLAGS_FLASHLIGHT) == 0) != ((shadow.m_Flags & SHADOW_FLAGS_SHADOW) == 0));
	IMaterial* pShadowMaterial = m_SimpleShadow;
	IMaterial* pShadowModelMaterial = m_SimpleShadow;
	void* pShadowProxyData = (void*)CLIENTSHADOW_INVALID_HANDLE;
	if (m_RenderToTextureActive && (flags & SHADOW_FLAGS_USE_RENDER_TO_TEXTURE))
	{
		SetupRenderToTextureShadow(h);
		pShadowMaterial = m_RenderShadow;
		pShadowModelMaterial = m_RenderModelShadow;
		pShadowProxyData = (void*)(uintp)h;
	}
	if (flags & SHADOW_FLAGS_USE_DEPTH_TEXTURE)
	{
		pShadowMaterial = m_RenderShadow;
		pShadowModelMaterial = m_RenderModelShadow;
		pShadowProxyData = (void*)(uintp)h;
	}
	int createShadowFlags = (flags & SHADOW_FLAGS_FLASHLIGHT) ? SHADOW_FLASHLIGHT : SHADOW_CACHE_VERTS;
	shadow.m_ShadowHandle = shadowmgr->CreateShadowEx(pShadowMaterial, pShadowModelMaterial, pShadowProxyData, createShadowFlags);
	return h;
}

ClientShadowHandle_t CClientShadowMgr::CreateFlashlight(const FlashlightState_t& lightState)
{
	static ClientEntityHandle_t invalidHandle = INVALID_CLIENTENTITY_HANDLE;
	int shadowFlags = SHADOW_FLAGS_FLASHLIGHT | SHADOW_FLAGS_LIGHT_WORLD;
	if (lightState.m_bEnableShadows && r_flashlightdepthtexture.GetBool())
		shadowFlags |= SHADOW_FLAGS_USE_DEPTH_TEXTURE;
	ClientShadowHandle_t shadowHandle = CreateProjectedTexture(invalidHandle, shadowFlags);
	UpdateFlashlightState(shadowHandle, lightState);
	UpdateProjectedTexture(shadowHandle, true);
	return shadowHandle;
}

ClientShadowHandle_t CClientShadowMgr::CreateShadow(ClientEntityHandle_t entity, int flags)
{
	flags &= ~SHADOW_FLAGS_PROJECTED_TEXTURE_TYPE_MASK;
	flags |= SHADOW_FLAGS_SHADOW | SHADOW_FLAGS_TEXTURE_DIRTY;
	ClientShadowHandle_t shadowHandle = CreateProjectedTexture(entity, flags);
	IClientRenderable* pRenderable = ClientEntityList().GetClientRenderableFromHandle(entity);
	if (pRenderable)
	{
		Assert(!pRenderable->IsShadowDirty());
		pRenderable->MarkShadowDirty(true);
	}
	AddToDirtyShadowList(shadowHandle, true);
	return shadowHandle;
}

void CClientShadowMgr::UpdateFlashlightState(ClientShadowHandle_t shadowHandle, const FlashlightState_t& flashlightState)
{
	VPROF_BUDGET("CClientShadowMgr::UpdateFlashlightState", VPROF_BUDGETGROUP_SHADOW_DEPTH_TEXTURING);
	BuildPerspectiveWorldToFlashlightMatrix(m_Shadows[shadowHandle].m_WorldToShadow, flashlightState);
	shadowmgr->UpdateFlashlightState(m_Shadows[shadowHandle].m_ShadowHandle, flashlightState);
}

void CClientShadowMgr::DestroyFlashlight(ClientShadowHandle_t shadowHandle)
{
	DestroyShadow(shadowHandle);
}

void CClientShadowMgr::RemoveShadowFromDirtyList(ClientShadowHandle_t handle)
{
	int idx = m_DirtyShadows.Find(handle);
	if (idx != m_DirtyShadows.InvalidIndex())
	{
		IClientRenderable* pRenderable = ClientEntityList().GetClientRenderableFromHandle(m_Shadows[handle].m_Entity);
		if (pRenderable)
			pRenderable->MarkShadowDirty(false);
		m_DirtyShadows.RemoveAt(idx);
	}
}

void CClientShadowMgr::DestroyShadow(ClientShadowHandle_t handle)
{
	Assert(m_Shadows.IsValidIndex(handle));
	RemoveShadowFromDirtyList(handle);
	shadowmgr->DestroyShadow(m_Shadows[handle].m_ShadowHandle);
	ClientLeafSystem()->RemoveShadow(m_Shadows[handle].m_ClientLeafShadowHandle);
	CleanUpRenderToTextureShadow(handle);
	m_Shadows.Remove(handle);
}

void CClientShadowMgr::BuildGeneralWorldToShadowMatrix(VMatrix& matWorldToShadow,
	const Vector& origin, const Vector& dir, const Vector& xvec, const Vector& yvec)
{
	matWorldToShadow.SetBasisVectors(xvec, yvec, dir);
	matWorldToShadow.SetTranslation(origin);
	matWorldToShadow[3][0] = matWorldToShadow[3][1] = matWorldToShadow[3][2] = 0.0f;
	matWorldToShadow[3][3] = 1.0f;
	MatrixInverseGeneral(matWorldToShadow, matWorldToShadow);
}

void CClientShadowMgr::BuildWorldToShadowMatrix(VMatrix& matWorldToShadow, const Vector& origin, const Quaternion& quatOrientation)
{
	matrix3x4_t matOrientation;
	QuaternionMatrix(quatOrientation, matOrientation);
	PositionMatrix(vec3_origin, matOrientation);
	VMatrix matBasis(matOrientation);
	Vector vForward, vLeft, vUp;
	matBasis.GetBasisVectors(vForward, vLeft, vUp);
	matBasis.SetForward(vLeft);
	matBasis.SetLeft(vUp);
	matBasis.SetUp(vForward);
	matWorldToShadow = matBasis.Transpose();
	Vector translation;
	Vector3DMultiply(matWorldToShadow, origin, translation);
	translation *= -1.0f;
	matWorldToShadow.SetTranslation(translation);
	matWorldToShadow[3][0] = matWorldToShadow[3][1] = matWorldToShadow[3][2] = 0.0f;
	matWorldToShadow[3][3] = 1.0f;
}

void CClientShadowMgr::BuildPerspectiveWorldToFlashlightMatrix(VMatrix& matWorldToShadow, const FlashlightState_t& flashlightState)
{
	VPROF_BUDGET("CClientShadowMgr::BuildPerspectiveWorldToFlashlightMatrix", VPROF_BUDGETGROUP_SHADOW_DEPTH_TEXTURING);
	VMatrix matWorldToShadowView, matPerspective;
	BuildWorldToShadowMatrix(matWorldToShadowView, flashlightState.m_vecLightOrigin, flashlightState.m_quatOrientation);
	MatrixBuildPerspective(matPerspective, flashlightState.m_fHorizontalFOVDegrees, flashlightState.m_fVerticalFOVDegrees, flashlightState.m_NearZ, flashlightState.m_FarZ);
	MatrixMultiply(matPerspective, matWorldToShadowView, matWorldToShadow);
}

float CClientShadowMgr::ComputeLocalShadowOrigin(IClientRenderable* pRenderable, const Vector& mins, const Vector& maxs, const Vector& localShadowDir, float backupFactor, Vector& origin)
{
	Vector vecCentroid;
	VectorAdd(mins, maxs, vecCentroid);
	vecCentroid *= 0.5f;
	Vector vecSize;
	VectorSubtract(maxs, mins, vecSize);
	float flRadius = vecSize.Length() * 0.5f;
	float centroidProjection = DotProduct(vecCentroid, localShadowDir);
	float minDist = -centroidProjection;
	for (int i = 0; i < 3; ++i)
	{
		minDist += (localShadowDir[i] > 0.0f) ? localShadowDir[i] * mins[i] : localShadowDir[i] * maxs[i];
	}
	minDist *= backupFactor;
	VectorMA(vecCentroid, minDist, localShadowDir, origin);
	return flRadius - minDist;
}

static inline void SortAbsVectorComponents(const Vector& src, int* pVecIdx)
{
	Vector absVec(fabs(src[0]), fabs(src[1]), fabs(src[2]));
	int maxIdx = (absVec[0] > absVec[1]) ? 0 : 1;
	if (absVec[2] > absVec[maxIdx])
		maxIdx = 2;
	switch (maxIdx)
	{
	case 0:
		pVecIdx[0] = 1; pVecIdx[1] = 2; pVecIdx[2] = 0; break;
	case 1:
		pVecIdx[0] = 2; pVecIdx[1] = 0; pVecIdx[2] = 1; break;
	case 2:
		pVecIdx[0] = 0; pVecIdx[1] = 1; pVecIdx[2] = 2; break;
	}
}

static void BuildWorldToTextureMatrix(const VMatrix& matWorldToShadow, const Vector2D& size, VMatrix& matWorldToTexture)
{
	VMatrix shadowToUnit;
	MatrixBuildScale(shadowToUnit, 1.0f / size.x, 1.0f / size.y, 1.0f);
	shadowToUnit[0][3] = shadowToUnit[1][3] = 0.5f;
	MatrixMultiply(shadowToUnit, matWorldToShadow, matWorldToTexture);
}

static void BuildOrthoWorldToShadowMatrix(VMatrix& worldToShadow, const Vector& origin, const Vector& dir, const Vector& xvec, const Vector& yvec)
{
	AssertFloatEquals(DotProduct(dir, xvec), 0.0f, 1e-3);
	AssertFloatEquals(DotProduct(dir, yvec), 0.0f, 1e-3);
	AssertFloatEquals(DotProduct(xvec, yvec), 0.0f, 1e-3);
	worldToShadow.SetBasisVectors(xvec, yvec, dir);
	MatrixTranspose(worldToShadow, worldToShadow);
	Vector translation;
	Vector3DMultiply(worldToShadow, origin, translation);
	translation *= -1.0f;
	worldToShadow.SetTranslation(translation);
	worldToShadow[3][0] = worldToShadow[3][1] = worldToShadow[3][2] = 0.0f;
	worldToShadow[3][3] = 1.0f;
}

void CClientShadowMgr::ClearExtraClipPlanes(ClientShadowHandle_t h)
{
	shadowmgr->ClearExtraClipPlanes(m_Shadows[h].m_ShadowHandle);
}

void CClientShadowMgr::AddExtraClipPlane(ClientShadowHandle_t h, const Vector& normal, float dist)
{
	shadowmgr->AddExtraClipPlane(m_Shadows[h].m_ShadowHandle, normal, dist);
}

void CClientShadowMgr::ComputeExtraClipPlanes(IClientRenderable* pRenderable, ClientShadowHandle_t handle, const Vector* vec, const Vector& mins, const Vector& maxs, const Vector& localShadowDir)
{
	Vector origin = pRenderable->GetRenderOrigin();
	float dir[3];
	for (int i = 0; i < 3; ++i)
	{
		if (localShadowDir[i] < 0.0f)
		{
			VectorMA(origin, maxs[i], vec[i], origin);
			dir[i] = 1;
		}
		else
		{
			VectorMA(origin, mins[i], vec[i], origin);
			dir[i] = -1;
		}
	}
	Vector normal;
	ClearExtraClipPlanes(handle);
	for (int i = 0; i < 3; ++i)
	{
		VectorMultiply(vec[i], dir[i], normal);
		float dist = DotProduct(normal, origin);
		AddExtraClipPlane(handle, normal, dist);
	}
	ClientShadow_t& shadow = m_Shadows[handle];
	C_BaseEntity* pEntity = ClientEntityList().GetBaseEntityFromHandle(shadow.m_Entity);
	if (pEntity && pEntity->m_bEnableRenderingClipPlane)
	{
		normal[0] = -pEntity->m_fRenderingClipPlane[0];
		normal[1] = -pEntity->m_fRenderingClipPlane[1];
		normal[2] = -pEntity->m_fRenderingClipPlane[2];
		AddExtraClipPlane(handle, normal, -pEntity->m_fRenderingClipPlane[3] - 0.5f);
	}
}

bool CClientShadowMgr::ComputeSeparatingPlane(IClientRenderable* pRend1, IClientRenderable* pRend2, cplane_t* pPlane)
{
	Vector min1, max1, min2, max2;
	pRend1->GetShadowRenderBounds(min1, max1, GetActualShadowCastType(pRend1));
	pRend2->GetShadowRenderBounds(min2, max2, GetActualShadowCastType(pRend2));
	return ::ComputeSeparatingPlane(pRend1->GetRenderOrigin(), pRend1->GetRenderAngles(), min1, max1,
		pRend2->GetRenderOrigin(), pRend2->GetRenderAngles(), min2, max2, 3.0f, pPlane);
}

void CClientShadowMgr::AddToDirtyShadowList(ClientShadowHandle_t handle, bool bForce)
{
	if (m_bUpdatingDirtyShadows)
		return;
	if (handle == CLIENTSHADOW_INVALID_HANDLE)
		return;
	Assert(m_DirtyShadows.Find(handle) == m_DirtyShadows.InvalidIndex());
	m_DirtyShadows.Insert(handle);
	if (bForce)
		m_Shadows[handle].m_LastAngles.Init(FLT_MAX, FLT_MAX, FLT_MAX);
	IClientRenderable* pParent = GetParentShadowEntity(handle);
	if (pParent)
		AddToDirtyShadowList(pParent, bForce);
}

void CClientShadowMgr::AddToDirtyShadowList(IClientRenderable* pRenderable, bool bForce)
{
	if (m_bUpdatingDirtyShadows)
		return;
	if (pRenderable->IsShadowDirty())
		return;
	ClientShadowHandle_t handle = pRenderable->GetShadowHandle();
	if (handle == CLIENTSHADOW_INVALID_HANDLE)
		return;
#ifdef _DEBUG
	{
		IClientRenderable* pShadowRenderable = ClientEntityList().GetClientRenderableFromHandle(m_Shadows[handle].m_Entity);
		Assert(pRenderable == pShadowRenderable);
	}
#endif
	pRenderable->MarkShadowDirty(true);
	AddToDirtyShadowList(handle, bForce);
}

void CClientShadowMgr::MarkRenderToTextureShadowDirty(ClientShadowHandle_t handle)
{
	if (handle != CLIENTSHADOW_INVALID_HANDLE)
	{
		ClientShadow_t& shadow = m_Shadows[handle];
		shadow.m_Flags |= SHADOW_FLAGS_TEXTURE_DIRTY;
		IClientRenderable* pParent = GetParentShadowEntity(handle);
		if (pParent)
		{
			ClientShadowHandle_t parentHandle = pParent->GetShadowHandle();
			if (parentHandle != CLIENTSHADOW_INVALID_HANDLE)
				m_Shadows[parentHandle].m_Flags |= SHADOW_FLAGS_TEXTURE_DIRTY;
		}
	}
}

void CClientShadowMgr::UpdateShadow(ClientShadowHandle_t handle, bool force)
{
	ClientShadow_t& shadow = m_Shadows[handle];
	IClientRenderable* pRenderable = ClientEntityList().GetClientRenderableFromHandle(shadow.m_Entity);
	if (!pRenderable)
	{
		DestroyShadow(handle);
		return;
	}
	if (!pRenderable->GetModel())
	{
		pRenderable->MarkShadowDirty(false);
		return;
	}
	const ShadowInfo_t& shadowInfo = shadowmgr->GetInfo(shadow.m_ShadowHandle);
	if (shadowInfo.m_FalloffBias == 255)
	{
		shadowmgr->EnableShadow(shadow.m_ShadowHandle, false);
		m_TransparentShadows.AddToTail(handle);
		return;
	}
#ifdef _DEBUG
	// Debug break if desired.
#endif
	if (ShouldUseParentShadow(pRenderable) || WillParentRenderBlobbyShadow(pRenderable))
	{
		shadowmgr->EnableShadow(shadow.m_ShadowHandle, false);
		pRenderable->MarkShadowDirty(false);
		return;
	}
	shadowmgr->EnableShadow(shadow.m_ShadowHandle, true);
	const Vector& origin = pRenderable->GetRenderOrigin();
	const QAngle& angles = pRenderable->GetRenderAngles();
	if (force || (origin != shadow.m_LastOrigin) || (angles != shadow.m_LastAngles))
	{
		VectorCopy(origin, shadow.m_LastOrigin);
		VectorCopy(angles, shadow.m_LastAngles);
		CMatRenderContextPtr pRenderContext(materials);
		const model_t* pModel = pRenderable->GetModel();
		MaterialFogMode_t fogMode = pRenderContext->GetFogMode();
		pRenderContext->FogMode(MATERIAL_FOG_NONE);
		switch (modelinfo->GetModelType(pModel))
		{
		case mod_brush:
			UpdateBrushShadow(pRenderable, handle);
			break;
		case mod_studio:
			UpdateStudioShadow(pRenderable, handle);
			break;
		default:
			Assert(0);
			break;
		}
		pRenderContext->FogMode(fogMode);
	}
	pRenderable->MarkShadowDirty(false);
}

void CClientShadowMgr::UpdateProjectedTextureInternal(ClientShadowHandle_t handle, bool force)
{
	ClientShadow_t& shadow = m_Shadows[handle];
	if (shadow.m_Flags & SHADOW_FLAGS_FLASHLIGHT)
	{
		VPROF_BUDGET("CClientShadowMgr::UpdateProjectedTextureInternal", VPROF_BUDGETGROUP_SHADOW_DEPTH_TEXTURING);
		Assert((shadow.m_Flags & SHADOW_FLAGS_SHADOW) == 0);
		shadowmgr->EnableShadow(shadow.m_ShadowHandle, true);
		UpdateBrushShadow(NULL, handle);
	}
	else
	{
		Assert(shadow.m_Flags & SHADOW_FLAGS_SHADOW);
		Assert((shadow.m_Flags & SHADOW_FLAGS_FLASHLIGHT) == 0);
		UpdateShadow(handle, force);
	}
}

void CClientShadowMgr::UpdateProjectedTexture(ClientShadowHandle_t handle, bool force)
{
	VPROF_BUDGET("CClientShadowMgr::UpdateProjectedTexture", VPROF_BUDGETGROUP_SHADOW_DEPTH_TEXTURING);
	if (handle == CLIENTSHADOW_INVALID_HANDLE)
		return;
	ClientShadow_t& shadow = m_Shadows[handle];
	if ((shadow.m_Flags & SHADOW_FLAGS_FLASHLIGHT) == 0)
	{
		Warning("CClientShadowMgr::UpdateProjectedTexture can only be used with flashlights!\n");
		return;
	}
	UpdateProjectedTextureInternal(handle, force);
	RemoveShadowFromDirtyList(handle);
}

void CClientShadowMgr::ComputeBoundingSphere(IClientRenderable* pRenderable, Vector& origin, float& radius)
{
	Assert(pRenderable);
	Vector mins, maxs;
	pRenderable->GetShadowRenderBounds(mins, maxs, GetActualShadowCastType(pRenderable));
	Vector size;
	VectorSubtract(maxs, mins, size);
	radius = size.Length() * 0.5f;
	Vector centroid;
	VectorAdd(mins, maxs, centroid);
	centroid *= 0.5f;
	Vector vec[3];
	AngleVectors(pRenderable->GetRenderAngles(), &vec[0], &vec[1], &vec[2]);
	vec[1] *= -1.0f;
	VectorCopy(pRenderable->GetRenderOrigin(), origin);
	VectorMA(origin, centroid.x, vec[0], origin);
	VectorMA(origin, centroid.y, vec[1], origin);
	VectorMA(origin, centroid.z, vec[2], origin);
}

void CClientShadowMgr::ComputeShadowBBox(IClientRenderable* pRenderable, const Vector& vecAbsCenter, float flRadius, Vector* pAbsMins, Vector* pAbsMaxs)
{
	for (int i = 0; i < 3; ++i)
	{
		float flShadowCastDistance = GetShadowDistance(pRenderable);
		float flDist = flShadowCastDistance * GetShadowDirection(pRenderable)[i];
		if (GetShadowDirection(pRenderable)[i] < 0)
		{
			(*pAbsMins)[i] = vecAbsCenter[i] - flRadius + flDist;
			(*pAbsMaxs)[i] = vecAbsCenter[i] + flRadius;
		}
		else
		{
			(*pAbsMins)[i] = vecAbsCenter[i] - flRadius;
			(*pAbsMaxs)[i] = vecAbsCenter[i] + flRadius + flDist;
		}
	}
}

class CShadowLeafEnum : public ISpatialLeafEnumerator
{
public:
	bool EnumerateLeaf(int leaf, intp context)
	{
		m_LeafList.AddToTail(leaf);
		return true;
	}
	CUtlVectorFixedGrowable<int, 512> m_LeafList;
};

static void BuildShadowLeafList(CShadowLeafEnum* pEnum, const Vector& origin, const Vector& dir, const Vector2D& size, float maxDist)
{
	Ray_t ray;
	VectorCopy(origin, ray.m_Start);
	VectorMultiply(dir, maxDist, ray.m_Delta);
	ray.m_StartOffset.Init(0, 0, 0);
	float flRadius = sqrt(size.x * size.x + size.y * size.y) * 0.5f;
	ray.m_Extents.Init(flRadius, flRadius, flRadius);
	ray.m_IsRay = false;
	ray.m_IsSwept = true;
	ISpatialQuery* pQuery = engine->GetBSPTreeQuery();
	pQuery->EnumerateLeavesAlongRay(ray, pEnum, 0);
}

void CClientShadowMgr::BuildOrthoShadow(IClientRenderable* pRenderable, ClientShadowHandle_t handle, const Vector& mins, const Vector& maxs)
{
	Vector vec[3];
	AngleVectors(pRenderable->GetRenderAngles(), &vec[0], &vec[1], &vec[2]);
	vec[1] *= -1.0f;
	Vector vecShadowDir = GetShadowDirection(pRenderable);
	Vector localShadowDir;
	localShadowDir[0] = DotProduct(vec[0], vecShadowDir);
	localShadowDir[1] = DotProduct(vec[1], vecShadowDir);
	localShadowDir[2] = DotProduct(vec[2], vecShadowDir);
	int vecIdx[3];
	SortAbsVectorComponents(localShadowDir, vecIdx);
	Vector xvec = vec[vecIdx[0]];
	Vector yvec = vec[vecIdx[1]];
	xvec -= vecShadowDir * DotProduct(vecShadowDir, xvec);
	yvec -= vecShadowDir * DotProduct(vecShadowDir, yvec);
	VectorNormalize(xvec);
	VectorNormalize(yvec);
	Vector boxSize;
	VectorSubtract(maxs, mins, boxSize);
	Vector2D size2D(boxSize[vecIdx[0]], boxSize[vecIdx[1]]);
	size2D.x *= fabs(DotProduct(vec[vecIdx[0]], xvec));
	size2D.y *= fabs(DotProduct(vec[vecIdx[1]], yvec));
	size2D.x += boxSize[vecIdx[2]] * fabs(DotProduct(vec[vecIdx[2]], xvec));
	size2D.y += boxSize[vecIdx[2]] * fabs(DotProduct(vec[vecIdx[2]], yvec));
	size2D.x += 10.0f;
	size2D.y += 10.0f;
	Vector2DMax(size2D, Vector2D(10.0f, 10.0f), size2D);
	Vector org;
	float falloffStart = ComputeLocalShadowOrigin(pRenderable, mins, maxs, localShadowDir, 2.0f, org);
	Vector worldOrigin = pRenderable->GetRenderOrigin();
	VectorMA(worldOrigin, org.x, vec[0], worldOrigin);
	VectorMA(worldOrigin, org.y, vec[1], worldOrigin);
	VectorMA(worldOrigin, org.z, vec[2], worldOrigin);
	float dx = 1.0f / TEXEL_SIZE_PER_CASTER_SIZE;
	worldOrigin.x = (int)(worldOrigin.x / dx) * dx;
	worldOrigin.y = (int)(worldOrigin.y / dx) * dx;
	worldOrigin.z = (int)(worldOrigin.z / dx) * dx;
	VMatrix matWorldToShadow, matWorldToTexture;
	BuildGeneralWorldToShadowMatrix(m_Shadows[handle].m_WorldToShadow, worldOrigin, vecShadowDir, xvec, yvec);
	BuildWorldToTextureMatrix(m_Shadows[handle].m_WorldToShadow, size2D, matWorldToTexture);
	Vector2DCopy(size2D, m_Shadows[handle].m_WorldSize);
	float flShadowCastDistance = GetShadowDistance(pRenderable);
	float maxHeight = flShadowCastDistance + falloffStart;
	CShadowLeafEnum leafList;
	BuildShadowLeafList(&leafList, worldOrigin, vecShadowDir, size2D, maxHeight);
	int nCount = leafList.m_LeafList.Count();
	const int* pLeafList = leafList.m_LeafList.Base();
	shadowmgr->ProjectShadow(m_Shadows[handle].m_ShadowHandle, worldOrigin, vecShadowDir, matWorldToTexture, size2D, nCount, pLeafList, maxHeight, falloffStart, MAX_FALLOFF_AMOUNT, pRenderable->GetRenderOrigin());
	ClientLeafSystem()->ProjectShadow(m_Shadows[handle].m_ClientLeafShadowHandle, nCount, pLeafList);
}

static void LineDrawHelper(const Vector& startShadowSpace, const Vector& endShadowSpace, const VMatrix& shadowToWorld, unsigned char r = 255, unsigned char g = 255, unsigned char b = 255)
{
	Vector startWorldSpace, endWorldSpace;
	Vector3DMultiplyPositionProjective(shadowToWorld, startShadowSpace, startWorldSpace);
	Vector3DMultiplyPositionProjective(shadowToWorld, endShadowSpace, endWorldSpace);
	if (debugoverlay)
	{
		debugoverlay->AddLineOverlay(startWorldSpace + Vector(0, 0, 1), endWorldSpace + Vector(0, 0, 1), r, g, b, false, -1);
	}
}

static void DebugDrawFrustum(const Vector& vOrigin, const VMatrix& matWorldToFlashlight)
{
	VMatrix flashlightToWorld;
	MatrixInverseGeneral(matWorldToFlashlight, flashlightToWorld);
	LineDrawHelper(Vector(0, 0, 0), Vector(0, 0, 1), flashlightToWorld, 255, 255, 255);
	LineDrawHelper(Vector(0, 0, 1), Vector(0, 1, 1), flashlightToWorld, 255, 255, 255);
	LineDrawHelper(Vector(0, 1, 1), Vector(0, 1, 0), flashlightToWorld, 255, 255, 255);
	LineDrawHelper(Vector(0, 1, 0), Vector(0, 0, 0), flashlightToWorld, 255, 255, 255);
	LineDrawHelper(Vector(1, 0, 0), Vector(1, 0, 1), flashlightToWorld, 255, 255, 255);
	LineDrawHelper(Vector(1, 0, 1), Vector(1, 1, 1), flashlightToWorld, 255, 255, 255);
	LineDrawHelper(Vector(1, 1, 1), Vector(1, 1, 0), flashlightToWorld, 255, 255, 255);
	LineDrawHelper(Vector(1, 1, 0), Vector(1, 0, 0), flashlightToWorld, 255, 255, 255);
	LineDrawHelper(Vector(0, 0, 0), Vector(1, 0, 0), flashlightToWorld, 255, 255, 255);
	LineDrawHelper(Vector(0, 0, 1), Vector(1, 0, 1), flashlightToWorld, 255, 255, 255);
	LineDrawHelper(Vector(0, 1, 1), Vector(1, 1, 1), flashlightToWorld, 255, 255, 255);
	LineDrawHelper(Vector(0, 1, 0), Vector(1, 1, 0), flashlightToWorld, 255, 255, 255);
	LineDrawHelper(Vector(0.5f, 0.5f, 0), Vector(1, 0.5f, 0), flashlightToWorld, 255, 0, 0);
	LineDrawHelper(Vector(0.5f, 0.5f, 0), Vector(0.5f, 1, 0), flashlightToWorld, 0, 255, 0);
	LineDrawHelper(Vector(0.5f, 0.5f, 0), Vector(0.5f, 0.5f, 0.35f), flashlightToWorld, 0, 0, 255);
}

static void BuildFlashlightLeafList(CShadowLeafEnum* pEnum, const VMatrix& worldToShadow)
{
	Vector mins, maxs;
	CalculateAABBFromProjectionMatrix(worldToShadow, &mins, &maxs);
	ISpatialQuery* pQuery = engine->GetBSPTreeQuery();
	pQuery->EnumerateLeavesInBox(mins, maxs, pEnum, 0);
}

void CClientShadowMgr::BuildFlashlight(ClientShadowHandle_t handle)
{
	ClientShadow_t& shadow = m_Shadows[handle];
	if (IsX360() || r_flashlight_version2.GetInt())
	{
		shadowmgr->ProjectFlashlight(shadow.m_ShadowHandle, shadow.m_WorldToShadow, 0, NULL);
		return;
	}
	VPROF_BUDGET("CClientShadowMgr::BuildFlashlight", VPROF_BUDGETGROUP_SHADOW_DEPTH_TEXTURING);
	bool bLightModels = r_flashlightmodels.GetBool();
	bool bLightSpecificEntity = (shadow.m_hTargetEntity.Get() != NULL);
	bool bLightWorld = (shadow.m_Flags & SHADOW_FLAGS_LIGHT_WORLD) != 0;
	int nCount = 0;
	const int* pLeafList = 0;
	CShadowLeafEnum leafList;
	if (bLightWorld || (bLightModels && !bLightSpecificEntity))
	{
		BuildFlashlightLeafList(&leafList, shadow.m_WorldToShadow);
		nCount = leafList.m_LeafList.Count();
		pLeafList = leafList.m_LeafList.Base();
	}
	if (bLightWorld)
		shadowmgr->ProjectFlashlight(shadow.m_ShadowHandle, shadow.m_WorldToShadow, nCount, pLeafList);
	else
	{
		shadowmgr->EnableShadow(shadow.m_ShadowHandle, false);
		shadowmgr->EnableShadow(shadow.m_ShadowHandle, true);
	}
	if (!bLightModels)
		return;
	if (!bLightSpecificEntity)
	{
		ClientLeafSystem()->ProjectFlashlight(shadow.m_ClientLeafShadowHandle, nCount, pLeafList);
		return;
	}
	Assert(shadow.m_hTargetEntity->GetModel());
	C_BaseEntity* pChild = shadow.m_hTargetEntity->FirstMoveChild();
	while (pChild)
	{
		int modelType = modelinfo->GetModelType(pChild->GetModel());
		if (modelType == mod_brush)
			AddShadowToReceiver(handle, pChild, SHADOW_RECEIVER_BRUSH_MODEL);
		else if (modelType == mod_studio)
			AddShadowToReceiver(handle, pChild, SHADOW_RECEIVER_STUDIO_MODEL);
		pChild = pChild->NextMovePeer();
	}
	int modelType = modelinfo->GetModelType(shadow.m_hTargetEntity->GetModel());
	if (modelType == mod_brush)
		AddShadowToReceiver(handle, shadow.m_hTargetEntity, SHADOW_RECEIVER_BRUSH_MODEL);
	else if (modelType == mod_studio)
		AddShadowToReceiver(handle, shadow.m_hTargetEntity, SHADOW_RECEIVER_STUDIO_MODEL);
}

void CClientShadowMgr::AddChildBounds(matrix3x4_t& matWorldToBBox, IClientRenderable* pParent, Vector& vecMins, Vector& vecMaxs)
{
	Vector vecChildMins, vecChildMaxs, vecNewChildMins, vecNewChildMaxs;
	matrix3x4_t childToBBox;
	IClientRenderable* pChild = pParent->FirstShadowChild();
	while (pChild)
	{
		if (GetActualShadowCastType(pChild) != SHADOWS_NONE)
		{
			pChild->GetShadowRenderBounds(vecChildMins, vecChildMaxs, SHADOWS_RENDER_TO_TEXTURE);
			ConcatTransforms(matWorldToBBox, pChild->RenderableToWorldTransform(), childToBBox);
			TransformAABB(childToBBox, vecChildMins, vecChildMaxs, vecNewChildMins, vecNewChildMaxs);
			VectorMin(vecMins, vecNewChildMins, vecMins);
			VectorMax(vecMaxs, vecNewChildMaxs, vecMaxs);
		}
		AddChildBounds(matWorldToBBox, pChild, vecMins, vecMaxs);
		pChild = pChild->NextShadowPeer();
	}
}

void CClientShadowMgr::ComputeHierarchicalBounds(IClientRenderable* pRenderable, Vector& vecMins, Vector& vecMaxs)
{
	ShadowType_t shadowType = GetActualShadowCastType(pRenderable);
	pRenderable->GetShadowRenderBounds(vecMins, vecMaxs, shadowType);
	if (IsPC())
	{
		IClientRenderable* pChild = pRenderable->FirstShadowChild();
		if (pChild && shadowType != SHADOWS_SIMPLE)
		{
			matrix3x4_t matWorldToBBox;
			MatrixInvert(pRenderable->RenderableToWorldTransform(), matWorldToBBox);
			AddChildBounds(matWorldToBBox, pRenderable, vecMins, vecMaxs);
		}
	}
}

void CClientShadowMgr::UpdateStudioShadow(IClientRenderable* pRenderable, ClientShadowHandle_t handle)
{
	if (!(m_Shadows[handle].m_Flags & SHADOW_FLAGS_FLASHLIGHT))
	{
		Vector mins, maxs;
		ComputeHierarchicalBounds(pRenderable, mins, maxs);
		ShadowType_t shadowType = GetActualShadowCastType(handle);
		if (shadowType != SHADOWS_RENDER_TO_TEXTURE)
			BuildOrthoShadow(pRenderable, handle, mins, maxs);
		else
			BuildRenderToTextureShadow(pRenderable, handle, mins, maxs);
	}
	else
		BuildFlashlight(handle);
}

void CClientShadowMgr::UpdateBrushShadow(IClientRenderable* pRenderable, ClientShadowHandle_t handle)
{
	if (!(m_Shadows[handle].m_Flags & SHADOW_FLAGS_FLASHLIGHT))
	{
		Vector mins, maxs;
		ComputeHierarchicalBounds(pRenderable, mins, maxs);
		ShadowType_t shadowType = GetActualShadowCastType(handle);
		if (shadowType != SHADOWS_RENDER_TO_TEXTURE)
			BuildOrthoShadow(pRenderable, handle, mins, maxs);
		else
			BuildRenderToTextureShadow(pRenderable, handle, mins, maxs);
	}
	else
	{
		VPROF_BUDGET("CClientShadowMgr::UpdateBrushShadow", VPROF_BUDGETGROUP_SHADOW_DEPTH_TEXTURING);
		BuildFlashlight(handle);
	}
}

#ifdef _DEBUG
static bool s_bBreak = false;
void ShadowBreak_f() { s_bBreak = true; }
static ConCommand r_shadowbreak("r_shadowbreak", ShadowBreak_f);
#endif

bool CClientShadowMgr::WillParentRenderBlobbyShadow(IClientRenderable* pRenderable)
{
	if (!pRenderable)
		return false;
	IClientRenderable* pShadowParent = pRenderable->GetShadowParent();
	if (!pShadowParent)
		return false;
	ShadowType_t shadowType = GetActualShadowCastType(pShadowParent);
	if (shadowType == SHADOWS_NONE)
		return WillParentRenderBlobbyShadow(pShadowParent);
	return (shadowType == SHADOWS_SIMPLE);
}

bool CClientShadowMgr::ShouldUseParentShadow(IClientRenderable* pRenderable)
{
	if (!pRenderable)
		return false;
	IClientRenderable* pShadowParent = pRenderable->GetShadowParent();
	if (!pShadowParent)
		return false;
	ShadowType_t shadowType = GetActualShadowCastType(pShadowParent);
	if (shadowType == SHADOWS_SIMPLE)
		return false;
	if (shadowType == SHADOWS_NONE)
		return ShouldUseParentShadow(pShadowParent);
	return true;
}

void CClientShadowMgr::PreRender()
{
	VPROF_BUDGET("CClientShadowMgr::PreRender", VPROF_BUDGETGROUP_SHADOW_RENDERING);
	MDLCACHE_CRITICAL_SECTION();

	{
		VPROF_BUDGET("CClientShadowMgr::PreRender DepthTextures", VPROF_BUDGETGROUP_SHADOW_DEPTH_TEXTURING);
		if (r_flashlightdepthtexture.GetBool() && !materials->SupportsShadowDepthTextures())
		{
			r_flashlightdepthtexture.SetValue(0);
			ShutdownDepthTextureShadows();
		}
		bool bDepthTextureActive = r_flashlightdepthtexture.GetBool();
		int nDepthTextureResolution = r_flashlightdepthres.GetInt();
		if ((bDepthTextureActive != m_bDepthTextureActive) || (nDepthTextureResolution != m_nDepthTextureResolution))
		{
			if (bDepthTextureActive && m_bDepthTextureActive && (nDepthTextureResolution != m_nDepthTextureResolution))
			{
				ShutdownDepthTextureShadows();
				InitDepthTextureShadows();
			}
			else if (m_bDepthTextureActive && !bDepthTextureActive)
			{
				ShutdownDepthTextureShadows();
			}
			else if (bDepthTextureActive && !m_bDepthTextureActive)
			{
				InitDepthTextureShadows();
			}
		}
	}
	bool bRenderToTextureActive = r_shadowrendertotexture.GetBool();
	if (bRenderToTextureActive != m_RenderToTextureActive)
	{
		if (m_RenderToTextureActive)
			ShutdownRenderToTextureShadows();
		else
			InitRenderToTextureShadows();
		UpdateAllShadows();
		return;
	}
	m_bUpdatingDirtyShadows = true;
	for (unsigned short i = m_DirtyShadows.FirstInorder(); i != m_DirtyShadows.InvalidIndex(); i = m_DirtyShadows.NextInorder(i))
	{
		UpdateProjectedTextureInternal(i, false);
	}
	m_DirtyShadows.RemoveAll();
	int nCount = m_TransparentShadows.Count();
	for (unsigned short i = 0; i < nCount; ++i)
		m_DirtyShadows.Insert(m_TransparentShadows[i]);
	m_TransparentShadows.RemoveAll();
	m_bUpdatingDirtyShadows = false;
}

IClientRenderable* CClientShadowMgr::GetParentShadowEntity(ClientShadowHandle_t handle)
{
	ClientShadow_t& shadow = m_Shadows[handle];
	IClientRenderable* pRenderable = ClientEntityList().GetClientRenderableFromHandle(shadow.m_Entity);
	if (pRenderable)
	{
		if (ShouldUseParentShadow(pRenderable))
		{
			IClientRenderable* pParent = pRenderable->GetShadowParent();
			while (GetActualShadowCastType(pParent) == SHADOWS_NONE)
			{
				pParent = pParent->GetShadowParent();
				Assert(pParent);
			}
			return pParent;
		}
	}
	return NULL;
}

void CClientShadowMgr::AdvanceFrame()
{
	m_ShadowAllocator.AdvanceFrame();
}
int CClientShadowMgr::BuildActiveShadowDepthList(const CViewSetup& viewSetup, int nMaxDepthShadows, ClientShadowHandle_t* pActiveDepthShadows)
{
	int nActiveDepthShadowCount = 0;
	for (ClientShadowHandle_t i = m_Shadows.Head(); i != m_Shadows.InvalidIndex(); i = m_Shadows.Next(i))
	{
		ClientShadow_t& shadow = m_Shadows[i];
		if (!(shadow.m_Flags & SHADOW_FLAGS_USE_DEPTH_TEXTURE))
			continue;
		const FlashlightState_t& flashlightState = shadowmgr->GetFlashlightState(shadow.m_ShadowHandle);
		if (!flashlightState.m_bEnableShadows)
			continue;
		Vector vecAbsMins, vecAbsMaxs;
		CalculateAABBFromProjectionMatrix(shadow.m_WorldToShadow, &vecAbsMins, &vecAbsMaxs);
		Frustum_t viewFrustum;
		GeneratePerspectiveFrustum(viewSetup.origin, viewSetup.angles, viewSetup.zNear, viewSetup.zFar, viewSetup.fov, viewSetup.m_flAspectRatio, viewFrustum);
		if (R_CullBox(vecAbsMins, vecAbsMaxs, viewFrustum))
		{
			shadowmgr->SetFlashlightDepthTexture(shadow.m_ShadowHandle, NULL, 0);
			continue;
		}
		if (nActiveDepthShadowCount >= nMaxDepthShadows)
		{
			static bool s_bOverflowWarning = false;
			if (!s_bOverflowWarning)
			{
				Warning("Too many depth textures rendered in a single view!\n");
				Assert(0);
				s_bOverflowWarning = true;
			}
			shadowmgr->SetFlashlightDepthTexture(shadow.m_ShadowHandle, NULL, 0);
			continue;
		}
		pActiveDepthShadows[nActiveDepthShadowCount++] = i;
	}
	return nActiveDepthShadowCount;
}

void CClientShadowMgr::SetViewFlashlightState(int nActiveFlashlightCount, ClientShadowHandle_t* pActiveFlashlights)
{
	if (!IsX360() && !r_flashlight_version2.GetInt())
		return;
	Assert(nActiveFlashlightCount <= 1);
	if (nActiveFlashlightCount > 0)
	{
		Assert((m_Shadows[pActiveFlashlights[0]].m_Flags & SHADOW_FLAGS_FLASHLIGHT) != 0);
		shadowmgr->SetFlashlightRenderState(pActiveFlashlights[0]);
	}
	else
		shadowmgr->SetFlashlightRenderState(SHADOW_HANDLE_INVALID);
}

void CClientShadowMgr::ComputeShadowDepthTextures(const CViewSetup& viewSetup)
{
	VPROF_BUDGET("CClientShadowMgr::ComputeShadowDepthTextures", VPROF_BUDGETGROUP_SHADOW_DEPTH_TEXTURING);
	CMatRenderContextPtr pRenderContext(materials);
	PIXEVENT(pRenderContext, "Shadow Depth Textures");
	ClientShadowHandle_t pActiveDepthShadows[1024];
	int nActiveDepthShadowCount = BuildActiveShadowDepthList(viewSetup, ARRAYSIZE(pActiveDepthShadows), pActiveDepthShadows);
	for (int j = 0; j < nActiveDepthShadowCount; ++j)
	{
		ClientShadow_t& shadow = m_Shadows[pActiveDepthShadows[j]];
		CTextureReference shadowDepthTexture;
		bool bGotShadowDepthTexture = LockShadowDepthTexture(&shadowDepthTexture);
		if (!bGotShadowDepthTexture)
		{
			static int mapCount = 0;
			if (mapCount < 10)
			{
				Warning("Too many shadow maps this frame!\n");
				mapCount++;
			}
			Assert(0);
			shadowmgr->SetFlashlightDepthTexture(shadow.m_ShadowHandle, NULL, 0);
			continue;
		}
		CViewSetup shadowView;
		shadowView.m_flAspectRatio = 1.0f;
		shadowView.x = shadowView.y = 0;
		shadowView.width = shadowDepthTexture->GetActualWidth();
		shadowView.height = shadowDepthTexture->GetActualHeight();
		shadowView.m_bOrtho = false;
		shadowView.m_bDoBloomAndToneMapping = false;
		const FlashlightState_t& flashlightState = shadowmgr->GetFlashlightState(shadow.m_ShadowHandle);
		shadowView.fov = shadowView.fovViewmodel = flashlightState.m_fHorizontalFOVDegrees;
		shadowView.origin = flashlightState.m_vecLightOrigin;
		QuaternionAngles(flashlightState.m_quatOrientation, shadowView.angles);
		shadowView.zNear = shadowView.zNearViewmodel = flashlightState.m_NearZ;
		shadowView.zFar = shadowView.zFarViewmodel = flashlightState.m_FarZ;
		if (r_flashlightdrawfrustum.GetBool() || flashlightState.m_bDrawShadowFrustum)
			DebugDrawFrustum(shadowView.origin, shadow.m_WorldToShadow);
		CMatRenderContextPtr pRenderContextMat(materials);
		pRenderContextMat->SetShadowDepthBiasFactors(flashlightState.m_flShadowSlopeScaleDepthBias, flashlightState.m_flShadowDepthBias);
		view->UpdateShadowDepthTexture(m_DummyColorTexture, shadowDepthTexture, shadowView);
		shadowmgr->SetFlashlightDepthTexture(shadow.m_ShadowHandle, shadowDepthTexture, 0);
	}
	SetViewFlashlightState(nActiveDepthShadowCount, pActiveDepthShadows);
}

static void SetupBonesOnBaseAnimating(C_BaseAnimating*& pAnim)
{
	if (pAnim)
	{
		pAnim->SetupBones(NULL, -1, -1, gpGlobals->curtime);
	}
}


void CClientShadowMgr::ComputeShadowTextures(const CViewSetup& viewShadow, int leafCount, LeafIndex_t* pLeafList)
{
	VPROF_BUDGET("CClientShadowMgr::ComputeShadowTextures", VPROF_BUDGETGROUP_SHADOW_RENDERING);
	if (!m_RenderToTextureActive || (r_shadows.GetInt() == 0) || r_shadows_gamecontrol.GetInt() == 0)
		return;
	m_bThreaded = false; // Can be set based on r_threaded_client_shadow_manager and thread pool availability.
	MDLCACHE_CRITICAL_SECTION();
	int nCount = s_VisibleShadowList.FindShadows(&viewShadow, leafCount, pLeafList);
	if (nCount == 0)
		return;
	CMatRenderContextPtr pRenderContext(materials);
	PIXEVENT(pRenderContext, "Render-To-Texture Shadows");
	pRenderContext->ClearColor4ub(255, 255, 255, 0);
	MaterialHeightClipMode_t oldHeightClipMode = pRenderContext->GetHeightClipMode();
	pRenderContext->SetHeightClipMode(MATERIAL_HEIGHTCLIPMODE_DISABLE);
	pRenderContext->MatrixMode(MATERIAL_PROJECTION);
	pRenderContext->PushMatrix();
	pRenderContext->LoadIdentity();
	pRenderContext->Scale(1, -1, 1);
	pRenderContext->Ortho(0, 0, 1, 1, -9999, 0);
	pRenderContext->MatrixMode(MATERIAL_VIEW);
	pRenderContext->PushMatrix();
	pRenderContext->PushRenderTargetAndViewport(m_ShadowAllocator.GetTexture());
	if (!IsX360() && m_bRenderTargetNeedsClear)
	{
		pRenderContext->ClearBuffers(true, false);
		m_bRenderTargetNeedsClear = false;
	}
	int nMaxShadows = r_shadowmaxrendered.GetInt();
	int nModelsRendered = 0;
	if (m_bThreaded && g_pThreadPool->NumIdleThreads())
	{
		s_NPCShadowBoneSetups.RemoveAll();
		s_NonNPCShadowBoneSetups.RemoveAll();
		for (int i = 0; i < nCount; ++i)
		{
			const VisibleShadowInfo_t& info = s_VisibleShadowList.GetVisibleShadow(i);
			if (nModelsRendered < nMaxShadows)
			{
				if (BuildSetupListForRenderToTextureShadow(info.m_hShadow, info.m_flArea))
					++nModelsRendered;
			}
		}
		ParallelProcess("NPCShadowBoneSetups", s_NPCShadowBoneSetups.Base(), s_NPCShadowBoneSetups.Count(), SetupBonesOnBaseAnimating);
		ParallelProcess("NonNPCShadowBoneSetups", s_NonNPCShadowBoneSetups.Base(), s_NonNPCShadowBoneSetups.Count(), SetupBonesOnBaseAnimating);
		nModelsRendered = 0;
	}
	for (int i = 0; i < nCount; ++i)
	{
		const VisibleShadowInfo_t& info = s_VisibleShadowList.GetVisibleShadow(i);
		if (nModelsRendered < nMaxShadows)
		{
			if (DrawRenderToTextureShadow(info.m_hShadow, info.m_flArea))
				++nModelsRendered;
		}
		else
			DrawRenderToTextureShadowLOD(info.m_hShadow);
	}
	pRenderContext->PopRenderTargetAndViewport();
	pRenderContext->MatrixMode(MATERIAL_PROJECTION);
	pRenderContext->PopMatrix();
	pRenderContext->MatrixMode(MATERIAL_VIEW);
	pRenderContext->PopMatrix();
	pRenderContext->SetHeightClipMode(oldHeightClipMode);
	pRenderContext->ClearColor3ub(0, 0, 0);
}

bool CClientShadowMgr::LockShadowDepthTexture(CTextureReference* shadowDepthTexture)
{
	for (int i = 0; i < m_DepthTextureCache.Count(); i++)
	{
		if (m_DepthTextureCacheLocks[i] == false)
		{
			*shadowDepthTexture = m_DepthTextureCache[i];
			m_DepthTextureCacheLocks[i] = true;
			return true;
		}
	}
	return false;
}

void CClientShadowMgr::UnlockAllShadowDepthTextures()
{
	for (int i = 0; i < m_DepthTextureCache.Count(); i++)
		m_DepthTextureCacheLocks[i] = false;
	SetViewFlashlightState(0, NULL);
}

void CClientShadowMgr::SetFlashlightTarget(ClientShadowHandle_t shadowHandle, EHANDLE targetEntity)
{
	Assert(m_Shadows.IsValidIndex(shadowHandle));
	ClientShadow_t& shadow = m_Shadows[shadowHandle];
	if (!(shadow.m_Flags & SHADOW_FLAGS_FLASHLIGHT))
		return;
	shadow.m_hTargetEntity = targetEntity;
}

void CClientShadowMgr::SetFlashlightLightWorld(ClientShadowHandle_t shadowHandle, bool bLightWorld)
{
	Assert(m_Shadows.IsValidIndex(shadowHandle));
	ClientShadow_t& shadow = m_Shadows[shadowHandle];
	if (!(shadow.m_Flags & SHADOW_FLAGS_FLASHLIGHT))
		return;
	if (bLightWorld)
		shadow.m_Flags |= SHADOW_FLAGS_LIGHT_WORLD;
	else
		shadow.m_Flags &= ~SHADOW_FLAGS_LIGHT_WORLD;
}

bool CClientShadowMgr::IsFlashlightTarget(ClientShadowHandle_t shadowHandle, IClientRenderable* pRenderable)
{
	ClientShadow_t& shadow = m_Shadows[shadowHandle];
	if (shadow.m_hTargetEntity->GetClientRenderable() == pRenderable)
		return true;
	C_BaseEntity* pChild = shadow.m_hTargetEntity->FirstMoveChild();
	while (pChild)
	{
		if (pChild->GetClientRenderable() == pRenderable)
			return true;
		pChild = pChild->NextMovePeer();
	}
	return false;
}

//-----------------------------------------------------------------------------
// Material proxy implementations for shadow textures.
//-----------------------------------------------------------------------------
class CShadowProxy : public IMaterialProxy
{
public:
	CShadowProxy() : m_BaseTextureVar(NULL) {}
	virtual ~CShadowProxy() {}
	virtual bool Init(IMaterial* pMaterial, KeyValues* pKeyValues) override
	{
		bool foundVar;
		m_BaseTextureVar = pMaterial->FindVar("$basetexture", &foundVar, false);
		return foundVar;
	}
	virtual void OnBind(void* pProxyData) override
	{
		unsigned short clientShadowHandle = (unsigned short)((intp)pProxyData & 0xffff);
		ITexture* pTex = s_ClientShadowMgr.GetShadowTexture(clientShadowHandle);
		m_BaseTextureVar->SetTextureValue(pTex);
		if (ToolsEnabled())
			ToolFramework_RecordMaterialParams(GetMaterial());
	}
	virtual void Release(void) override { delete this; }
	virtual IMaterial* GetMaterial() override { return m_BaseTextureVar->GetOwningMaterial(); }
private:
	IMaterialVar* m_BaseTextureVar;
};

EXPOSE_INTERFACE(CShadowProxy, IMaterialProxy, "Shadow" IMATERIAL_PROXY_INTERFACE_VERSION);

class CShadowModelProxy : public IMaterialProxy
{
public:
	CShadowModelProxy() : m_BaseTextureVar(NULL), m_BaseTextureOffsetVar(NULL), m_BaseTextureScaleVar(NULL), m_BaseTextureMatrixVar(NULL), m_FalloffOffsetVar(NULL), m_FalloffDistanceVar(NULL), m_FalloffAmountVar(NULL) {}
	virtual ~CShadowModelProxy() {}
	virtual bool Init(IMaterial* pMaterial, KeyValues* pKeyValues) override
	{
		bool foundVar;
		m_BaseTextureVar = pMaterial->FindVar("$basetexture", &foundVar, false);
		if (!foundVar)
			return false;
		m_BaseTextureOffsetVar = pMaterial->FindVar("$basetextureoffset", &foundVar, false);
		if (!foundVar)
			return false;
		m_BaseTextureScaleVar = pMaterial->FindVar("$basetexturescale", &foundVar, false);
		if (!foundVar)
			return false;
		m_BaseTextureMatrixVar = pMaterial->FindVar("$basetexturetransform", &foundVar, false);
		if (!foundVar)
			return false;
		m_FalloffOffsetVar = pMaterial->FindVar("$falloffoffset", &foundVar, false);
		if (!foundVar)
			return false;
		m_FalloffDistanceVar = pMaterial->FindVar("$falloffdistance", &foundVar, false);
		if (!foundVar)
			return false;
		m_FalloffAmountVar = pMaterial->FindVar("$falloffamount", &foundVar, false);
		return foundVar;
	}
	virtual void OnBind(void* pProxyData) override
	{
		unsigned short clientShadowHandle = (unsigned short)((intp)pProxyData & 0xffff);
		ITexture* pTex = s_ClientShadowMgr.GetShadowTexture(clientShadowHandle);
		m_BaseTextureVar->SetTextureValue(pTex);
		const ShadowInfo_t& info = s_ClientShadowMgr.GetShadowInfo(clientShadowHandle);
		m_BaseTextureMatrixVar->SetMatrixValue(info.m_WorldToShadow);
		m_BaseTextureOffsetVar->SetVecValue(info.m_TexOrigin.Base(), 2);
		m_BaseTextureScaleVar->SetVecValue(info.m_TexSize.Base(), 2);
		m_FalloffOffsetVar->SetFloatValue(info.m_FalloffOffset);
		m_FalloffDistanceVar->SetFloatValue(info.m_MaxDist);
		m_FalloffAmountVar->SetFloatValue(info.m_FalloffAmount);
		if (ToolsEnabled())
			ToolFramework_RecordMaterialParams(GetMaterial());
	}
	virtual void Release(void) override { delete this; }
	virtual IMaterial* GetMaterial() override { return m_BaseTextureVar->GetOwningMaterial(); }
private:
	IMaterialVar* m_BaseTextureVar;
	IMaterialVar* m_BaseTextureOffsetVar;
	IMaterialVar* m_BaseTextureScaleVar;
	IMaterialVar* m_BaseTextureMatrixVar;
	IMaterialVar* m_FalloffOffsetVar;
	IMaterialVar* m_FalloffDistanceVar;
	IMaterialVar* m_FalloffAmountVar;
};

EXPOSE_INTERFACE(CShadowModelProxy, IMaterialProxy, "ShadowModel" IMATERIAL_PROXY_INTERFACE_VERSION);

// End of refactored code.
