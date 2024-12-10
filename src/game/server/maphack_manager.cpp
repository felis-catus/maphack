//========= Copyright Felis, All rights reserved. =============================//
//
// Purpose: "Map hacks" are text files used for adding or modifying entities
//			in the map. Good for modifying existing maps without having the VMF.
//
//			While engine already has the .lmp files for basically achieving the
//			same, map hacks aim to be more dynamic by running on every frame.
//			This allows for runtime manipulation of the entities, see "events".
//
//=============================================================================//

#include "cbase.h"
#include "maphack_manager.h"
#include "filesystem.h"
#include "engine/IEngineSound.h"
#include "tier1/utlbuffer.h"
#include "mapentities.h"
#include "vprof.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
static CMapHackManager s_MapHackManager;
CMapHackManager *const g_pMapHackManager = &s_MapHackManager;

//-----------------------------------------------------------------------------
#define MAPHACK_ENTDATA_BLOCK_PADDING 1024
#define MAPHACK_ENTITIES_MAX_RECURSION_LEVEL 64

#ifndef CON_COLOR_MAPHACK
#define CON_COLOR_MAPHACK Color( 166, 84, 184, 255 )
#endif

//-----------------------------------------------------------------------------
static int g_iMapHackEntitiesRecursionLevel = 0;

//-----------------------------------------------------------------------------
static CUtlVector<MapHackOutputCallback_t> g_vecOutputCallbacks;

//-----------------------------------------------------------------------------
CON_COMMAND( maphack_load, "Load maphack file by name." )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() )
		return;

	if ( args.ArgC() < 2 )
	{
		Msg( "Usage: maphack_load <filename>\n" );
		return;
	}

	GetMapHackManager()->LoadMapHackFromFile( args[1], MAPHACK_LOAD_POST_ENTITY );
}

//-----------------------------------------------------------------------------
CON_COMMAND( maphack_include, "Include file by name into existing maphack." )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() )
		return;

	if ( args.ArgC() < 2 )
	{
		Msg( "Usage: maphack_include <filename>\n" );
		return;
	}

	if ( !GetMapHackManager()->HasMapHack() )
	{
		Warning( "No maphack loaded, use \"maphack_load\" instead.\n" );
		return;
	}

	int loadFlags = MAPHACK_LOAD_POST_ENTITY;

	// Mark it as include
	loadFlags |= MAPHACK_INCLUDE;

	GetMapHackManager()->LoadMapHackFromFile( args[1], loadFlags );
}

//-----------------------------------------------------------------------------
CON_COMMAND( maphack_reload, "Reload current maphack." )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() )
		return;

	GetMapHackManager()->ReloadMapHack();
}

//-----------------------------------------------------------------------------
CON_COMMAND( maphack_trigger, "Trigger a MapHack event." )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() )
		return;

	if ( args.ArgC() < 2 )
	{
		Msg( "Usage: maphack_trigger <event label>\n" );
		return;
	}

	GetMapHackManager()->TriggerEventByName( args[1] );
}

//-----------------------------------------------------------------------------
CON_COMMAND( maphack_dump_vars, "Dump MapHack variables to console." )
{
	if ( !UTIL_IsCommandIssuedByServerAdmin() )
		return;

	GetMapHackManager()->DumpVariablesToConsole();
}

//-----------------------------------------------------------------------------
void Fn_SV_MapHackChanged( IConVar *pConVar, const char *pszOldValue, float flOldValue );
ConVar sv_maphack( "sv_maphack", "1", FCVAR_NOTIFY | FCVAR_REPLICATED, "Enable MapHack system. Maphacks are text files for adding and modifying entities in the map.", Fn_SV_MapHackChanged );
ConVar sv_maphack_filename( "sv_maphack_filename", "", FCVAR_NOTIFY | FCVAR_REPLICATED, "If not empty, load this file as maphack.", Fn_SV_MapHackChanged );
ConVar sv_maphack_directory( "sv_maphack_directory", "maps/maphacks", FCVAR_REPLICATED, "The game will search this directory for [mapname].txt files." );
ConVar sv_maphack_allow_servercommand( "sv_maphack_allow_servercommand", "0", FCVAR_REPLICATED, "Allow $console function to execute server commands." );
ConVar sv_maphack_debug( "sv_maphack_debug", "0", FCVAR_GAMEDLL, "Print MapHack behavior to the server console." );

//-----------------------------------------------------------------------------
void Fn_SV_MapHackChanged( IConVar *pConVar, const char *pszOldValue, float flOldValue )
{
	// Replicated cvar will execute this callback on clients...
	// Check for gamerules object to test if the server is active
	if ( !g_pGameRules )
		return;

	const ConVarRef var( pConVar );

	GetMapHackManager()->ResetMapHack();

	if ( var.GetBool() )
	{
		const char *pszFileName = sv_maphack_filename.GetString();
		if ( pszFileName[0] == '\0' )
			pszFileName = UTIL_VarArgs( "%s\\%s.txt", sv_maphack_directory.GetString(), STRING( gpGlobals->mapname ) );

		GetMapHackManager()->LoadMapHackFromFile( pszFileName, MAPHACK_LOAD_POST_ENTITY );
	}
}

//-----------------------------------------------------------------------------
void Fn_EntityOutputCallback( CBaseEntity *pEntity, const char *pszName, const MapHackOutputCallbackParams_t &params )
{
	GetMapHackManager()->OnEntityOutputFired( pEntity, pszName, params );
}

//-----------------------------------------------------------------------------
void MapHack_DebugMsg( const char *pszMsg, ... )
{
	if ( !sv_maphack_debug.GetBool() )
		return;

	static char	szBuffer[1024];
	char szFormattedMessage[512];

	va_list	argPtr;
	va_start( argPtr, pszMsg );
	V_vsnprintf( szFormattedMessage, sizeof( szFormattedMessage ), pszMsg, argPtr );
	va_end( argPtr );

	V_strcpy_safe( szBuffer, "MapHack: " );
	V_strcat_safe( szBuffer, szFormattedMessage );
	ConColorMsg( CON_COLOR_MAPHACK, "%s", szBuffer );
}

//-----------------------------------------------------------------------------
const char *MapHack_VariableValueHelper( const char *pszValue, MapHackType_t *pType /* = NULL */ )
{
	if ( pszValue && pszValue[0] == '%' )
	{
		const char *pszVar = pszValue + 1;

		const MapHackVariable_t *pVar = GetMapHackManager()->GetVariableByName( pszVar );
		if ( !pVar )
		{
			Warning( "MapHack WARNING: Variable \"%s\" does not exist!\n", pszVar );
		}
		else
		{
			if ( pType )
				*pType = pVar->m_Type;

			return pVar->GetValue();
		}
	}

	return pszValue;
}

//-----------------------------------------------------------------------------
// For parsing entity KeyValues
//-----------------------------------------------------------------------------
void MapHack_ParseEntKVBlockHelper( CBaseEntity *pEntity, KeyValues *pNode )
{
	KeyValues *pNodeData = pNode->GetFirstSubKey();
	while ( pNodeData )
	{
		if ( FStrEq( pNodeData->GetName(), "keyvalues" ) )
		{
			pNodeData = pNodeData->GetNextKey();
			continue;
		}

		// Handle the connections block
		if ( FStrEq( pNodeData->GetName(), "connections" ) )
		{
			MapHack_ParseEntKVBlockHelper( pEntity, pNodeData );
		}
		else
		{
			const char *pszName = pNodeData->GetName();
			const char *pszValue = MapHack_VariableValueHelper( pNodeData->GetString() );

			// Handle special cases
			if ( FStrEq( pszName, "model" ) )
			{
				// Precache model and set it
				CBaseEntity::PrecacheModel( pszValue );
				pEntity->SetModel( pszValue );
			}

			pEntity->KeyValue( pNodeData->GetName(), pszValue );

			MapHack_DebugMsg( "Changed keyvalue \"%s\" to \"%s\" (targetname: %s)\n",
				pNodeData->GetName(), pszValue, pEntity->GetDebugName() );
		}

		pNodeData = pNodeData->GetNextKey();
	}
}

//-----------------------------------------------------------------------------
// Entdata version
//-----------------------------------------------------------------------------
void MapHack_ParseEntDataBlockHelper( MapHackEntityData_t *pEntData, KeyValues *pNode )
{
	int currentKeyInstance = 0;
	const char *pszPreviousKeyName = "";

	KeyValues *pNodeData = pNode->GetFirstSubKey();
	while ( pNodeData )
	{
		if ( FStrEq( pNodeData->GetName(), "keyvalues" ) )
		{
			pNodeData = pNodeData->GetNextKey();
			continue;
		}

		// Handle the connections block
		if ( FStrEq( pNodeData->GetName(), "connections" ) )
		{
			MapHack_ParseEntDataBlockHelper( pEntData, pNodeData );
		}
		else
		{
			const char *pszKeyName = pNodeData->GetName();

			// Move to next instance if this is a duplicate key
			if ( FStrEq( pszKeyName, pszPreviousKeyName ) )
			{
				++currentKeyInstance;
			}
			else
			{
				currentKeyInstance = 0;
			}

			pszPreviousKeyName = pszKeyName;

			// Handle variables
			const char *pszValue = MapHack_VariableValueHelper( pNodeData->GetString() );

			char szBuffer[256];
			V_strcpy_safe( szBuffer, pszValue );
			pEntData->SetKeyValue( pszKeyName, szBuffer, currentKeyInstance );

			MapHack_DebugMsg( "(Pre-entity) Changed keyvalue \"%s\" to \"%s\"\n", pszKeyName, szBuffer );
		}

		pNodeData = pNodeData->GetNextKey();
	}
}

//-----------------------------------------------------------------------------
// Finds a named offset in datamap
//-----------------------------------------------------------------------------
unsigned int MapHack_FindInDataMap( const datamap_t *pMap, const char *pszName, fieldtype_t *pReturnType = NULL )
{
	while ( pMap )
	{
		for ( int i = 0; i < pMap->dataNumFields; ++i )
		{
			if ( !pMap->dataDesc[i].fieldName )
				continue;

			// Case-insensitive
			if ( V_stricmp( pszName, pMap->dataDesc[i].fieldName ) == 0 )
			{
				if ( pReturnType )
					*pReturnType = pMap->dataDesc[i].fieldType;

				return pMap->dataDesc[i].fieldOffset[TD_OFFSET_NORMAL];
			}

			if ( pMap->dataDesc[i].td )
			{
				unsigned int offset;
				if ( ( offset = MapHack_FindInDataMap( pMap->dataDesc[i].td, pszName, pReturnType ) ) != 0 )
					return offset;
			}
		}

		pMap = pMap->baseMap;
	}

	return 0;
}

//-----------------------------------------------------------------------------
const char *MapHack_GetLabel( const char *psz, int *pDataType = NULL )
{
	const char *pszChr = strchr( psz, ':' );
	if ( pszChr != NULL )
	{
		// Hop over ':'
		++pszChr;

		// Set data type
		if ( pDataType )
		{
			if ( FStrEq( pszChr, "entities" ) )
				*pDataType = 0;
			else if ( FStrEq( pszChr, "precache" ) )
				*pDataType = 1;
		}

		return pszChr;
	}

	return psz;
}

