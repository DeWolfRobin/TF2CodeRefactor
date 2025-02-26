//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#include "rangecheckedvar.h"

bool g_bDoRangeChecks = true;


static int g_nDisables = 0;


CDisableRangeChecks::CDisableRangeChecks()
{
	if ( !ThreadInMainThread() )
		return;
	g_nDisables++;
	g_bDoRangeChecks = false;
}


CDisableRangeChecks::~CDisableRangeChecks()
{
	if ( !ThreadInMainThread() )
		return;
	Assert( g_nDisables > 0 );
	--g_nDisables;
	if ( g_nDisables == 0 )
	{
		g_bDoRangeChecks = true;
	}
}




