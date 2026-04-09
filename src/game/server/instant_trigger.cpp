//========= Copyright Felis, Licensed under 0BSD. =============================//
//
// Purpose: Alternative to Source's triggers, mainly used by MapHack
//
//=============================================================================//

#include "cbase.h"
#include "filters.h"
#include "nmrih_maphack_manager.h"
#include "nmrih_player.h"
#include "instant_trigger.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
ConVar show_instant_triggers( "show_instant_triggers", "0", FCVAR_CHEAT );

//-----------------------------------------------------------------------------
BEGIN_DATADESC( CInstantTrigger )
	DEFINE_KEYFIELD( m_bDisabled, FIELD_BOOLEAN, "StartDisabled" ),
	DEFINE_KEYFIELD( m_iszFilterName, FIELD_STRING, "filtername" ),
	DEFINE_FIELD( m_hFilter, FIELD_EHANDLE ),

	DEFINE_KEYFIELD( m_bAllowPlayers, FIELD_BOOLEAN, "allowPlayers" ),
	DEFINE_KEYFIELD( m_bAllowNPCs, FIELD_BOOLEAN, "allowNPCs" ),
	DEFINE_KEYFIELD( m_bAllowPhysics, FIELD_BOOLEAN, "allowPhysics" ),
	DEFINE_KEYFIELD( m_bAllowAll, FIELD_BOOLEAN, "allowAll" ),

	DEFINE_KEYFIELD( m_iszMapHackEvent, FIELD_STRING, "event" ),
	DEFINE_KEYFIELD( m_flRadius, FIELD_FLOAT, "radius" ),
	DEFINE_KEYFIELD( m_bNoClear, FIELD_BOOLEAN, "noclear" ),

	DEFINE_INPUTFUNC( FIELD_VOID, "Enable", InputEnable ),
	DEFINE_INPUTFUNC( FIELD_VOID, "Disable", InputDisable ),
	DEFINE_INPUTFUNC( FIELD_VOID, "Toggle", InputToggle ),

	DEFINE_OUTPUT( m_OnTrigger, "OnTrigger" )
END_DATADESC()

//-----------------------------------------------------------------------------
LINK_ENTITY_TO_CLASS( instant_trigger, CInstantTrigger );

//-----------------------------------------------------------------------------
CInstantTrigger::CInstantTrigger()
{
	m_bDisabled = false;

	m_iszFilterName = NULL_STRING;

	m_bAllowPlayers = false;
	m_bAllowNPCs = false;
	m_bAllowPhysics = false;
	m_bAllowAll = false;

	m_iszMapHackEvent = NULL_STRING;
	m_flRadius = 0.0f;
	m_bNoClear = false;

	m_bActive = false;
}

//-----------------------------------------------------------------------------
void CInstantTrigger::Spawn()
{
	SetSolid( SOLID_NONE );
	SetMoveType( MOVETYPE_NONE );

	// Don't become active if we're off by default
	SetActive( !m_bDisabled );
}

//-----------------------------------------------------------------------------
void CInstantTrigger::Activate()
{
	// Get a handle to my filter entity if there is one
	if ( m_iszFilterName != NULL_STRING )
	{
		m_hFilter = dynamic_cast<CBaseFilter *>( gEntList.FindEntityByName( NULL, m_iszFilterName ) );
	}

	BaseClass::Activate();
}

//-----------------------------------------------------------------------------
void CInstantTrigger::TriggerThink()
{
	const Vector &vecSrc = GetAbsOrigin();

	if ( show_instant_triggers.GetBool() )
	{
		NDebugOverlay::Sphere( vecSrc, vec3_angle, m_flRadius, 0, 255, 0, 0, false, 0.15f );
	}

	CBaseEntity *pEnt;
	for ( CEntitySphereQuery sphere( vecSrc, m_flRadius ); ( pEnt = sphere.GetCurrentEntity() ) != NULL; sphere.NextEntity() )
	{
		if ( !CanTrigger( pEnt ) )
			continue;

		CBaseFilter *pFilter = m_hFilter.Get();
		const bool bPassesFilter = ( !pFilter ) ? true : pFilter->PassesFilter( this, pEnt );

		if ( bPassesFilter )
		{
			m_OnTrigger.FireOutput( pEnt, this );

			// Trigger MapHack event
			const char *pszEventName = STRING( m_iszMapHackEvent );
			if ( GetMapHackManager()->HasMapHack() && pszEventName[0] != '\0' )
			{
				GetMapHackManager()->TriggerEventByName( pszEventName );
			}

			// Kill trigger on use
			if ( !m_bNoClear )
			{
				UTIL_Remove( this );
				return;
			}
		}
	}

	SetNextThink( gpGlobals->curtime + 0.1f );
}

//------------------------------------------------------------------------------
bool CInstantTrigger::CanTrigger( CBaseEntity *pEnt ) const
{
	// Players
	if ( ( m_bAllowPlayers || m_bAllowAll ) && ( pEnt->GetFlags() & FL_CLIENT ) )
	{
		CNMRiH_Player *pPlayer = ToNMRiHPlayer( pEnt );
		return ( pPlayer && !pPlayer->IsObserver() && pPlayer->IsAlive() ); // Ignore observers
	}

	// NPCs
	if ( ( m_bAllowNPCs || m_bAllowAll ) && ( pEnt->GetFlags() & FL_NPC ) )
		return true;

	// Physics
	if ( ( m_bAllowPhysics || m_bAllowAll ) && pEnt->GetMoveType() == MOVETYPE_VPHYSICS )
		return true;

	// Everything else
	if ( m_bAllowAll )
		return true;

	return false;
}

//------------------------------------------------------------------------------
void CInstantTrigger::InputEnable( inputdata_t &inputdata )
{
	SetActive( true );
}

//------------------------------------------------------------------------------
void CInstantTrigger::InputDisable( inputdata_t &inputdata )
{
	SetActive( false );
}

//------------------------------------------------------------------------------
void CInstantTrigger::InputToggle( inputdata_t &inputdata )
{
	SetActive( !m_bActive );
}

//------------------------------------------------------------------------------
void CInstantTrigger::SetActive( const bool bActive )
{
	// Toggles think func for entity query
	if ( bActive )
	{
		SetThink( &CInstantTrigger::TriggerThink );
		SetNextThink( gpGlobals->curtime + 0.1f );
	}
	else
	{
		SetThink( NULL );
		SetNextThink( TICK_NEVER_THINK );
	}

	m_bActive = bActive;

	// Backward compatibility
	m_bDisabled = !bActive;
}