//-----------------------------------------------------------------------------
bool MapHack_IsKeyWord( const char *psz )
{
	for ( unsigned int i = 0; i < ARRAYSIZE( g_pszMapHackKeyWords ); ++i )
	{
		if ( FStrEq( psz, g_pszMapHackKeyWords[i] ) )
			return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
void MapHack_EditEntity( CBaseEntity *pEntity, KeyValues *pKV )
{
	if ( pEntity && pKV )
	{
		MapHack_ParseEntKVBlockHelper( pEntity, pKV );

		KeyValues *pEntKeyValuesVal = pKV->GetFirstValue();
		while ( pEntKeyValuesVal )
		{
			const char *pszValueName = pEntKeyValuesVal->GetName();

			if ( FStrEq( pszValueName, "model" ) )
			{
				// Model is a special case, precache and set it
				const char *pszModelName = pEntKeyValuesVal->GetString();
				CBaseEntity::PrecacheModel( pszModelName );

				pEntity->SetModel( pszModelName );
			}

			pEntKeyValuesVal = pEntKeyValuesVal->GetNextValue();
		}
	}
}

//-----------------------------------------------------------------------------
bool MapHack_EditEntityField( CBaseEntity *pEntity, const char *pszKeyName, const char *pszFieldName, const char *pszValue )
{
	// No key names? No field names? No editing.
	if ( !pszKeyName && !pszFieldName )
		return false;

	// Find a named offset in datamap
	fieldtype_t fieldType;
	const unsigned int fieldOffset = MapHack_FindInDataMap( pEntity->GetDataDescMap(), pszFieldName, &fieldType );
	if ( fieldOffset == 0 )
		return false;

	switch ( fieldType )
	{
		// Strings
		case FIELD_MODELNAME:
		case FIELD_SOUNDNAME:
		case FIELD_STRING:
			( *(string_t *)( (char *)pEntity + fieldOffset ) ) = AllocPooledString( pszValue );
			break;

		// Floats
		case FIELD_TIME:
		case FIELD_FLOAT:
			( *(float *)( (char *)pEntity + fieldOffset ) ) = V_atof( pszValue );
			break;

		// Boolean
		case FIELD_BOOLEAN:
			( *(bool *)( (char *)pEntity + fieldOffset ) ) = ( V_atoi( pszValue ) != 0 );
			break;

		// Char
		case FIELD_CHARACTER:
			( *( (char *)pEntity + fieldOffset ) ) = (char)V_atoi( pszValue );
			break;

		// Short
		case FIELD_SHORT:
			( *(short *)( (char *)pEntity + fieldOffset ) ) = (short)V_atoi( pszValue );
			break;

		// Integers
		case FIELD_INTEGER:
		case FIELD_TICK:
			( *(int *)( (char *)pEntity + fieldOffset ) ) = V_atoi( pszValue );
			break;

		// Vectors
		case FIELD_POSITION_VECTOR:
		case FIELD_VECTOR:
			UTIL_StringToVector( (float *)( (char *)pEntity + fieldOffset ), pszValue );
			break;

		// Matrices
		case FIELD_VMATRIX:
		case FIELD_VMATRIX_WORLDSPACE:
			UTIL_StringToFloatArray( (float *)( (char *)pEntity + fieldOffset ), 16, pszValue );
			break;

		case FIELD_MATRIX3X4_WORLDSPACE:
			UTIL_StringToFloatArray( (float *)( (char *)pEntity + fieldOffset ), 12, pszValue );
			break;

		// Colors
		case FIELD_COLOR32:
			UTIL_StringToColor32( (color32 *)( (char *)pEntity + fieldOffset ), pszValue );
			break;

		// Ignore these
		case FIELD_INTERVAL:
		case FIELD_CLASSPTR:
		case FIELD_MODELINDEX:
		case FIELD_MATERIALINDEX:
		case FIELD_EDICT:
		default:
			ConColorMsg( 0, CON_COLOR_MAPHACK, "MapHack WARNING: Field type %d unsupported! (field name: \"%s\")\n", fieldType, pszFieldName );
			return false;
	}

	MapHack_DebugMsg( "Changed field \"%s\" value to \"%s\"\n", pszFieldName, pszValue );
	return true;
}

//-----------------------------------------------------------------------------
bool MapHack_RemoveEntityConnections( CBaseEntity *pEntity )
{
	if ( !pEntity )
		return false;

	// Iterate through a typedescript data block
	for ( const datamap_t *pDataMap = pEntity->GetDataDescMap(); pDataMap != NULL; pDataMap = pDataMap->baseMap )
	{
		for ( int i = 0; i < pDataMap->dataNumFields; ++i )
		{
			if ( pDataMap->dataDesc[i].fieldType == FIELD_CUSTOM &&
				( pDataMap->dataDesc[i].flags & ( FTYPEDESC_OUTPUT | FTYPEDESC_KEY ) ) )
			{
				CBaseEntityOutput *pOutput = (CBaseEntityOutput *)( (char *)pEntity + pDataMap->dataDesc[i].fieldOffset[0] );

				// Remove all connections
				pOutput->DeleteAllElements();
			}
		}
	}

	return true;
}

//-----------------------------------------------------------------------------
MapHackType_t MapHack_GetTypeByIdentifier( const char *pszIdent )
{
	if ( !pszIdent )
		return MapHackType_t::TYPE_NONE;

	if ( FStrEq( pszIdent, "int" ) )
	{
		return MapHackType_t::TYPE_INT;
	}

	if ( FStrEq( pszIdent, "float" ) )
	{
		return MapHackType_t::TYPE_FLOAT;
	}

	if ( FStrEq( pszIdent, "string" ) )
	{
		return MapHackType_t::TYPE_STRING;
	}

	if ( FStrEq( pszIdent, "color" ) )
	{
		return MapHackType_t::TYPE_COLOR;
	}

	return MapHackType_t::TYPE_NONE;
}

//-----------------------------------------------------------------------------
inline bool MapHack_IsSafeEntity( CBaseEntity *pEntity )
{
	if ( !pEntity ||
		pEntity->IsPlayer() ||
		pEntity->ClassMatches( FindPooledString( "worldspawn" ) ) )
	{
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
class CMapHackSystemHook : public CAutoGameSystemPerFrame
{
public:
	CMapHackSystemHook( const char *name ) : CAutoGameSystemPerFrame( name ) {}

	bool Init() OVERRIDE
	{
		return s_MapHackManager.Init();
	}

	void Shutdown() OVERRIDE
	{
		s_MapHackManager.Shutdown();
	}

	void FrameUpdatePostEntityThink() OVERRIDE
	{
		s_MapHackManager.Think();
	}

	void LevelInitPostEntity() OVERRIDE
	{
		s_MapHackManager.LevelInitPostEntity();
	}

	void LevelShutdownPostEntity() OVERRIDE
	{
		s_MapHackManager.LevelShutdownPostEntity();
	}
};

static CMapHackSystemHook g_MapHackSystemHook( "CMapHackSystemHook" );

//-----------------------------------------------------------------------------
CMapHackManager::CMapHackManager()
{
	NOTE_UNUSED( g_MapHackSystemHook );

	m_pMapHack = NULL;
	m_bPreEntity = true;
	m_pNewMapData = NULL;
	m_pszIdentifier = "";
}

//-----------------------------------------------------------------------------
CMapHackManager::~CMapHackManager()
{
	if ( m_pMapHack )
	{
		m_pMapHack->deleteThis();
		m_pMapHack = NULL;
	}

	delete[] m_pNewMapData;
}

//-----------------------------------------------------------------------------
bool CMapHackManager::Init()
{
	// Register functions
	for ( int type = 0; type < MAPHACK_FUNCTION_COUNT; ++type )
		m_dictFunctions.Insert( g_pszMapHackFunctionTable[type], (MapHackFunctionType_t)type );

	return true;
}

//-----------------------------------------------------------------------------
void CMapHackManager::Shutdown()
{
	ResetMapHack();

	m_dictFunctions.Purge();
	m_vecEntData.PurgeAndDeleteElements();
}

//-----------------------------------------------------------------------------
const char *CMapHackManager::LevelInit( const char *pMapData )
{
	if ( m_pNewMapData )
	{
		delete[] m_pNewMapData;
		m_pNewMapData = NULL;
	}

	m_bPreEntity = true;

	// Load maphack into memory before entities settle in
	if ( sv_maphack.GetBool() )
	{
		// Look for custom filename
		const char *pszFileName = sv_maphack_filename.GetString();
		if ( pszFileName[0] == '\0' )
			pszFileName = UTIL_VarArgs( "%s\\%s.txt", sv_maphack_directory.GetString(), STRING( gpGlobals->mapname ) );

		const int loadFlags = MAPHACK_PRECACHE | MAPHACK_REGISTER_VARS | MAPHACK_LOAD_INCLUDES;
		LoadMapHackFromFile( pszFileName, loadFlags );
	}

	// Do pre-entity stuff if we got a maphack in memory
	if ( m_pMapHack )
	{
		KeyValues *pKVPreEntities = m_pMapHack->FindKey( "pre_entities" );
		if ( pKVPreEntities )
		{
			// Parse map data
			BuildEntityList( pMapData );

			// Run pre-entity field
			RunEntities( pKVPreEntities );

			// Now turn this monster of a hacked entdata into a string
			FinalizeEntData();

			// Clean up the mess
			m_vecEntData.PurgeAndDeleteElements();
		}
	}

	// Return new data
	return m_pNewMapData;
}

//-----------------------------------------------------------------------------
void CMapHackManager::LevelInitPostEntity()
{
	m_bPreEntity = false;

	if ( sv_maphack.GetBool() && m_pMapHack )
	{
		// If we got a maphack in memory, run entities
		RunEntities( m_pMapHack->FindKey( "entities" ) );
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::LevelShutdownPostEntity()
{
	ResetMapHack();
}

//-----------------------------------------------------------------------------
void CMapHackManager::Think()
{
	if ( !HasMapHack() )
		return;

	if ( m_dictEvents.Count() > 0 )
		HandleEvents();
}

//-----------------------------------------------------------------------------
void CMapHackManager::FireGameEvent( IGameEvent *event )
{
	// Fire all events that listen to this game event
	FOR_EACH_DICT( m_dictEvents, i )
	{
		MapHackEvent_t *pEvent = m_dictEvents[i];
		if ( !pEvent || pEvent->m_Type != MAPHACK_EVENT_GAMEEVENT )
			continue;

		if ( FStrEq( pEvent->m_szGameEventName, event->GetName() ) )
		{
			TriggerEvent( pEvent );
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::OnEntityOutputFired( const CBaseEntity *pEntity, const char *pszName, const MapHackOutputCallbackParams_t &params )
{
	// Fire all events that listen to this output
	FOR_EACH_DICT( m_dictEvents, i )
	{
		MapHackEvent_t *pEvent = m_dictEvents[i];
		if ( !pEvent || pEvent->m_Type != MAPHACK_EVENT_OUTPUT )
			continue;

		if ( pEntity == pEvent->m_hOutputEnt.Get() && FStrEq( pEvent->m_szOutputName, pszName ) )
		{
			TriggerEvent( pEvent, params.m_flDelay );
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::RegisterOutputCallback( CBaseEntity *pEnt, const FnMapHackOutputCallback_t fn )
{
	if ( !pEnt )
		return;

	bool bListed = false;
	for ( int i = 0; i < g_vecOutputCallbacks.Count(); ++i )
	{
		if ( g_vecOutputCallbacks[i].m_hEntity == pEnt )
		{
			bListed = true;
			break;
		}
	}

	if ( !bListed )
	{
		MapHackOutputCallback_t callback;
		callback.m_hEntity = pEnt;
		callback.m_fnCallback = fn;
		g_vecOutputCallbacks.AddToTail( callback );
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::RemoveOutputCallback( const CBaseEntity *pEnt )
{
	if ( !pEnt )
		return;

	for ( int i = 0; i < g_vecOutputCallbacks.Count(); ++i )
	{
		const CBaseEntity *pEntity = g_vecOutputCallbacks[i].m_hEntity.Get();
		if ( pEntity && pEntity->entindex() == pEnt->entindex() )
		{
			g_vecOutputCallbacks.FastRemove( i );
			break;
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::RemoveAllOutputCallbacks()
{
	g_vecOutputCallbacks.Purge();
}

//-----------------------------------------------------------------------------
// This should be called from CBaseEntityOutput::FireOutput
//-----------------------------------------------------------------------------
void CMapHackManager::InvokeEntityOutputCallbacks( const MapHackOutputCallbackParams_t &params )
{
	if ( g_vecOutputCallbacks.IsEmpty() )
	{
		return;
	}

	for ( int i = 0; i < g_vecOutputCallbacks.Count(); ++i )
	{
		const MapHackOutputCallback_t &callback = g_vecOutputCallbacks[i];

		CBaseEntity *pEnt = callback.m_hEntity.Get();
		if ( !pEnt || pEnt->entindex() != params.m_pCaller->entindex() )
			continue;

		const datamap_t *pDataMap = pEnt->GetDataDescMap();
		while ( pDataMap )
		{
			const int fields = pDataMap->dataNumFields;
			for ( int j = 0; j < fields; ++j )
			{
				const typedescription_t *pDataDesc = &pDataMap->dataDesc[j];
				if ( pDataDesc->fieldType == FIELD_CUSTOM && pDataDesc->flags & FTYPEDESC_OUTPUT )
				{
					const CBaseEntityOutput *pOutput = (CBaseEntityOutput *)( (int)pEnt + (int)pDataDesc->fieldOffset[0] );
					if ( pOutput == params.m_pSource )
					{
						callback.m_fnCallback( pEnt, pDataDesc->externalName, params );
						break;
					}
				}
			}

			pDataMap = pDataMap->baseMap;
		}
	}
}

//-----------------------------------------------------------------------------
bool CMapHackManager::LoadMapHack( KeyValues *pKV, const int loadFlags )
{
	return LoadMapHack( pKV, loadFlags, MAPHACK_DEFAULT_IDENTIFIER );
}

//-----------------------------------------------------------------------------
bool CMapHackManager::LoadMapHack( KeyValues *pKV, const int loadFlags, const char *pszIdentifier )
{
	if ( !pKV )
		return false;

	// Validate name
	if ( !FStrEq( pKV->GetName(), "maphack" ) )
	{
		Warning( "MapHack ERROR: Root key must be named \"maphack\" (case insensitive)\n" );
		return false;
	}

	const bool bInclude = ( loadFlags & MAPHACK_INCLUDE );

	if ( !bInclude )
	{
		ResetMapHack();

		SetIdentifier( pszIdentifier );

		// Store keyvalues in memory
		m_pMapHack = pKV->MakeCopy();
	}

	if ( loadFlags & MAPHACK_LOAD_INCLUDES )
		LoadIncludes( pKV->FindKey( "includes" ), loadFlags );

	if ( loadFlags & MAPHACK_REGISTER_VARS )
		RegisterVariables( pKV->FindKey( "vars" ) );

	if ( loadFlags & MAPHACK_PRECACHE )
		Precache( pKV->FindKey( "precache" ) );

	if ( loadFlags & MAPHACK_REGISTER_EVENTS )
		RegisterEvents( pKV->FindKey( "events" ), pKV );

	if ( loadFlags & MAPHACK_RUN_ENTITIES )
		RunEntities( pKV->FindKey( "entities" ) );

	return true;
}

//-----------------------------------------------------------------------------
bool CMapHackManager::LoadMapHackFromFile( const char *pszFileName, const int loadFlags )
{
	bool bSuccess = false;

	KeyValues *pKV = new KeyValues( "maphack" );

	// KV parser requires that we allow escape characters
	pKV->UsesEscapeSequences( true );

	if ( pKV->LoadFromFile( filesystem, pszFileName ) )
	{
		MapHack_DebugMsg( "Loading from file \"%s\"\n", pszFileName );

		// Parse file
		bSuccess = LoadMapHack( pKV, loadFlags );
	}

	if ( !bSuccess && ( loadFlags & MAPHACK_COMPLAIN ) )
		Warning( "Failed to load MapHack %s!\n", pszFileName );

	pKV->deleteThis();
	return bSuccess;
}

//-----------------------------------------------------------------------------
void CMapHackManager::ReloadMapHack()
{
	if ( !HasMapHack() )
		return;

	ResetMapHack( false );

	LoadIncludes( m_pMapHack->FindKey( "includes" ), MAPHACK_LOAD_POST_ENTITY );
	RegisterVariables( m_pMapHack->FindKey( "vars" ) );
	RegisterEvents( m_pMapHack->FindKey( "events" ), m_pMapHack );
	RunEntities( m_pMapHack->FindKey( "entities" ) );
}

//-----------------------------------------------------------------------------
void CMapHackManager::LoadIncludes( KeyValues *pKV, int loadFlags )
{
	if ( !pKV )
		return;

	KeyValues *pValue = pKV->GetFirstValue();
	while ( pValue )
	{
		const char *pszFilename = pValue->GetString();

		if ( !filesystem->FileExists( pszFilename ) )
		{
			DevWarning( "MapHack WARNING: Missing include file \"%s\"\n", pszFilename );
			pValue = pKV->GetNextValue();
			continue;
		}

		KeyValues *pInclude = new KeyValues( "maphack" );
		if ( pInclude->LoadFromFile( filesystem, pszFilename ) )
		{
			MapHack_DebugMsg( "Including \"%s\"\n", pszFilename );
			loadFlags |= MAPHACK_INCLUDE;
			LoadMapHack( pInclude, loadFlags );
		}

		pInclude->deleteThis();

		pValue = pValue->GetNextValue();
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::RegisterVariables( KeyValues *pKV )
{
	if ( !pKV )
		return;

	KeyValues *pVariable = pKV->GetFirstTrueSubKey();
	while ( pVariable )
	{
		const char *pszName = pVariable->GetName();
		const char *pszType = pVariable->GetString( "type", "int" );
		const char *pszValue = pVariable->GetString( "value", "0" );

		// Create new variable
		MapHackVariable_t *pVar = new MapHackVariable_t();
		V_strcpy_safe( pVar->m_szName, pszName );

		// Figure out the type
		pVar->m_Type = MapHack_GetTypeByIdentifier( pszType );
		if ( pVar->m_Type != MapHackType_t::TYPE_NONE )
		{
			switch ( pVar->m_Type )
			{
				case MapHackType_t::TYPE_INT:
					pVar->m_Type = MapHackType_t::TYPE_INT;
					pVar->SetInt( V_atoi( pszValue ) );
					break;
				case MapHackType_t::TYPE_FLOAT:
					pVar->m_Type = MapHackType_t::TYPE_FLOAT;
					pVar->SetFloat( V_atof( pszValue ) );
					break;
				case MapHackType_t::TYPE_STRING:
					pVar->m_Type = MapHackType_t::TYPE_STRING;
					pVar->SetString( pszValue );
					break;
				case MapHackType_t::TYPE_COLOR:
					pVar->m_Type = MapHackType_t::TYPE_COLOR;

					int clr[3];
					if ( sscanf( pszValue, "%d %d %d", &clr[0], &clr[1], &clr[2] ) == 3 )
						pVar->SetColor( Color( clr[0], clr[1], clr[2] ) );
					break;

				default:
					pVar->SetValue( pszValue );
					break;
			}
		}
		else
		{
			Warning( "MapHack WARNING: Unknown type \"%s\" for variable \"%s\"!\n", pszType, pszValue );

			pVariable = pVariable->GetNextTrueSubKey();
			continue;
		}

		// Insert it
		m_dictVars.Insert( pVar->m_szName, pVar );

		pVariable = pVariable->GetNextTrueSubKey();
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::Precache( KeyValues *pKV )
{
	if ( !pKV )
		return;

	KeyValues *pPrecacheVal = pKV->GetFirstValue();
	while ( pPrecacheVal )
	{
		const char *pszType = pPrecacheVal->GetName();
		const char *pszName = pPrecacheVal->GetString();

		if ( FStrEq( pszType, "model" ) )
		{
			CBaseEntity::PrecacheModel( pszName );
			MapHack_DebugMsg( "Precached model \"%s\"\n", pszName );
		}
		else if ( FStrEq( pszType, "material" ) )
		{
			PrecacheMaterial( pszName );
			MapHack_DebugMsg( "Precached material \"%s\"\n", pszName );
		}
		else if ( FStrEq( pszType, "sound" ) )
		{
			CBaseEntity::PrecacheScriptSound( pszName );
			MapHack_DebugMsg( "Precached sound \"%s\"\n", pszName );
		}
		else if ( FStrEq( pszType, "particle" ) )
		{
			PrecacheParticleSystem( pszName );
			MapHack_DebugMsg( "Precached particle system \"%s\"\n", pszName );
		}
		else if ( FStrEq( pszType, "entity" ) )
		{
			UTIL_PrecacheOther( pszName );
			MapHack_DebugMsg( "Precached entity \"%s\"\n", pszName );
		}

		pPrecacheVal = pPrecacheVal->GetNextValue();
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::RegisterEvents( KeyValues *pKV, KeyValues *pMapHack )
{
	if ( pKV ) // Events field isn't required
	{
		KeyValues *pKVEvent = pKV->GetFirstTrueSubKey();
		while ( pKVEvent )
		{
			const char *pszName = pKVEvent->GetName();

			// Can't have events with keyword names
			if ( MapHack_IsKeyWord( pszName ) )
			{
				Warning( "MapHack WARNING: Can't name an event as a keyword \"%s\"!\n", pszName );
				pKVEvent = pKVEvent->GetNextTrueSubKey();
				continue;
			}

			MapHackEvent_t *pEvent = new MapHackEvent_t();
			V_strcpy_safe( pEvent->m_szName, pszName );
			pEvent->m_Type = GetEventTypeByString( pKVEvent->GetString( "type", "EVENT_TRIGGER" ) );

			switch ( pEvent->m_Type )
			{
				case MAPHACK_EVENT_TRIGGER:
				{
					// Wait for manual trigger
					pEvent->m_bTriggered = true;
					pEvent->m_flTriggerTime = 0.0;

					break;
				}

				case MAPHACK_EVENT_TIMED:
				{
					pEvent->m_flDelayTime = pKVEvent->GetFloat( "delay", 1.0f );
					pEvent->m_bRepeat = pKVEvent->GetBool( "repeat", true );
					pEvent->m_bStopped = pKVEvent->GetBool( "startdisabled", false );

					break;
				}

				case MAPHACK_EVENT_OUTPUT:
				{
					// Find our entity
					CBaseEntity *pEnt = NULL;
					const char *pszTargetName = pKVEvent->GetString( "targetname", NULL );
					if ( pszTargetName )
						pEnt = GetEntityByTargetName( pszTargetName );

					if ( !pEnt )
					{
						// Try classname (someone might want outputs from single ent)
						pEnt = GetFirstEntityByClassName( pKVEvent->GetString( "classname" ) );
					}

					if ( pEnt )
					{
						pEvent->m_hOutputEnt = pEnt;

						// Register output callback for this entity
						RegisterOutputCallback( pEnt, Fn_EntityOutputCallback );
					}

					V_strcpy_safe( pEvent->m_szOutputName, pKVEvent->GetString( "output" ) );

					// If pEnt is NULL, this is not uncommon as desired entities might be spawned later by MapHack
					// Store the targetname and we'll try to grab the entity later on if the handle is invalid
					if ( pszTargetName )
						V_strcpy_safe( pEvent->m_szOutputEntName, pszTargetName );

					break;
				}

				case MAPHACK_EVENT_GAMEEVENT:
				{
					const char *pszEventName = pKVEvent->GetString( "eventname", NULL );
					if ( pszEventName )
					{
						ListenForGameEvent( pszEventName );
						V_strcpy_safe( pEvent->m_szGameEventName, pszEventName );
					}

					break;
				}

				default:
					break;
			}

			MapHack_DebugMsg( "Registered event \"%s\"\n", pEvent->m_szName );
			m_dictEvents.Insert( pEvent->m_szName, pEvent );

			pKVEvent = pKVEvent->GetNextTrueSubKey();
		}
	}

	// Find unregistered events, MapHack allows those
	KeyValues *pSubKey = pMapHack->GetFirstTrueSubKey();
	while ( pSubKey )
	{
		const char *pszName = pSubKey->GetName();
		if ( !MapHack_IsKeyWord( pszName ) )
		{
			const char *pszLabel = MapHack_GetLabel( pSubKey->GetName() );

			if ( pszLabel && !GetEventByName( pszLabel ) )
			{
				// Create new event using default properties (trigger)
				MapHackEvent_t *pEvent = new MapHackEvent_t();
				V_strcpy_safe( pEvent->m_szName, pszLabel );
				pEvent->m_Type = MAPHACK_EVENT_TRIGGER;

				MapHack_DebugMsg( "Registered event \"%s\" (default properties)\n", pEvent->m_szName );
				m_dictEvents.Insert( pEvent->m_szName, pEvent );
			}
		}

		pSubKey = pSubKey->GetNextTrueSubKey();
	}

	// Search for event labels
	FOR_EACH_DICT( m_dictEvents, i )
	{
		MapHackEvent_t *pEvent = m_dictEvents[i];
		if ( !pEvent )
			continue;

		KeyValues *pSub = pMapHack->GetFirstTrueSubKey();
		while ( pSub )
		{
			bool bLabelFound = false;
			const char *pszName = pSub->GetName();

			if ( V_strstr( pszName, pEvent->m_szName ) != NULL )
			{
				int dataType = 0;
				const char *pszLabel = MapHack_GetLabel( pszName, &dataType );
				if ( pszLabel != NULL )
				{
					bLabelFound = true;
					pEvent->m_iDataType = dataType;
				}
				else
				{
					// Allow labels without prefix, script authors might prefer doing it this way
					if ( FStrEq( pEvent->m_szName, pszName ) )
					{
						bLabelFound = true;

						// Only entities field allowed
						pEvent->m_iDataType = 0;
					}
				}
			}

			if ( bLabelFound )
			{
				MapHack_DebugMsg( "Event data set for \"%s\" (type: %d)\n", pEvent->m_szName, pEvent->m_iDataType );
				pEvent->m_pKVData = pSub->MakeCopy();
				break;
			}

			pSub = pSub->GetNextTrueSubKey();
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::RunEntities( KeyValues *pKV )
{
	if ( !pKV )
		return;

	// Safety net in case of infinite recursion by poorly written scripts
	++g_iMapHackEntitiesRecursionLevel;
	if ( g_iMapHackEntitiesRecursionLevel > MAPHACK_ENTITIES_MAX_RECURSION_LEVEL )
	{
		Warning( "MapHack WARNING: Recursion level over the limit, terminating.\n" );
		g_iMapHackEntitiesRecursionLevel = 0;
		return;
	}

	// Do a traverse here, key names are entity classnames
	KeyValues *pKVEnt = pKV->GetFirstTrueSubKey();
	while ( pKVEnt )
	{
		const char *pszName = pKVEnt->GetName();
		const bool bIsFunction = ( pszName[0] == '$' );

		// Pre-entities are always first!
		if ( IsPreEntity() && !bIsFunction )
		{
			// Insert new entity to ent data
			KeyValues *pEntityKeyValues;

			// First version of MapHack required the keyvalues field
			KeyValues *pLegacyKeyValues = pKVEnt->FindKey( "keyvalues" );
			if ( pLegacyKeyValues )
			{
				pEntityKeyValues = pLegacyKeyValues;

				// Set positions
				pEntityKeyValues->SetString( "origin", pKVEnt->GetString( "origin" ) );
				pEntityKeyValues->SetString( "angles", pKVEnt->GetString( "angles" ) );
			}
			else
			{
				pEntityKeyValues = pKVEnt;
			}

			// Set classname
			pEntityKeyValues->SetString( "classname", pszName );

			// Handle connections
			KeyValues *pConnections = pEntityKeyValues->FindKey( "connections" );
			if ( pConnections )
			{
				// Clone keys and remove, we don't want this block in entdata!
				for ( KeyValues *pSub = pConnections->GetFirstValue(); pSub; pSub = pSub->GetNextValue() )
				{
					KeyValues *pNewKey = pEntityKeyValues->CreateNewKey();
					if ( pNewKey )
					{
						pNewKey->SetName( pSub->GetName() );
						pNewKey->SetStringValue( pSub->GetString() );
					}
				}

				pEntityKeyValues->RemoveSubKey( pConnections );
				pConnections->deleteThis();
			}

			// Export keyvalues as text
			CUtlBuffer buf = CUtlBuffer( 0, 0, CUtlBuffer::TEXT_BUFFER );
			pEntityKeyValues->RecursiveSaveToFile( buf, 0, false, true );

			// Rid of the root key name to mimic BSP map lump, complete hack
			const char *pszEntData = strchr( (char *)buf.Base(), '{' ) + 1;

			// List it
			const int entBlockSize = V_strlen( pszEntData ) + MAPHACK_ENTDATA_BLOCK_PADDING;
			MapHackEntityData_t *pEntData = ParseEntityData( pszEntData, entBlockSize );
			if ( pEntData )
			{
				m_vecEntData.AddToTail( pEntData );
			}
		}

		// Look for function keys first, those start with '$'
		if ( bIsFunction )
		{
			const MapHackFunctionType_t type = GetFunctionTypeByString( pszName );
			switch ( type )
			{
				// Variables
				case MAPHACK_FUNCTION_IF:
					KvIfCond( pKVEnt );
					break;
				case MAPHACK_FUNCTION_SET:
					KvSetVariable( pKVEnt );
					break;
				case MAPHACK_FUNCTION_INCREMENT:
					KvIncrement( pKVEnt );
					break;
				case MAPHACK_FUNCTION_DECREMENT:
					KvDecrement( pKVEnt );
					break;
				case MAPHACK_FUNCTION_RAND:
					KvRandVariable( pKVEnt );
					break;

				// Basic functions
				case MAPHACK_FUNCTION_CONSOLE:
					KvConsole( pKVEnt );
					break;
				case MAPHACK_FUNCTION_FIRE:
					KvFireInput( pKVEnt );
					break;
				case MAPHACK_FUNCTION_EDIT:
					KvEdit( pKVEnt );
					break;
				case MAPHACK_FUNCTION_EDIT_ALL:
					KvEditAll( pKVEnt );
					break;
				case MAPHACK_FUNCTION_MODIFY:
					KvModify( pKVEnt );
					break;
				case MAPHACK_FUNCTION_FILTER:
					KvFilter( pKVEnt );
					break;
				case MAPHACK_FUNCTION_TRIGGER:
					KvTriggerEvent( pKVEnt );
					break;
				case MAPHACK_FUNCTION_START:
					KvStartEvent( pKVEnt );
					break;
				case MAPHACK_FUNCTION_STOP:
					KvStopEvent( pKVEnt );
					break;
				case MAPHACK_FUNCTION_RESPAWN:
					KvRespawnEntity( pKVEnt );
					break;
				case MAPHACK_FUNCTION_REMOVE:
					KvRemoveEntity( pKVEnt );
					break;
				case MAPHACK_FUNCTION_REMOVE_ALL:
					KvRemoveAllEntities( pKVEnt );
					break;
				case MAPHACK_FUNCTION_REMOVE_CONNECTIONS:
					KvRemoveConnections( pKVEnt );
					break;

				// Entity positions
				case MAPHACK_FUNCTION_GETPOS:
					KvGetPos( pKVEnt );
					break;
				case MAPHACK_FUNCTION_SETPOS:
					KvSetPos( pKVEnt );
					break;
				case MAPHACK_FUNCTION_GETANG:
					KvGetAng( pKVEnt );
					break;
				case MAPHACK_FUNCTION_SETANG:
					KvSetAng( pKVEnt );
					break;

				// Entity datadesc manipulation
				case MAPHACK_FUNCTION_EDIT_FIELD:
					KvEditField( pKVEnt );
					break;

				// Extra functions
				case MAPHACK_FUNCTION_PLAYSOUND:
					KvPlaySound( pKVEnt );
					break;

				case MAPHACK_FUNCTION_SCRIPT:
#if 0 // If your mod has VScript, toggle this
					KvScript( pKVEnt );
#endif
					break;

				default:
					Warning( "MapHack WARNING: Invalid function key \"%s\"!\n", pszName );
					break;
			}
		}
		else
		{
			if ( !IsPreEntity() )
			{
				// Create new entity
				// If invalid, CreateEntityByName() spews a warning for us
				CBaseEntity *pEntity = CreateEntityByName( pszName );
				if ( pEntity )
				{
					KeyValues *pEntityKeyValues;

					// First version of MapHack required the keyvalues field
					KeyValues *pLegacyKeyValues = pKVEnt->FindKey( "keyvalues" );
					if ( pLegacyKeyValues )
					{
						// Parse the outer values first
						MapHack_ParseEntKVBlockHelper( pEntity, pKVEnt );

						pEntityKeyValues = pLegacyKeyValues;
					}
					else
					{
						pEntityKeyValues = pKVEnt;
					}

					MapHack_ParseEntKVBlockHelper( pEntity, pEntityKeyValues );

					// Spawn!
					DispatchSpawn( pEntity );
					m_dictSpawnedEnts.Insert( STRING( pEntity->GetEntityName() ), pEntity );
					MapHack_DebugMsg( "Spawned entity \"%s\" (targetname: %s)\n", pszName, STRING( pEntity->GetEntityName() ) );
				}
			}
		}

		pKVEnt = pKVEnt->GetNextTrueSubKey();
	}

	if ( !IsPreEntity() )
	{
		// Now that we have run this entities field, update our output events
		FOR_EACH_DICT( m_dictEvents, i )
		{
			MapHackEvent_t *pEvent = m_dictEvents[i];
			if ( !pEvent || pEvent->m_Type != MAPHACK_EVENT_OUTPUT )
				continue;

			// No need if we got a valid handle
			if ( pEvent->m_hOutputEnt.Get() )
				continue;

			CBaseEntity *pEnt = GetEntityByTargetName( pEvent->m_szOutputEntName );
			if ( pEnt )
			{
				pEvent->m_hOutputEnt = pEnt;

				// Register output callback for this entity
				RegisterOutputCallback( pEnt, Fn_EntityOutputCallback );
			}
		}
	}

	g_iMapHackEntitiesRecursionLevel = 0;
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvIfCond( KeyValues *pKV )
{
	const char *pszCond = pKV->GetString( "cond", NULL );
	if ( !pszCond )
	{
		Warning( "MapHack WARNING: $if block has no cond!\n" );
		return;
	}

	// Find entities block
	KeyValues *pEntities = pKV->FindKey( "entities" );
	if ( !pEntities )
	{
		Warning( "MapHack WARNING: $if block has no entities field!\n" );
		return;
	}

	if ( TestIfCondBlock( pszCond ) )
	{
		// Run entities and all function keys in this block
		RunEntities( pEntities );
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvSetVariable( KeyValues *pKV )
{
	const char *pszVar = pKV->GetString( "var", NULL );
	if ( !pszVar )
	{
		Warning( "MapHack WARNING: $set block has no 'var'!\n" );
		return;
	}

	const char *pszValue = MapHack_VariableValueHelper( pKV->GetString( "value", NULL ) );
	if ( !pszValue )
	{
		Warning( "MapHack WARNING: $set block has no 'value'!\n" );
		return;
	}

	MapHackVariable_t *pVar = GetVariableByName( pszVar );
	if ( !pVar )
	{
		Warning( "MapHack WARNING: $set block 'var' value references a non-existent variable! (%s)\n", pszVar );
		return;
	}

	switch ( pVar->m_Type )
	{
		case MapHackType_t::TYPE_INT:
			pVar->SetInt( V_atoi( pszValue ) );
			break;
		case MapHackType_t::TYPE_FLOAT:
			pVar->SetFloat( V_atof( pszValue ) );
			break;
		case MapHackType_t::TYPE_STRING:
			pVar->SetString( pszValue );
			break;
		case MapHackType_t::TYPE_COLOR:
			int clr[3];
			if ( sscanf( pszValue, "%d %d %d", &clr[0], &clr[1], &clr[2] ) == 3 )
				pVar->SetColor( Color( clr[0], clr[1], clr[2] ) );
			break;

		default:
			pVar->SetValue( pszValue );
			break;
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvIncrement( KeyValues *pKV )
{
	const char *pszVar = pKV->GetString( "var", NULL );
	if ( !pszVar )
	{
		Warning( "MapHack WARNING: $increment block has no 'var'!\n" );
		return;
	}

	MapHackVariable_t *pVar = GetVariableByName( pszVar );
	if ( !pVar )
	{
		Warning( "MapHack WARNING: $increment block 'var' value references a non-existent variable! (%s)\n", pszVar );
		return;
	}

	switch ( pVar->m_Type )
	{
		case MapHackType_t::TYPE_INT:
			pVar->SetInt( ( pVar->GetInt() + 1 ) );
			break;
		case MapHackType_t::TYPE_FLOAT:
			pVar->SetFloat( ( pVar->GetFloat() + 1.0f ) );
			break;
		default:
			break;
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvDecrement( KeyValues *pKV )
{
	const char *pszVar = pKV->GetString( "var", NULL );
	if ( !pszVar )
	{
		Warning( "MapHack WARNING: $decrement block has no 'var'!\n" );
		return;
	}

	MapHackVariable_t *pVar = GetVariableByName( pszVar );
	if ( !pVar )
	{
		Warning( "MapHack WARNING: $decrement block 'var' value references a non-existent variable! (%s)\n", pszVar );
		return;
	}

	switch ( pVar->m_Type )
	{
		case MapHackType_t::TYPE_INT:
			pVar->SetInt( ( pVar->GetInt() - 1 ) );
			break;
		case MapHackType_t::TYPE_FLOAT:
			pVar->SetFloat( ( pVar->GetFloat() - 1.0f ) );
			break;
		default:
			break;
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvRandVariable( KeyValues *pKV )
{
	const char *pszVar = pKV->GetString( "var", NULL );
	if ( !pszVar )
	{
		Warning( "MapHack WARNING: $rand block has no 'var'!\n" );
		return;
	}

	MapHackVariable_t *pVar = GetVariableByName( pszVar );
	if ( !pVar )
	{
		Warning( "MapHack WARNING: $rand block 'var' value references a non-existent variable! (%s)\n", pszVar );
		return;
	}

	const char *pszRandMin = MapHack_VariableValueHelper( pKV->GetString( "rand_min", "0" ) );
	const char *pszRandMax = MapHack_VariableValueHelper( pKV->GetString( "rand_max", "1" ) );

	switch ( pVar->m_Type )
	{
		case MapHackType_t::TYPE_INT:
			pVar->SetInt( RandomInt( V_atoi( pszRandMin ), V_atoi( pszRandMax ) ) );
			break;
		case MapHackType_t::TYPE_FLOAT:
			pVar->SetFloat( RandomFloat( V_atof( pszRandMin ), V_atof( pszRandMax ) ) );
			break;
		case MapHackType_t::TYPE_COLOR:
			pVar->SetColor( Color( RandomInt( 0, 255 ), RandomInt( 0, 255 ), RandomInt( 0, 255 ), 255 ) );
			break;
		default:
			break;
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvConsole( KeyValues *pKV ) const
{
	if ( IsPreEntity() )
		return;

	const char *pszCmd = MapHack_VariableValueHelper( pKV->GetString( "cmd", NULL ) );
	if ( pszCmd )
	{
		if ( sv_maphack_allow_servercommand.GetBool() )
		{
			// Send command
			engine->ServerCommand( UTIL_VarArgs( "%s\n", pszCmd ) );
			engine->ServerExecute();
		}
		else
		{
			Warning( "MapHack WARNING: $console key \"cmd\" not allowed, set \"sv_maphack_allow_servercommand 1\" to bypass this check\n" );
		}
	}
	else
	{
		const char *pszMsg = MapHack_VariableValueHelper( pKV->GetString( "msg", NULL ) );
		if ( pszMsg )
		{
			// Print normal spew
			Msg( "%s\n", pszMsg );
		}
		else
		{
			// Print warning
			pszMsg = MapHack_VariableValueHelper( pKV->GetString( "warning", NULL ) );
			if ( pszMsg )
				Warning( "%s\n", pszMsg );
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvFireInput( KeyValues *pKV )
{
	if ( IsPreEntity() )
		return;

	// Fire an input
	CBaseEntity *pEntity = GetEntityHelper( pKV );
	if ( pEntity )
	{
		MapHackType_t type = MapHack_GetTypeByIdentifier( pKV->GetString( "type", NULL ) );
		const char *pszValue = MapHack_VariableValueHelper( pKV->GetString( "value" ), &type );

		SendInput( pEntity, MapHack_VariableValueHelper( pKV->GetString( "input" ) ), pszValue, type );
	}
	else
	{
		Warning( "MapHack WARNING: Failed to send input to \"%s\"!\n", pKV->GetString( "targetname" ) );
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvEdit( KeyValues *pKV )
{
	if ( IsPreEntity() )
	{
		// Find this in entdata
		const int index = GetEntDataIndexHelper( pKV );
		if ( m_vecEntData.IsValidIndex( index ) )
		{
			MapHackEntityData_t *pEntData = m_vecEntData.Element( index );
			if ( pEntData )
			{
				// Replace with our values
				KeyValues *pEntKeyValues = pKV->FindKey( "keyvalues" );
				if ( pEntKeyValues )
				{
					MapHack_ParseEntDataBlockHelper( pEntData, pEntKeyValues );
				}
			}
		}
	}
	else
	{
		CBaseEntity *pEntity = GetEntityHelper( pKV );
		if ( pEntity )
		{
			KeyValues *pEntKeyValues = pKV->FindKey( "keyvalues" );
			if ( pEntKeyValues )
			{
				MapHack_EditEntity( pEntity, pEntKeyValues );
			}
		}
		else
		{
			Warning( "MapHack WARNING: Can't find entity named \"%s\"!\n", pKV->GetString( "targetname" ) );
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvEditAll( KeyValues *pKV )
{
	if ( IsPreEntity() )
	{
		// Iterate ent data
		FOR_EACH_VEC( m_vecEntData, i )
		{
			MapHackEntityData_t *pEntData = m_vecEntData[i];
			if ( !pEntData )
				continue;

			char szKeyName[MAPKEY_MAXLENGTH];
			char szValue[MAPKEY_MAXLENGTH];

			if ( !pEntData->GetFirstKey( szKeyName, szValue ) )
				continue;

			// Loop through our keys
			while ( pEntData->GetNextKey( szKeyName, szValue ) )
			{
				if ( FStrEq( szKeyName, "classname" ) && FStrEq( szValue, pKV->GetString( "classname" ) ) )
				{
					// Replace with our values
					KeyValues *pEntKeyValues = pKV->FindKey( "keyvalues" );
					if ( pEntKeyValues )
					{
						MapHack_ParseEntDataBlockHelper( pEntData, pEntKeyValues );
					}

					break;
				}
			}
		}
	}
	else
	{
		const char *pszClassName = MapHack_VariableValueHelper( pKV->GetString( "classname", NULL ) );
		if ( !pszClassName )
			return;

		for ( const CEntInfo *pInfo = gEntList.FirstEntInfo(); pInfo; pInfo = pInfo->m_pNext )
		{
			CBaseEntity *pEntity = (CBaseEntity *)pInfo->m_pEntity;
			if ( !pEntity )
				continue;

			if ( pEntity->ClassMatches( pszClassName ) )
			{
				KeyValues *pEntKeyValues = pKV->FindKey( "keyvalues" );
				if ( pEntKeyValues )
				{
					MapHack_EditEntity( pEntity, pEntKeyValues );
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvModify( KeyValues *pKV )
{
	// Find "match"
	KeyValues *pMatch = pKV->FindKey( "match" );
	if ( !pMatch )
	{
		Warning( "MapHack WARNING: $modify block is missing a \"match\" key!\n" );
		return;
	}

	// Find the rest of the functions
	KeyValues *pReplace = pKV->FindKey( "replace" );
	KeyValues *pDelete = pKV->FindKey( "delete" );
	KeyValues *pInsert = pKV->FindKey( "insert" );
	KeyValues *pEntKeyValues = pKV->FindKey( "keyvalues" );

	if ( IsPreEntity() )
	{
		// Iterate ent data
		FOR_EACH_VEC( m_vecEntData, i )
		{
			MapHackEntityData_t *pEntData = m_vecEntData[i];
			if ( !pEntData )
				continue;

			char szKeyName[MAPKEY_MAXLENGTH];
			char szValue[MAPKEY_MAXLENGTH];

			if ( !pEntData->GetFirstKey( szKeyName, szValue ) )
				continue;

			if ( !HasMatches( pMatch, pEntData ) )
				continue;

			// Do "replace"
			if ( pReplace )
			{
				KeyValues *pNode = pReplace->GetFirstSubKey();
				while ( pNode )
				{
					// Replace value
					const char *pszValue = MapHack_VariableValueHelper( pNode->GetString() );
					pEntData->SetKeyValue( pNode->GetName(), pszValue );

					pNode = pNode->GetNextKey();
				}
			}

			// Do "delete"
			if ( pDelete )
			{
				KeyValues *pNode = pDelete->GetFirstSubKey();
				while ( pNode )
				{
					// Drop the value if it matches
					const char *pszValue = MapHack_VariableValueHelper( pNode->GetString() );
					char szExtractedValue[256];
					pEntData->GetKeyValue( pNode->GetName(), szExtractedValue, sizeof( szExtractedValue ) );

					if ( FStrEq( pszValue, szExtractedValue ) )
					{
						pEntData->RemoveValue( pNode->GetName() );
					}

					pNode = pNode->GetNextKey();
				}
			}

			// Do "insert"
			if ( pInsert )
			{
				KeyValues *pNode = pInsert->GetFirstSubKey();
				while ( pNode )
				{
					// Insert keyvalue
					const char *pszValue = MapHack_VariableValueHelper( pNode->GetString() );
					pEntData->InsertValue( pNode->GetName(), pszValue );

					pNode = pNode->GetNextKey();
				}
			}

			// Do traditional keyvalues block
			if ( pEntKeyValues )
			{
				MapHack_ParseEntDataBlockHelper( pEntData, pEntKeyValues );
			}
		}
	}
	else
	{
		// Now, iterate all entities
		for ( const CEntInfo *pInfo = gEntList.FirstEntInfo(); pInfo; pInfo = pInfo->m_pNext )
		{
			CBaseEntity *pEntity = static_cast<CBaseEntity *>( pInfo->m_pEntity );
			if ( !pEntity )
				continue;

			if ( !HasMatches( pMatch, pEntity ) )
				continue;

			// Do "replace"
			if ( pReplace )
			{
				KeyValues *pNode = pReplace->GetFirstSubKey();
				while ( pNode )
				{
					// Replace value
					const char *pszValue = MapHack_VariableValueHelper( pNode->GetString() );
					pEntity->KeyValue( pNode->GetName(), pszValue );

					MapHack_DebugMsg( "Changed keyvalue \"%s\" to \"%s\" (targetname: %s)\n",
						pNode->GetName(), pszValue, pEntity->GetDebugName() );

					pNode = pNode->GetNextKey();
				}
			}

			// Do "delete"
			if ( pDelete )
			{
				KeyValues *pNode = pDelete->GetFirstSubKey();
				while ( pNode )
				{
					// Drop the value if it matches
					const char *pszValue = MapHack_VariableValueHelper( pNode->GetString() );
					char szValue[256];
					pEntity->GetKeyValue( pNode->GetName(), szValue, sizeof( szValue ) );

					if ( FStrEq( pszValue, szValue ) )
					{
						// REVISIT: Can we clear keyvalues this way?
						pEntity->KeyValue( pNode->GetName(), "" );

						MapHack_DebugMsg( "Deleted keyvalue \"%s\" (targetname: %s)\n",
							pNode->GetName(), pEntity->GetDebugName() );
					}

					pNode = pNode->GetNextKey();
				}
			}

			// Do "insert"
			if ( pInsert )
			{
				KeyValues *pNode = pInsert->GetFirstSubKey();
				while ( pNode )
				{
					// Insert keyvalue
					const char *pszValue = MapHack_VariableValueHelper( pNode->GetString() );
					pEntity->KeyValue( pNode->GetName(), pszValue );

					MapHack_DebugMsg( "Inserted keyvalue \"%s\" with value \"%s\" (targetname: %s)\n",
						pNode->GetName(), pszValue, pEntity->GetDebugName() );

					pNode = pNode->GetNextKey();
				}
			}

			// Do traditional keyvalues block
			if ( pEntKeyValues )
			{
				MapHack_EditEntity( pEntity, pEntKeyValues );
			}
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvFilter( KeyValues *pKV )
{
	if ( IsPreEntity() )
	{
		FOR_EACH_VEC( m_vecEntData, i )
		{
			MapHackEntityData_t *pEntData = m_vecEntData[i];
			if ( !pEntData )
				continue;

			char szKeyName[MAPKEY_MAXLENGTH];
			char szValue[MAPKEY_MAXLENGTH];

			if ( !pEntData->GetFirstKey( szKeyName, szValue ) )
				continue;

			if ( HasMatches( pKV, pEntData ) )
			{
				m_vecEntData.Remove( i );
				--i;
			}
		}
	}
	else
	{
		// Iterate all entities
		for ( const CEntInfo *pInfo = gEntList.FirstEntInfo(); pInfo; pInfo = pInfo->m_pNext )
		{
			CBaseEntity *pEntity = static_cast<CBaseEntity *>( pInfo->m_pEntity );
			if ( !pEntity )
				continue;

			// Can't filter unsafe entities
			if ( !MapHack_IsSafeEntity( pEntity ) )
				continue;

			if ( HasMatches( pKV, pEntity ) )
			{
				MapHack_DebugMsg( "Filtered entity \"%s\"\n", pEntity->GetDebugName() );
				UTIL_Remove( pEntity );
			}
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvTriggerEvent( KeyValues *pKV )
{
	if ( IsPreEntity() )
		return;

	const char *pszEventName = MapHack_VariableValueHelper( pKV->GetString( "event" ) );
	const char *pszDelay = MapHack_VariableValueHelper( pKV->GetString( "delay", "0.0" ) );

	MapHackEvent_t *pEvent = GetEventByName( pszEventName );
	if ( pEvent )
	{
		TriggerEvent( pEvent, V_atof( pszDelay ) );
	}
	else
	{
		Warning( "MapHack WARNING: Event \"%s\" doesn't exist!\n", pszEventName );
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvStartEvent( KeyValues *pKV )
{
	if ( IsPreEntity() )
		return;

	const char *pszEventName = MapHack_VariableValueHelper( pKV->GetString( "event" ) );
	const char *pszDelay = MapHack_VariableValueHelper( pKV->GetString( "delay", NULL ) );

	MapHackEvent_t *pEvent = GetEventByName( pszEventName );
	if ( pEvent )
	{
		pEvent->m_bStopped = false;
		pEvent->m_flTriggerTime = gpGlobals->curtime;

		if ( pszDelay )
			pEvent->m_flDelayTime = V_atof( pszDelay );

		MapHack_DebugMsg( "Started event \"%s\"\n", pszEventName );
	}
	else
	{
		Warning( "MapHack WARNING: Event \"%s\" doesn't exist!\n", pszEventName );
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvStopEvent( KeyValues *pKV )
{
	if ( IsPreEntity() )
		return;

	const char *pszEventName = MapHack_VariableValueHelper( pKV->GetString( "event" ) );

	MapHackEvent_t *pEvent = GetEventByName( pszEventName );
	if ( pEvent )
	{
		pEvent->m_bTriggered = false;
		pEvent->m_bStopped = true;

		MapHack_DebugMsg( "Stopped event \"%s\"\n", pszEventName );
	}
	else
	{
		Warning( "MapHack WARNING: Event \"%s\" doesn't exist!\n", pszEventName );
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvRespawnEntity( KeyValues *pKV )
{
	if ( IsPreEntity() )
		return;

	// Find the entity we are supposted to respawn
	CBaseEntity *pEntity = GetEntityHelper( pKV, true );
	if ( pEntity )
	{
		CBaseEntity *pNewEntity = RespawnEntity( pEntity );
		if ( pNewEntity )
		{
			MapHack_DebugMsg( "Respawned entity targetnamed \"%s\"\n", pNewEntity->GetDebugName() );
		}
		else
		{
			Warning( "MapHack WARNING: Failed to respawn entity targetnamed \"%s\"!\n", pKV->GetString( "targetname" ) );
		}
	}
	else
	{
		Warning( "MapHack WARNING: Can't find entity named \"%s\"!\n", pKV->GetString( "targetname" ) );
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvRemoveEntity( KeyValues *pKV )
{
	if ( IsPreEntity() )
	{
		// Find this in entdata
		const int index = GetEntDataIndexHelper( pKV );
		if ( m_vecEntData.IsValidIndex( index ) )
		{
			const MapHackEntityData_t *pEntData = m_vecEntData.Element( index );
			if ( pEntData )
			{
				// Remove this
				m_vecEntData.Remove( index );
			}
		}
	}
	else
	{
		// Find the entity we are supposted to remove
		CBaseEntity *pEntity = GetEntityHelper( pKV, true );
		if ( pEntity )
		{
			MapHack_DebugMsg( "Removed entity targetnamed \"%s\"\n", pEntity->GetDebugName() );
			UTIL_Remove( pEntity );
		}
		else
		{
			Warning( "MapHack WARNING: Failed to remove entity targetnamed \"%s\"!\n", pKV->GetString( "targetname" ) );
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvRemoveAllEntities( KeyValues *pKV )
{
	if ( IsPreEntity() )
	{
		// Iterate ent data
		FOR_EACH_VEC( m_vecEntData, i )
		{
			MapHackEntityData_t *pEntData = m_vecEntData[i];
			if ( !pEntData )
				continue;

			char szKeyName[MAPKEY_MAXLENGTH];
			char szValue[MAPKEY_MAXLENGTH];

			if ( !pEntData->GetFirstKey( szKeyName, szValue ) )
				continue;

			// Loop through our keys
			while ( pEntData->GetNextKey( szKeyName, szValue ) )
			{
				if ( FStrEq( szKeyName, "classname" ) && FStrEq( szValue, pKV->GetString( "classname" ) ) )
				{
					// Remove this
					m_vecEntData.Remove( i );
					--i;

					MapHack_DebugMsg( "(Pre-entity) Removed entity \"%s\"\n", pKV->GetString( "classname" ) );
					break;
				}
			}
		}
	}
	else
	{
		// Check first if we should remove all entities by targetname
		const char *pszTargetName = MapHack_VariableValueHelper( pKV->GetString( "targetname", NULL ) );
		if ( pszTargetName )
		{
			// Remove by targetname
			for ( const CEntInfo *pInfo = gEntList.FirstEntInfo(); pInfo; pInfo = pInfo->m_pNext )
			{
				CBaseEntity *pEntity = (CBaseEntity *)pInfo->m_pEntity;
				if ( !pEntity )
					continue;

				if ( !MapHack_IsSafeEntity( pEntity ) )
					continue;

				if ( AllocPooledString( pszTargetName ) == pEntity->GetEntityName() )
					UTIL_Remove( pEntity );

				MapHack_DebugMsg( "Removed all entities targetnamed \"%s\"\n", pszTargetName );
			}
		}
		else
		{
			// Remove by classname
			const char *pszClassName = MapHack_VariableValueHelper( pKV->GetString( "classname", NULL ) );
			if ( pszClassName )
			{
				for ( const CEntInfo *pInfo = gEntList.FirstEntInfo(); pInfo; pInfo = pInfo->m_pNext )
				{
					CBaseEntity *pEntity = (CBaseEntity *)pInfo->m_pEntity;
					if ( !pEntity )
						continue;

					if ( !MapHack_IsSafeEntity( pEntity ) )
						continue;

					if ( pEntity->ClassMatches( pszClassName ) )
						UTIL_Remove( pEntity );
				}

				MapHack_DebugMsg( "Removed all entities classnamed \"%s\"\n", pszClassName );
			}
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvRemoveConnections( KeyValues *pKV )
{
	if ( IsPreEntity() )
		return;

	// Find the entity
	CBaseEntity *pEntity = GetEntityHelper( pKV );
	if ( pEntity )
	{
		if ( MapHack_RemoveEntityConnections( pEntity ) )
		{
			MapHack_DebugMsg( "Removed entity connections from \"%s\"\n", pEntity->GetDebugName() );
		}
		else
		{
			Warning( "MapHack WARNING: Failed to remove entity connections from \"%s\"!\n", pKV->GetString( "targetname" ) );
		}
	}
	else
	{
		Warning( "MapHack WARNING: Can't find entity named \"%s\"!\n", pKV->GetString( "targetname" ) );
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvGetPos( KeyValues *pKV )
{
	if ( IsPreEntity() )
		return;

	const char *pszVar = pKV->GetString( "var", NULL );

	MapHackVariable_t *pVar = GetVariableByName( pszVar );
	if ( !pVar )
	{
		Warning( "MapHack WARNING: $getpos block 'var' value references a non-existent variable! (%s)\n", pszVar );
		return;
	}

	const CBaseEntity *pEntity = GetEntityHelper( pKV );
	if ( !pEntity )
	{
		Warning( "MapHack WARNING: $getpos couldn't find an entity targetnamed \"%s\"!\n", pKV->GetString( "targetname" ) );
		return;
	}

	const Vector &vecOrigin = pEntity->GetAbsOrigin();
	pVar->SetString( UTIL_VarArgs( "%f %f %f", vecOrigin.x, vecOrigin.y, vecOrigin.z ) );
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvSetPos( KeyValues *pKV )
{
	if ( IsPreEntity() )
		return;

	const char *pszValue = MapHack_VariableValueHelper( pKV->GetString( "value", NULL ) );

	CBaseEntity *pEntity = GetEntityHelper( pKV );
	if ( !pEntity )
	{
		Warning( "MapHack WARNING: $setpos couldn't find an entity targetnamed \"%s\"!\n", pKV->GetString( "targetname" ) );
		return;
	}

	// Set positions
	Vector vec = pEntity->GetAbsOrigin();

	if ( pszValue )
	{
		if ( sscanf( pszValue, "%f %f %f", &vec[0], &vec[1], &vec[2] ) != 3 )
		{
			Warning( "MapHack WARNING: Invalid value \"%s\" for $setpos!\n", pszValue );
			return;
		}
	}

	pEntity->SetAbsOrigin( vec );

	MapHack_DebugMsg( "$setpos for \"%s\", new origin is %s\n", pKV->GetString( "targetname" ), pszValue );
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvGetAng( KeyValues *pKV )
{
	if ( IsPreEntity() )
		return;

	const char *pszVar = pKV->GetString( "var", NULL );

	MapHackVariable_t *pVar = GetVariableByName( pszVar );
	if ( !pVar )
	{
		Warning( "MapHack WARNING: $getang block 'var' value references a non-existent variable! (%s)\n", pszVar );
		return;
	}

	const CBaseEntity *pEntity = GetEntityHelper( pKV );
	if ( !pEntity )
	{
		Warning( "MapHack WARNING: $getang couldn't find an entity targetnamed \"%s\"!\n", pKV->GetString( "targetname" ) );
		return;
	}

	const QAngle &angles = pEntity->GetAbsAngles();
	pVar->SetString( UTIL_VarArgs( "%f %f %f", angles.x, angles.y, angles.z ) );
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvSetAng( KeyValues *pKV )
{
	if ( IsPreEntity() )
		return;

	const char *pszValue = MapHack_VariableValueHelper( pKV->GetString( "value", NULL ) );

	CBaseEntity *pEntity = GetEntityHelper( pKV );
	if ( !pEntity )
	{
		Warning( "MapHack WARNING: $setang couldn't find an entity targetnamed \"%s\"!\n", pKV->GetString( "targetname" ) );
		return;
	}

	// Set positions
	QAngle angles = pEntity->GetAbsAngles();

	if ( pszValue )
	{
		if ( sscanf( pszValue, "%f %f %f", &angles[0], &angles[1], &angles[2] ) != 3 )
		{
			Warning( "MapHack WARNING: Invalid value \"%s\" for $setang!\n", pszValue );
			return;
		}
	}

	pEntity->SetAbsAngles( angles );

	MapHack_DebugMsg( "$setang for \"%s\", new angles is %s\n", pKV->GetString( "targetname" ), pszValue );
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvEditField( KeyValues *pKV )
{
	if ( IsPreEntity() )
		return;

	CBaseEntity *pEntity = GetEntityHelper( pKV );
	if ( !pEntity )
	{
		Warning( "MapHack WARNING: $edit_field couldn't find an entity targetnamed \"%s\"!\n",
			pKV->GetString( "targetname" ) );

		return;
	}

	const char *pszKeyName = MapHack_VariableValueHelper( pKV->GetString( "keyname", NULL ) );
	const char *pszFieldName = MapHack_VariableValueHelper( pKV->GetString( "fieldname", NULL ) );
	const char *pszValue = MapHack_VariableValueHelper( pKV->GetString( "value" ) );

	const bool bFound = MapHack_EditEntityField( pEntity, pszKeyName, pszFieldName, pszValue );

	if ( !bFound )
	{
		if ( pszKeyName )
		{
			Warning( "MapHack WARNING: Couldn't find an entity keyfield named \"%s\" (%s)\n",
				pszKeyName, pEntity->GetDebugName() );
		}
		if ( pszFieldName )
		{
			Warning( "MapHack WARNING: Couldn't find an entity datadesc field named \"%s\" (%s)\n",
				pszFieldName, pEntity->GetDebugName() );
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvPlaySound( KeyValues *pKV )
{
	if ( IsPreEntity() )
		return;

	const char *pszName = MapHack_VariableValueHelper( pKV->GetString( "name" ) );
	const char *pszSource = MapHack_VariableValueHelper( pKV->GetString( "source", NULL ) );

	CSoundParameters params;
	if ( !CBaseEntity::GetParametersForSound( pszName, params, NULL ) )
	{
		Warning( "MapHack WARNING: Failed to play sound \"%s\"\n", pszName );
		return;
	}

	if ( !pszSource )
	{
		// Play a global sound
		CBroadcastRecipientFilter filter;
		CBaseEntity::EmitSound( filter, SOUND_FROM_WORLD, pszName );
	}
	else
	{
		// Play from source ent
		CBaseEntity *pEnt = GetEntityByTargetName( pszSource );
		if ( !pEnt )
		{
			Warning( "MapHack WARNING: Sound source entity named \"%s\" doesn't exist!\n", pszSource );
		}
		else
		{
			CPASAttenuationFilter filter( pEnt, params.soundlevel );
			CBaseEntity::EmitSound( filter, pEnt->entindex(), params );
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::KvScript( KeyValues *pKV ) const
{
#if 0 // If your mod has VScript, toggle this
	if ( IsPreEntity() )
		return;

	if ( !g_pScriptVM )
	{
		Warning( "MapHack WARNING: $script failed, no VM running.\n" );
		return;
	}

	const char *pszFile = pKV->GetString( "file", NULL );
	const char *pszRun = pKV->GetString( "run", NULL );

	if ( !pszFile && !pszRun )
	{
		Warning( "MapHack WARNING: $script failed, no script specified.\n" );
		return;
	}

	// Run script
	bool bSuccess = false;
	if ( pszFile )
	{
		// Load from file
		bSuccess = VScriptRunScript( pszFile );
	}
	else if ( pszRun )
	{
		const ScriptStatus_t status = g_pScriptVM->Run( pszRun );
		bSuccess = status != SCRIPT_ERROR;
	}

	if ( !bSuccess )
	{
		Warning( "MapHack WARNING: $script failed, check console for errors.\n" );
	}
#endif
}

//-----------------------------------------------------------------------------
void CMapHackManager::HandleEvents()
{
	FOR_EACH_DICT( m_dictEvents, i )
	{
		MapHackEvent_t *pEvent = m_dictEvents[i];
		if ( pEvent && pEvent->m_Type == MAPHACK_EVENT_TIMED )
		{
			if ( ( pEvent->m_bTriggered && !pEvent->m_bRepeat ) || pEvent->m_bStopped )
				continue;

			if ( pEvent->m_flTriggerTime <= gpGlobals->curtime )
			{
				TriggerEvent( pEvent );
			}
		}
	}

	// Deal with the delayed events
	if ( !m_vecEventQueue.IsEmpty() )
	{
		FOR_EACH_VEC( m_vecEventQueue, i )
		{
			if ( m_vecEventQueue[i].m_flTriggerTime <= gpGlobals->curtime )
			{
				TriggerEvent( m_vecEventQueue[i].m_pEvent );
				m_vecEventQueue.FastRemove( i );
			}
		}
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::TriggerEvent( MapHackEvent_t *pEvent, const float flDelay )
{
	if ( !pEvent )
		return;

	if ( !pEvent->m_pKVData )
		return;

	if ( flDelay > 0.0f )
	{
		// Delay the event
		MapHackDelayedEvent_t delayed;
		delayed.m_pEvent = pEvent;
		delayed.m_flTriggerTime = gpGlobals->curtime + flDelay;

		m_vecEventQueue.AddToTail( delayed );
		return;
	}

	MapHack_DebugMsg( "Triggered event \"%s\"\n", pEvent->m_szName );

	if ( pEvent->m_iDataType == 0 )
	{
		RunEntities( pEvent->m_pKVData );
	}
	else if ( pEvent->m_iDataType == 1 )
	{
		Precache( pEvent->m_pKVData );
	}

	pEvent->m_bTriggered = true;

	if ( pEvent->m_bRepeat )
	{
		pEvent->m_flTriggerTime = gpGlobals->curtime + pEvent->m_flDelayTime;
	}
}

//-----------------------------------------------------------------------------
void CMapHackManager::TriggerEventByName( const char *pszName, const float flDelay )
{
	MapHackEvent_t *pEvent = GetEventByName( pszName );
	if ( !pEvent )
	{
		Warning( "MapHack WARNING: Event label \"%s\" does not exist!\n", pszName );
		return;
	}

	TriggerEvent( pEvent, flDelay );
}

//-----------------------------------------------------------------------------
MapHackEvent_t *CMapHackManager::GetEventByName( const char *pszName )
{
	const int idx = m_dictEvents.Find( pszName );
	if ( !m_dictEvents.IsValidIndex( idx ) )
		return NULL;

	return m_dictEvents[idx];
}

//-----------------------------------------------------------------------------
MapHackType_t CMapHackManager::GetTypeForString( const char *pszValue )
{
	MapHackType_t type = MapHackType_t::TYPE_STRING;

	const int len = V_strlen( pszValue );

	// Here, let's determine if we got a float or an int....
	char *pIEnd;	// pos where int scan ended
	char *pFEnd;	// pos where float scan ended
	const char *pSEnd = pszValue + len; // pos where token ends

	const int iVal = strtol( pszValue, &pIEnd, 10 );
	const double result = strtod( pszValue, &pFEnd );
	NOTE_UNUSED( result );
	const bool bOverflow = ( iVal == LONG_MAX || iVal == LONG_MIN ) && errno == ERANGE;

#ifdef POSIX
	// strtod supports hex representation in strings under posix, but we DON'T
	// want that support in keyvalues, so undo it here if needed
	if ( len > 1 && tolower( pszValue[1] ) == 'x' )
	{
		pFEnd = (char *)pszValue;
	}
#endif

	if ( *pszValue == 0 )
	{
		// It's a string
	}
	else if ( ( pFEnd > pIEnd ) && ( pFEnd == pSEnd ) )
	{
		type = KeyValues::TYPE_FLOAT;
	}
	else if ( pIEnd == pSEnd && !bOverflow )
	{
		type = KeyValues::TYPE_INT;
	}

	return type;
}

//-----------------------------------------------------------------------------
MapHackVariable_t *CMapHackManager::GetVariableByName( const char *pszName )
{
	const int idx = m_dictVars.Find( pszName );
	if ( !m_dictVars.IsValidIndex( idx ) )
		return NULL;

	return m_dictVars[idx];
}

//-----------------------------------------------------------------------------
void CMapHackManager::DumpVariablesToConsole()
{
	ConColorMsg( 0, CON_COLOR_MAPHACK, "MapHack: Active vars\n\n" );

	FOR_EACH_DICT( m_dictVars, i )
	{
		switch ( m_dictVars[i]->m_Type )
		{
			case MapHackType_t::TYPE_INT:
				ConColorMsg( 0, CON_COLOR_MAPHACK, "%s = %d\n", m_dictVars[i]->m_szName, m_dictVars[i]->m_iValue );
				break;
			case MapHackType_t::TYPE_FLOAT:
				ConColorMsg( 0, CON_COLOR_MAPHACK, "%s = %f\n", m_dictVars[i]->m_szName, m_dictVars[i]->m_flValue );
				break;
			case MapHackType_t::TYPE_STRING:
				ConColorMsg( 0, CON_COLOR_MAPHACK, "%s = %s\n", m_dictVars[i]->m_szName, m_dictVars[i]->m_pszValue );
				break;
			case MapHackType_t::TYPE_COLOR:
				ConColorMsg( 0, CON_COLOR_MAPHACK, "%s = %d %d %d\n", m_dictVars[i]->m_szName,
					m_dictVars[i]->m_Color[0], m_dictVars[i]->m_Color[1], m_dictVars[i]->m_Color[2] );
				break;
			default:
				break;
		}
	}

	ConColorMsg( 0, CON_COLOR_MAPHACK, "\nTotal vars: %d\n", m_dictVars.Count() );
}

//-----------------------------------------------------------------------------
bool CMapHackManager::TestIfCondBlock( const char *psz )
{
	bool bResult = false;

	static const char *pszOps[]
	{
		"==", "!=", ">=", ">", "<=", "<",
	};

	const MapHackVariable_t *pLVal = NULL;
	const MapHackVariable_t *pRVal = NULL;

	unsigned int op;

	CUtlVector<char *> vecOutStrings;
	for ( op = 0; op < ARRAYSIZE( pszOps ); ++op )
	{
		if ( V_strstr( psz, pszOps[op] ) )
		{
			V_SplitString( psz, pszOps[op], vecOutStrings );
			break;
		}
	}

	// Clear the '%' chars, we don't have to explicitly reference a var here
	for ( int i = 0; i < vecOutStrings.Count(); ++i )
	{
		if ( vecOutStrings[i][0] == '%' )
			vecOutStrings[i][0] = ' ';
	}

	// Lazy strip...
	for ( int i = 0; i < vecOutStrings.Count(); ++i )
		Q_StripPrecedingAndTrailingWhitespace( vecOutStrings[i] );

	const MapHackType_t lType = GetTypeForString( vecOutStrings[0] );
	const MapHackType_t rType = GetTypeForString( vecOutStrings[1] );

	MapHackVariable_t l, r;
	l.m_Type = lType;
	r.m_Type = rType;

	if ( lType == MapHackType_t::TYPE_STRING )
	{
		pLVal = GetVariableByName( vecOutStrings[0] );
		if ( !pLVal )
		{
			l.SetString( vecOutStrings[0] );
			pLVal = &l;
		}
	}
	else if ( lType == MapHackType_t::TYPE_INT )
	{
		l.SetInt( V_atoi( vecOutStrings[0] ) );
		pLVal = &l;
	}
	else if ( lType == MapHackType_t::TYPE_FLOAT )
	{
		l.SetFloat( V_atof( vecOutStrings[0] ) );
		pLVal = &l;
	}

	if ( rType == MapHackType_t::TYPE_STRING )
	{
		pRVal = GetVariableByName( vecOutStrings[1] );
		if ( !pRVal )
		{
			r.SetString( vecOutStrings[1] );
			pRVal = &r;
		}
	}
	else if ( rType == MapHackType_t::TYPE_INT )
	{
		r.SetInt( V_atoi( vecOutStrings[1] ) );
		pRVal = &r;
	}
	else if ( rType == MapHackType_t::TYPE_FLOAT )
	{
		r.SetFloat( V_atof( vecOutStrings[1] ) );
		pRVal = &r;
	}

	// Test
	if ( pLVal && pRVal )
	{
		if ( pLVal->m_Type == MapHackType_t::TYPE_INT && pRVal->m_Type == MapHackType_t::TYPE_INT )
		{
			switch ( op )
			{
				case 0: bResult = ( pLVal->GetInt() == pRVal->GetInt() ); break;
				case 1: bResult = ( pLVal->GetInt() != pRVal->GetInt() ); break;
				case 2: bResult = ( pLVal->GetInt() >= pRVal->GetInt() ); break;
				case 3: bResult = ( pLVal->GetInt() > pRVal->GetInt() ); break;
				case 4: bResult = ( pLVal->GetInt() <= pRVal->GetInt() ); break;
				case 5: bResult = ( pLVal->GetInt() < pRVal->GetInt() ); break;
				default:
					break;
			}
		}
		else if ( pLVal->m_Type == MapHackType_t::TYPE_FLOAT && pRVal->m_Type == MapHackType_t::TYPE_FLOAT )
		{
			switch ( op )
			{
				case 0: bResult = ( pLVal->GetFloat() == pRVal->GetFloat() ); break;
				case 1: bResult = ( pLVal->GetFloat() != pRVal->GetFloat() ); break;
				case 2: bResult = ( pLVal->GetFloat() >= pRVal->GetFloat() ); break;
				case 3: bResult = ( pLVal->GetFloat() > pRVal->GetFloat() ); break;
				case 4: bResult = ( pLVal->GetFloat() <= pRVal->GetFloat() ); break;
				case 5: bResult = ( pLVal->GetFloat() < pRVal->GetFloat() ); break;
				default:
					break;
			}
		}
		else if ( pLVal->m_Type == MapHackType_t::TYPE_STRING && pRVal->m_Type == MapHackType_t::TYPE_STRING )
		{
			switch ( op )
			{
				case 0: bResult = ( FStrEq( pLVal->GetString(), pRVal->GetString() ) ); break;
				case 1: bResult = ( !FStrEq( pLVal->GetString(), pRVal->GetString() ) ); break;
				default:
					break;
			}
		}
	}

	vecOutStrings.PurgeAndDeleteElements();

	return bResult;
}

//-----------------------------------------------------------------------------
void CMapHackManager::SendInput( CBaseEntity *pEntity, const char *pszInput, const char *pszValue, const MapHackType_t typeOverride )
{
	if ( !pEntity )
		return;

	variant_t variant;
	MapHackType_t type;

	if ( typeOverride != MapHackType_t::TYPE_NONE )
		type = typeOverride;
	else
		type = GetTypeForString( pszValue );

	switch ( type )
	{
		case MapHackType_t::TYPE_STRING:
		{
			variant.SetString( AllocPooledString( pszValue ) );
			break;
		}
		case MapHackType_t::TYPE_INT:
		{
			variant.SetInt( V_atoi( pszValue ) );
			break;
		}
		case MapHackType_t::TYPE_FLOAT:
		{
			variant.SetFloat( V_atof( pszValue ) );
			break;
		}
		default:
		{
			variant.SetString( AllocPooledString( pszValue ) );
			break;
		}
	}

	pEntity->AcceptInput( pszInput, pEntity, pEntity, variant, 0 );
	MapHack_DebugMsg( "Sent input \"%s\" to \"%s\" (value = %s)\n", pszInput, STRING( pEntity->GetEntityName() ), pszValue );
}

//-----------------------------------------------------------------------------
CBaseEntity *CMapHackManager::GetEntityByTargetName( const char *pszTargetName )
{
	// Get it from dict if we spawned it from maphack
	const int idx = m_dictSpawnedEnts.Find( pszTargetName );
	if ( m_dictSpawnedEnts.IsValidIndex( idx ) )
	{
		return m_dictSpawnedEnts[idx].Get();
	}

	for ( const CEntInfo *pInfo = gEntList.FirstEntInfo(); pInfo; pInfo = pInfo->m_pNext )
	{
		CBaseEntity *pEntity = (CBaseEntity *)pInfo->m_pEntity;
		if ( !pEntity )
			continue;

		if ( AllocPooledString( pszTargetName ) == pEntity->GetEntityName() )
		{
			return pEntity;
		}
	}

	return NULL;
}

//-----------------------------------------------------------------------------
CBaseEntity *CMapHackManager::GetEntityByHammerID( const int hammerID )
{
	for ( const CEntInfo *pInfo = gEntList.FirstEntInfo(); pInfo; pInfo = pInfo->m_pNext )
	{
		CBaseEntity *pEntity = (CBaseEntity *)pInfo->m_pEntity;
		if ( !pEntity )
			continue;

		if ( pEntity->m_iHammerID == hammerID )
		{
			return pEntity;
		}
	}

	return NULL;
}

//-----------------------------------------------------------------------------
CBaseEntity *CMapHackManager::GetFirstEntityByClassName( const char *pszClassName )
{
	for ( const CEntInfo *pInfo = gEntList.FirstEntInfo(); pInfo; pInfo = pInfo->m_pNext )
	{
		CBaseEntity *pEntity = (CBaseEntity *)pInfo->m_pEntity;
		if ( !pEntity )
			continue;

		if ( FClassnameIs( pEntity, pszClassName ) )
		{
			return pEntity;
		}
	}

	return NULL;
}

//-----------------------------------------------------------------------------
CBaseEntity *CMapHackManager::GetEntityHelper( KeyValues *pKV, const bool bRestrict )
{
	// This util function tries to find an entity with targetname first, and Hammer ID second
	CBaseEntity *pEntity = NULL;
	const char *pszTargetName = MapHack_VariableValueHelper( pKV->GetString( "targetname", NULL ) );
	if ( pszTargetName )
	{
		pEntity = GetEntityByTargetName( pszTargetName );
	}
	else
	{
		const int hammerID = V_atoi( MapHack_VariableValueHelper( pKV->GetString( "id", "-1" ) ) );
		if ( hammerID != -1 )
			pEntity = GetEntityByHammerID( hammerID );
	}

	if ( !pEntity )
	{
		return NULL;
	}

	// Don't return entities that are unsafe
	if ( bRestrict && !MapHack_IsSafeEntity( pEntity ) )
	{
		return NULL;
	}

	return pEntity;
}

//-----------------------------------------------------------------------------
CBaseEntity *CMapHackManager::RespawnEntity( CBaseEntity *pEntity ) const
{
	CBaseEntity *pNewEntity = NULL;

	char szHammerID[16];
	V_sprintf_safe( szHammerID, "%d", pEntity->m_iHammerID );

	UTIL_Remove( pEntity );

	// Respawn from entdata
	char szTokenBuffer[MAPKEY_MAXLENGTH];
	const char *pEntData = HasEntData() ? GetMapEntitiesString() : engine->GetMapEntitiesString();
	for ( ; true; pEntData = MapEntity_SkipToNextEntity( pEntData, szTokenBuffer ) )
	{
		char token[MAPKEY_MAXLENGTH];
		pEntData = MapEntity_ParseToken( pEntData, token );
		if ( !pEntData )
		{
			break;
		}

		if ( token[0] != '{' )
		{
			continue;
		}

		CEntityMapData entData( (char*)pEntData );
		char szExtractedHammerID[16];
		if ( !entData.ExtractValue( "hammerid", szExtractedHammerID ) )
		{
			continue;
		}

		// Identify using HammerIDs
		if ( !FStrEq( szHammerID, szExtractedHammerID ) )
		{
			continue;
		}

		// Parse and create as usual
		MapEntity_ParseEntity( pNewEntity, pEntData, NULL );
		break;
	}

	if ( pNewEntity )
	{
		DispatchSpawn( pNewEntity );
	}

	return pNewEntity;
}

//-----------------------------------------------------------------------------
void CMapHackManager::BuildEntityList( const char *pszEntData )
{
	char szTokenBuffer[MAPKEY_MAXLENGTH];

	// Grab ent data
	// Loop through all entities in the map data
	for ( ; true; pszEntData = MapEntity_SkipToNextEntity( pszEntData, szTokenBuffer ) )
	{
		// Parse the opening brace
		char szToken[MAPKEY_MAXLENGTH];
		pszEntData = MapEntity_ParseToken( pszEntData, szToken );

		// Check to see if we've finished or not
		if ( !pszEntData )
			break;

		if ( szToken[0] != '{' )
		{
			// If this happens, just bail
			return;
		}

		// Determine data block size
		int entBlockSize = 0;
		int cur = 0;
		const char *psz = &pszEntData[cur];
		while ( psz && *psz != '\0' )
		{
			if ( *psz == '}' )
			{
				entBlockSize = cur + 1;
				break;
			}

			psz = &pszEntData[++cur];
		}

		// Add some padding...
		entBlockSize += MAPHACK_ENTDATA_BLOCK_PADDING;

		// Parse the entity and add it to the list
		MapHackEntityData_t *pEntData = ParseEntityData( pszEntData, entBlockSize );

		// List it
		if ( pEntData )
		{
			m_vecEntData.AddToTail( pEntData );
		}
	}
}

//-----------------------------------------------------------------------------
MapHackEntityData_t *CMapHackManager::ParseEntityData( const char *pszEntData, const int size )
{
	// Allocate this entity block, we don't want to store a pointer to full entdata!
	char *pszNewData = new char[size];
	int i = 0;
	int bufIdx = 0;
	const char *psz = &pszEntData[i];
	while ( psz && *psz != '\0' )
	{
		if ( bufIdx >= size )
			break;

		// Copy characters to the buffer til closing bracket
		if ( *psz == '}' )
		{
			// Slice here
			pszNewData[bufIdx] = *psz;
			pszNewData[++bufIdx] = '\0';
			break;
		}

		// Keep writing
		pszNewData[bufIdx] = *psz;
		++bufIdx;

		psz = &pszEntData[++i];
	}

	if ( bufIdx >= size ) // Bounds check
		pszNewData[size - 1] = '\0';
	else
		pszNewData[bufIdx] = '\0';

	// Create new entry
	// Entdata buffer is freed in destructor
	MapHackEntityData_t *pEntData = new MapHackEntityData_t( pszNewData, size );
	return pEntData;
}

//-----------------------------------------------------------------------------
void CMapHackManager::FinalizeEntData()
{
	if ( m_pNewMapData )
	{
		delete[] m_pNewMapData;
		m_pNewMapData = NULL;
	}

	// Set up buffer
	m_pNewMapData = new char[1];
	m_pNewMapData[0] = '\0';

	// Walk through our entries, and convert to readable entdata
	FOR_EACH_VEC( m_vecEntData, i )
	{
		const MapHackEntityData_t *pEntData = m_vecEntData[i];
		if ( !pEntData )
			continue;

		const int bufSize = V_strlen( pEntData->GetEntDataPtr() ) + 3; // new line, nul
		char *pszBuffer = new char[bufSize];
		const int outLen = GetEntDataString( pEntData, pszBuffer, bufSize );

		if ( outLen >= bufSize )
		{
			// For safety, this shouldn't happen
			Assert( 0 );

			delete[] pszBuffer;
			continue;
		}

		// Add newline and null terminator
		pszBuffer[outLen] = '\n';
		pszBuffer[outLen + 1] = '\0';

		// Append!
		const int newLen = V_strlen( m_pNewMapData ) + bufSize + 1;
		char *pBlock = m_pNewMapData;
		m_pNewMapData = (char *)realloc( pBlock, newLen + 1 );
		if ( m_pNewMapData )
		{
			V_strcat( m_pNewMapData, pszBuffer, newLen );
		}

		delete[] pszBuffer;
	}
}

//-----------------------------------------------------------------------------
int CMapHackManager::GetEntDataString( const MapHackEntityData_t *pEntData, char *pszOut, const int outSize )
{
	// Get pointer to the data
	const char *pszData = pEntData->GetEntDataPtr();
	if ( !pszData )
		return 0;

	// HACKHACK: Fix this for realsies, MapHackEntityData_t cuts starting bracket so add it here
	pszOut[0] = '{';

	// Copy characters to the buffer til closing bracket
	int i = 0;
	int outIdx = 1; // (Skip by one because of the aforementioned hack)
	const char *psz = &pszData[i];
	while ( psz && *psz != '\0' )
	{
		if ( outIdx >= outSize )
			break;

		if ( *psz == '}' )
		{
			// Slice here
			pszOut[outIdx] = *psz;
			pszOut[++outIdx] = '\0';
			break;
		}

		// Convert tabs to spaces
		if ( *psz == '\t' )
		{
			pszOut[outIdx] = ' ';
			++outIdx;

			psz = &pszData[++i];
			continue;
		}

		// Keep writing
		pszOut[outIdx] = *psz;
		++outIdx;

		psz = &pszData[++i];
	}

	if ( outIdx >= outSize ) // Bounds check
	{
		pszOut[outSize - 1] = '\0';
		return outSize - 1;
	}

	pszOut[outIdx] = '\0';
	return outIdx;
}

//-----------------------------------------------------------------------------
int CMapHackManager::GetEntDataIndexHelper( KeyValues *pKV )
{
	// This util function tries to find an ent data index with targetname first, and Hammer ID second
	int idx = m_vecEntData.InvalidIndex();
	const char *pszTargetName = MapHack_VariableValueHelper( pKV->GetString( "targetname", NULL ) );
	if ( pszTargetName )
	{
		idx = GetEntDataIndexByTargetName( pszTargetName );
	}
	else
	{
		const int hammerID = V_atoi( MapHack_VariableValueHelper( pKV->GetString( "id", "-1" ) ) );
		if ( hammerID != -1 )
			idx = GetEntDataIndexByHammerID( hammerID );
	}

	return idx;
}

//-----------------------------------------------------------------------------
int CMapHackManager::GetEntDataIndexByTargetName( const char *pszTargetName )
{
	FOR_EACH_VEC( m_vecEntData, i )
	{
		MapHackEntityData_t *pEntData = m_vecEntData[i];
		if ( !pEntData )
			continue;

		char szKeyName[MAPKEY_MAXLENGTH];
		char szValue[MAPKEY_MAXLENGTH];

		if ( !pEntData->GetFirstKey( szKeyName, szValue ) )
			continue;

		// Loop through our keys
		while ( pEntData->GetNextKey( szKeyName, szValue ) )
		{
			if ( !V_stricmp( szKeyName, "targetname" ) )
			{
				// Compare names
				if ( FStrEq( szValue, pszTargetName ) )
					return i;
			}
		}
	}

	return m_vecEntData.InvalidIndex();
}

//-----------------------------------------------------------------------------
int CMapHackManager::GetEntDataIndexByHammerID( const int id )
{
	FOR_EACH_VEC( m_vecEntData, i )
	{
		MapHackEntityData_t *pEntData = m_vecEntData[i];
		if ( !pEntData )
			continue;

		char szKeyName[MAPKEY_MAXLENGTH];
		char szValue[MAPKEY_MAXLENGTH];

		if ( !pEntData->GetFirstKey( szKeyName, szValue ) )
			continue;

		// Loop through our keys
		while ( pEntData->GetNextKey( szKeyName, szValue ) )
		{
			if ( !V_stricmp( szKeyName, "hammerid" ) )
			{
				// Compare id
				if ( V_atoi( szValue ) == id )
					return i;
			}
		}
	}

	return m_vecEntData.InvalidIndex();
}

//-----------------------------------------------------------------------------
void CMapHackManager::ResetMapHack( const bool bDeleteKeyValues )
{
	// Remove all callbacks from output events
	FOR_EACH_DICT( m_dictEvents, i )
	{
		const MapHackEvent_t *pEvent = m_dictEvents[i];
		if ( !pEvent || pEvent->m_Type != MAPHACK_EVENT_OUTPUT )
			continue;

		RemoveOutputCallback( pEvent->m_hOutputEnt.Get() );
	}

	// Stop listening to game events
	StopListeningForAllEvents();

	m_vecEventQueue.Purge();

	// Delete everything
	m_dictSpawnedEnts.Purge();
	m_dictEvents.PurgeAndDeleteElements();
	m_dictVars.PurgeAndDeleteElements();

	if ( bDeleteKeyValues )
	{
		if ( m_pMapHack )
		{
			m_pMapHack->deleteThis();
			m_pMapHack = NULL;
		}

		m_pszIdentifier = "";
	}
}

//-----------------------------------------------------------------------------
MapHackFunctionType_t CMapHackManager::GetFunctionTypeByString( const char *pszString )
{
	const int idx = m_dictFunctions.Find( pszString );
	if ( !m_dictFunctions.IsValidIndex( idx ) )
		return MAPHACK_FUNCTION_INVALID;

	return m_dictFunctions[idx];
}

//-----------------------------------------------------------------------------
MapHackEventType_t CMapHackManager::GetEventTypeByString( const char *pszString )
{
	if ( FStrEq( pszString, "EVENT_TRIGGER" ) )
	{
		return MAPHACK_EVENT_TRIGGER;
	}

	if ( FStrEq( pszString, "EVENT_TIMED" ) )
	{
		return MAPHACK_EVENT_TIMED;
	}

	if ( FStrEq( pszString, "EVENT_OUTPUT" ) )
	{
		return MAPHACK_EVENT_OUTPUT;
	}

	if ( FStrEq( pszString, "EVENT_GAMEEVENT" ) )
	{
		return MAPHACK_EVENT_GAMEEVENT;
	}

	return MAPHACK_EVENT_INVALID;
}

//-----------------------------------------------------------------------------
// MapHackEntityData_t
//-----------------------------------------------------------------------------
bool MapHackEntityData_t::GetKeyValue( const char *pszKeyName, char *pszValue, const int bufSize ) const
{
	const char *pszInputData = m_pEntData;

	while ( pszInputData )
	{
		char szToken[MAPKEY_MAXLENGTH];
		pszInputData = MapEntity_ParseToken( pszInputData, szToken );
		if ( szToken[0] == '}' )
			break;

		if ( !V_stricmp( szToken, pszKeyName ) )
		{
			MapEntity_ParseToken( pszInputData, szToken );
			V_strncpy( pszValue, szToken, bufSize );
			return true;
		}

		pszInputData = MapEntity_ParseToken( pszInputData, szToken );
	}

	return false;
}

//-----------------------------------------------------------------------------
bool MapHackEntityData_t::GetFirstKey( char *pszKeyName, char *pszValue )
{
	m_pCurrentKey = m_pEntData; // reset the status pointer
	return GetNextKey( pszKeyName, pszValue );
}

//-----------------------------------------------------------------------------
bool MapHackEntityData_t::GetNextKey( char *pszKeyName, char *pszValue )
{
	char szToken[MAPKEY_MAXLENGTH];

	// Parse key
	char *pPrevKey = m_pCurrentKey;
	m_pCurrentKey = const_cast<char *>( MapEntity_ParseToken( m_pCurrentKey, szToken ) );
	if ( szToken[0] == '}' )
	{
		// Step back
		m_pCurrentKey = pPrevKey;
		return false;
	}

	if ( !m_pCurrentKey )
	{
		return false;
	}

	V_strncpy( pszKeyName, szToken, MAPKEY_MAXLENGTH );

	// Fix up keynames with trailing spaces
	int i = V_strlen( pszKeyName );
	while ( i && pszKeyName[i - 1] == ' ' )
	{
		pszKeyName[i - 1] = 0;
		--i;
	}

	// Parse value	
	m_pCurrentKey = const_cast<char *>( MapEntity_ParseToken( m_pCurrentKey, szToken ) );
	if ( !m_pCurrentKey )
	{
		return false;
	}
	if ( szToken[0] == '}' )
	{
		return false;
	}

	// Value successfully found
	V_strncpy( pszValue, szToken, MAPKEY_MAXLENGTH );
	return true;
}

//-----------------------------------------------------------------------------
bool MapHackEntityData_t::SetKeyValue( const char *pszKeyName, const char *pszNewValue, const int keyInstance )
{
	char *pInputData = m_pEntData;

	char szNewValue[1024];
	int currentKeyInstance = 0;

	while ( pInputData )
	{
		char szToken[MAPKEY_MAXLENGTH];

		// Get keyname
		pInputData = const_cast<char *>( MapEntity_ParseToken( pInputData, szToken ) );
		if ( szToken[0] == '}' ) // End of entity?
		{
			// Must not have seen the classname
			break;
		}

		// Is this the right key?
		if ( !V_strcmp( szToken, pszKeyName ) )
		{
			++currentKeyInstance;

			if ( currentKeyInstance > keyInstance )
			{
				V_sprintf_safe( szNewValue, "\"%s\"", pszNewValue );

				const int newValueLen = V_strlen( szNewValue ) + 1; // + 1 for the null terminator
				const int entDataSize = V_strlen( m_pEntData ) + newValueLen;

				// Find the start & end of the token we're going to replace
				char *pPostData = new char[entDataSize];
				char *pPrevData = pInputData;
				pInputData = const_cast<char *>( MapEntity_ParseToken( pInputData, szToken ) );
				V_strncpy( pPostData, pInputData, entDataSize );

				// prevData has a space at the start, separating the value from the key
				// Add 1 to prevData when pasting in the new Value, to account for the space
				V_strncpy( pPrevData + 1, szNewValue, newValueLen );
				V_strcat( pPrevData, pPostData, entDataSize - ( pPrevData - m_pEntData ) + 1 );

				delete[] pPostData;

				m_pCurrentKey = m_pEntData;
				return true;
			}

			// It's a new instance
			return InsertValue( pszKeyName, pszNewValue );
		}

		// Skip over value
		pInputData = const_cast<char *>( MapEntity_ParseToken( pInputData, szToken ) );
	}

	// Not found? Insert value
	return InsertValue( pszKeyName, pszNewValue );
}

//-----------------------------------------------------------------------------
bool MapHackEntityData_t::InsertValue( const char *pszKeyName, const char *pszNewValue )
{
	// Find end bracket
	bool bFoundBracket = false;
	int i = 0;
	char *psz = &m_pEntData[i];
	while ( psz && *psz != '\0' )
	{
		if ( *psz == '}' )
		{
			// Got it, rid of it
			--psz;
			*psz = '\0';

			bFoundBracket = true;
			break;
		}

		psz = &m_pEntData[++i];
	}

	if ( !bFoundBracket )
	{
		// Bad ent data
		return false;
	}

	// Write line
	char szLine[256];
	V_sprintf_safe( szLine, "\n\"%s\" \"%s\"", pszKeyName, pszNewValue );

	m_iBlockSize += V_strlen( szLine );
	m_iBlockSize += 3; // new line, end bracket, nul

	// Re-allocate this block of data
	char *pBlock = m_pEntData;
	m_pEntData = (char *)realloc( pBlock, m_iBlockSize );

	if ( m_pEntData )
	{
		// Append
		V_strcat( m_pEntData, szLine, m_iBlockSize );

		// Re-add end bracket
		V_strcat( m_pEntData, "\n}", m_iBlockSize );

		return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
bool MapHackEntityData_t::RemoveValue( const char *pszKeyName ) const
{
	const char *pInputData = m_pEntData;
	const char *pPrevData = NULL;

	bool bValueFound = false;

	while ( pInputData )
	{
		char szToken[MAPKEY_MAXLENGTH];
		pPrevData = pInputData;

		pInputData = const_cast<char *>( MapEntity_ParseToken( pInputData, szToken ) );
		if ( szToken[0] == '}' )
			break;

		if ( !V_strcmp( szToken, pszKeyName ) )
		{
			bValueFound = true;
			break;
		}
	}

	if ( bValueFound && pPrevData )
	{
		// We'll want to strip this line...
		char szLine[1024];
		szLine[0] = '\0';

		int i = 1;
		int bufIdx = 0;
		const char *psz = pPrevData + 1; // hop over newline
		while ( psz && *psz != '\0' )
		{
			if ( *psz == '\n' )
			{
				szLine[bufIdx] = *psz;
				szLine[++bufIdx] = '\0';
				break;
			}

			szLine[bufIdx] = *psz;
			++bufIdx;

			psz = &pPrevData[++i];
		}

		// Find substring and strip it from ent data
		char *pszSub = V_strstr( m_pEntData, szLine );
		if ( !pszSub )
		{
			Assert( 0 );
			return false;
		}

		const int remLen = V_strlen( szLine );
		const char *pszFrom = pszSub + remLen;
		const char *pszEnd = V_strstr( pszFrom, szLine );

		const char *pszCurrent = pszEnd;
		while ( pszCurrent )
		{
			V_memmove( pszSub, pszFrom, pszEnd - pszFrom );
			pszSub += pszEnd - pszFrom;
			pszFrom = pszEnd + remLen;

			pszEnd = V_strstr( pszFrom, szLine );
			pszCurrent = pszEnd;
		}

		V_memmove( pszSub, pszFrom, 1 + V_strlen( pszFrom ) );
	}

	return bValueFound;
}
