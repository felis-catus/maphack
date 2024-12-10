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

#ifndef MAPHACK_MANAGER_H
#define MAPHACK_MANAGER_H

#include "GameEventListener.h"

//-----------------------------------------------------------------------------
#define MAPHACK_DEFAULT_IDENTIFIER "maphack"

//-----------------------------------------------------------------------------
static const char *g_pszMapHackKeyWords[]
{
	"entities",
	"events",
	"precache",
	"vars",
	"includes",
	"pre_entities"
};

//-----------------------------------------------------------------------------
enum MapHackFunctionType_t
{
	MAPHACK_FUNCTION_INVALID = -1,

	MAPHACK_FUNCTION_IF,
	MAPHACK_FUNCTION_SET,
	MAPHACK_FUNCTION_INCREMENT,
	MAPHACK_FUNCTION_DECREMENT,
	MAPHACK_FUNCTION_RAND,

	MAPHACK_FUNCTION_CONSOLE,
	MAPHACK_FUNCTION_FIRE,
	MAPHACK_FUNCTION_EDIT,
	MAPHACK_FUNCTION_EDIT_ALL,
	MAPHACK_FUNCTION_MODIFY,
	MAPHACK_FUNCTION_FILTER,
	MAPHACK_FUNCTION_TRIGGER,
	MAPHACK_FUNCTION_START,
	MAPHACK_FUNCTION_STOP,
	MAPHACK_FUNCTION_RESPAWN,
	MAPHACK_FUNCTION_REMOVE,
	MAPHACK_FUNCTION_REMOVE_ALL,
	MAPHACK_FUNCTION_REMOVE_CONNECTIONS,

	MAPHACK_FUNCTION_GETPOS,
	MAPHACK_FUNCTION_SETPOS,
	MAPHACK_FUNCTION_GETANG,
	MAPHACK_FUNCTION_SETANG,

	MAPHACK_FUNCTION_EDIT_FIELD,

	MAPHACK_FUNCTION_PLAYSOUND,
	MAPHACK_FUNCTION_SCRIPT,

	MAPHACK_FUNCTION_COUNT,
};

//-----------------------------------------------------------------------------
static const char *g_pszMapHackFunctionTable[]
{
	// Variables
	"$if",					// Check for variable condition
	"$set",					// Set variable
	"$increment",			// Increment variable
	"$decrement",			// Decrement variable
	"$rand",				// Set a variable to a random value

	// Basic functions
	"$console",				// Send a command to console, or debug spew
	"$fire",				// Fire an input
	"$edit",				// Set KeyValues for existing entity
	"$edit_all",			// Set KeyValues for all existing entities with the classname
	"$modify",				// Extended KeyValue modification
	"$filter",				// Remove entities by matching keyvalues
	"$trigger",				// Trigger a MapHack event
	"$start",				// Start a timed MapHack event
	"$stop",				// Stop a timed MapHack event
	"$respawn",				// Respawn an entity from entdata
	"$remove",				// Remove an entity
	"$remove_all",			// Remove all named entities
	"$remove_connections",	// Remove all output connections

	// Entity positions
	"$getpos",				// Get entity origin, assigns it to a variable
	"$setpos",				// Set entity origin
	"$getang",				// Get entity angles, assigns it to a variable
	"$setang",				// Set entity angles

	// Entity datadesc manipulation
	"$edit_field",			// Edit entity field

	// Extra functions
	"$playsound",			// Emits a sound
	"$script",				// Runs a VScript
};

COMPILE_TIME_ASSERT( ARRAYSIZE( g_pszMapHackFunctionTable ) == MAPHACK_FUNCTION_COUNT );

//-----------------------------------------------------------------------------
enum MapHackEventType_t
{
	MAPHACK_EVENT_INVALID = -1,
	MAPHACK_EVENT_TRIGGER,
	MAPHACK_EVENT_TIMED,
	MAPHACK_EVENT_OUTPUT,
	MAPHACK_EVENT_GAMEEVENT,
};

//-----------------------------------------------------------------------------
// Load flags
//-----------------------------------------------------------------------------
enum
{
	MAPHACK_INCLUDE = 1 << 0, // Include file to existing maphack
	MAPHACK_RUN_ENTITIES = 1 << 1, // Run entities on load (post-entity)
	MAPHACK_REGISTER_EVENTS = 1 << 2, // Register events on load (post-entity)
	MAPHACK_REGISTER_VARS = 1 << 3, // Register variables on load (pre-entity)
	MAPHACK_LOAD_INCLUDES = 1 << 4, // Load includes (pre-entity)
	MAPHACK_PRECACHE = 1 << 5, // Precache on load (pre-entity)
	MAPHACK_COMPLAIN = 1 << 6, // Complain if something goes wrong
};

#define MAPHACK_LOAD_PRE_ENTITY (MAPHACK_REGISTER_VARS | MAPHACK_LOAD_INCLUDES | MAPHACK_PRECACHE | MAPHACK_COMPLAIN)
#define MAPHACK_LOAD_POST_ENTITY (MAPHACK_RUN_ENTITIES | MAPHACK_REGISTER_EVENTS | MAPHACK_REGISTER_VARS | MAPHACK_LOAD_INCLUDES | MAPHACK_PRECACHE | MAPHACK_COMPLAIN)

//-----------------------------------------------------------------------------
// Entity output callbacks
//-----------------------------------------------------------------------------
struct MapHackOutputCallbackParams_t
{
	MapHackOutputCallbackParams_t( const CBaseEntityOutput *pSource,
		const variant_t &value, CBaseEntity *pActivator, CBaseEntity *pCaller, const float flDelay )
	{
		m_pSource = pSource;
		m_pValue = &value;
		m_pActivator = pActivator;
		m_pCaller = pCaller;
		m_flDelay = flDelay;
	}

	const CBaseEntityOutput *m_pSource;
	const variant_t *m_pValue;
	CBaseEntity *m_pActivator;
	CBaseEntity *m_pCaller;
	float m_flDelay;
};

//-----------------------------------------------------------------------------
typedef void( *FnMapHackOutputCallback_t )( 
	CBaseEntity *pEntity, const char *pszName, const MapHackOutputCallbackParams_t &params );

//-----------------------------------------------------------------------------
struct MapHackOutputCallback_t
{
	CHandle<CBaseEntity> m_hEntity;
	FnMapHackOutputCallback_t m_fnCallback;
};

//-----------------------------------------------------------------------------
struct MapHackEntityData_t
{
	MapHackEntityData_t( char *pEntBlock, const int entBlockSize = -1 ) :
		m_pEntData( pEntBlock ), m_pCurrentKey( pEntBlock )
	{
		m_iBlockSize = entBlockSize;
	}

	~MapHackEntityData_t()
	{
		if ( m_pEntData )
		{
			delete[] m_pEntData;
			m_pEntData = NULL;
		}
	}

	bool GetKeyValue( const char *pszKeyName, char *pszValue, int bufSize ) const;
	bool SetKeyValue( const char *pszKeyName, const char *pszNewValue, int keyInstance = 0 );
	bool InsertValue( const char *pszKeyName, const char *pszNewValue );
	bool RemoveValue( const char *pszKeyName ) const;

	bool GetFirstKey( char *pszKeyName, char *pszValue );
	bool GetNextKey( char *pszKeyName, char *pszValue );

	char *GetEntDataPtr() const { return m_pEntData; }

private:
	char *m_pEntData;
	char *m_pCurrentKey;

	int m_iBlockSize;
};

//-----------------------------------------------------------------------------
typedef KeyValues::types_t MapHackType_t;
struct MapHackVariable_t
{
	MapHackVariable_t()
	{
		m_szName[0] = '\0';
		m_Type = MapHackType_t::TYPE_INT;
		m_pszValue = NULL;
		m_iValue = 0;
		m_flValue = 0.0;
		V_memset( m_Color, 0, 4 );
	}

	~MapHackVariable_t()
	{
		delete[] m_pszValue;
	}

	char m_szName[128];
	MapHackType_t m_Type;

	char *m_pszValue;

	union
	{
		int m_iValue;
		float m_flValue;
		int m_Color[4];
	};

	const char *GetValue() const { return m_pszValue; }
	bool GetBool() const { return ( m_Type == MapHackType_t::TYPE_INT ) ? ( m_iValue != 0 ) : false; }
	int GetInt() const { return ( m_Type == MapHackType_t::TYPE_INT ) ? m_iValue : 0; }
	float GetFloat() const { return ( m_Type == MapHackType_t::TYPE_FLOAT ) ? m_flValue : 0.0f; }
	Color GetColor() const { return ( m_Type == MapHackType_t::TYPE_COLOR ) ? Color( m_Color[0], m_Color[1], m_Color[2] ) : Color( 0, 0, 0 ); }
	const char *GetString() const { return ( m_Type == MapHackType_t::TYPE_STRING ) ? m_pszValue : ""; }

	void SetValue( const char *pszValue )
	{
		delete[] m_pszValue;

		const int size = V_strlen( pszValue ) + 1;
		m_pszValue = new char[size];
		V_strncpy( m_pszValue, pszValue, size );
	}

	void SetInt( const int i ) { m_iValue = i; Convert(); }
	void SetFloat( const float fl ) { m_flValue = fl; Convert(); }
	void SetColor( const Color &clr ) { m_Color[0] = clr.r(); m_Color[1] = clr.g(); m_Color[2] = clr.b(); Convert(); }
	void SetString( const char *pszString ) { SetValue( pszString ); }

	void Convert()
	{
		if ( m_Type == MapHackType_t::TYPE_INT )
			SetValue( UTIL_VarArgs( "%d", m_iValue ) );
		else if ( m_Type == MapHackType_t::TYPE_FLOAT )
			SetValue( UTIL_VarArgs( "%f", m_flValue ) );
		else if ( m_Type == MapHackType_t::TYPE_COLOR )
			SetValue( UTIL_VarArgs( "%d %d %d", m_Color[0], m_Color[1], m_Color[2] ) );
	}
};

//-----------------------------------------------------------------------------
struct MapHackEvent_t
{
	MapHackEvent_t()
	{
		m_szName[0] = '\0';
		m_Type = MAPHACK_EVENT_INVALID;
		m_bTriggered = false;
		m_flTriggerTime = 0.0;

		m_pKVData = NULL;
		m_iDataType = -1;

		m_bRepeat = false;
		m_bStopped = false;
		m_flDelayTime = 0.0;

		m_hOutputEnt = NULL;
		m_szOutputEntName[0] = '\0';
		m_szOutputName[0] = '\0';

		m_szGameEventName[0] = '\0';
	}

	~MapHackEvent_t()
	{
		if ( m_pKVData )
			m_pKVData->deleteThis();
	}

	char m_szName[128];
	MapHackEventType_t m_Type;
	bool m_bTriggered;
	float m_flTriggerTime;

	KeyValues *m_pKVData;
	int m_iDataType;

	// MAPHACK_EVENT_TIMED
	bool m_bRepeat;
	bool m_bStopped;
	float m_flDelayTime;

	// MAPHACK_EVENT_OUTPUT
	EHANDLE m_hOutputEnt;
	char m_szOutputEntName[128];
	char m_szOutputName[128];

	// MAPHACK_EVENT_GAMEEVENT
	char m_szGameEventName[128];
};

//-----------------------------------------------------------------------------
struct MapHackDelayedEvent_t
{
	MapHackEvent_t *m_pEvent;
	float m_flTriggerTime;
};

//-----------------------------------------------------------------------------
class CMapHackManager : public CGameEventListener
{
public:
	CMapHackManager();
	~CMapHackManager() OVERRIDE;

	bool Init();
	void Shutdown();

	void Think();

	void LevelInitPostEntity();
	void LevelShutdownPostEntity();

	void FireGameEvent( IGameEvent *event ) OVERRIDE;

	const char *LevelInit( const char *pMapData );

	void OnEntityOutputFired( const CBaseEntity *pEntity, const char *pszName, const MapHackOutputCallbackParams_t &params );

	bool LoadMapHack( KeyValues *pKV, int loadFlags );
	bool LoadMapHack( KeyValues *pKV, int loadFlags, const char *pszIdentifier );
	bool LoadMapHackFromFile( const char *pszFileName, int loadFlags );

	void ReloadMapHack();

	void LoadIncludes( KeyValues *pKV, int loadFlags = 0 );
	void RegisterVariables( KeyValues *pKV );
	static void Precache( KeyValues *pKV );
	void RegisterEvents( KeyValues *pKV, KeyValues *pMapHack );
	void RunEntities( KeyValues *pKV );

	void HandleEvents();
	void TriggerEvent( MapHackEvent_t *pEvent, float flDelay = 0.0f );
	void TriggerEventByName( const char *pszName, float flDelay = 0.0f );

	MapHackEvent_t *GetEventByName( const char *pszName );

	static MapHackType_t GetTypeForString( const char *pszValue );

	MapHackVariable_t *GetVariableByName( const char *pszName );
	void DumpVariablesToConsole();

	MapHackFunctionType_t GetFunctionTypeByString( const char *pszString );
	static MapHackEventType_t GetEventTypeByString( const char *pszString );

	void ResetMapHack( bool bDeleteKeyValues = true );

	bool HasMapHack() const { return ( m_pMapHack != NULL ); }

	bool HasEntData() const { return ( m_pNewMapData != NULL ); }
	const char *GetMapEntitiesString() const { return m_pNewMapData; }

	bool IsPreEntity() const { return m_bPreEntity; }

	const char *GetIdentifier() const { return m_pszIdentifier; }
	void SetIdentifier( const char *pszIdentifier ) { m_pszIdentifier = pszIdentifier; }

	static void RegisterOutputCallback( CBaseEntity *pEnt, FnMapHackOutputCallback_t fn );
	static void RemoveOutputCallback( const CBaseEntity *pEnt );
	static void RemoveAllOutputCallbacks();
	static void InvokeEntityOutputCallbacks( const MapHackOutputCallbackParams_t &params );

private:
	void KvIfCond( KeyValues *pKV );
	void KvSetVariable( KeyValues *pKV );
	void KvIncrement( KeyValues *pKV );
	void KvDecrement( KeyValues *pKV );
	void KvRandVariable( KeyValues *pKV );

	void KvConsole( KeyValues *pKV ) const;
	void KvFireInput( KeyValues *pKV );
	void KvEdit( KeyValues *pKV );
	void KvEditAll( KeyValues *pKV );
	void KvModify( KeyValues *pKV );
	void KvFilter( KeyValues *pKV );
	void KvTriggerEvent( KeyValues *pKV );
	void KvStartEvent( KeyValues *pKV );
	void KvStopEvent( KeyValues *pKV );
	void KvRespawnEntity( KeyValues *pKV );
	void KvRemoveEntity( KeyValues *pKV );
	void KvRemoveAllEntities( KeyValues *pKV );
	void KvRemoveConnections( KeyValues *pKV );

	void KvGetPos( KeyValues *pKV );
	void KvSetPos( KeyValues *pKV );
	void KvGetAng( KeyValues *pKV );
	void KvSetAng( KeyValues *pKV );

	void KvEditField( KeyValues *pKV );

	void KvPlaySound( KeyValues *pKV );
	void KvScript( KeyValues *pKV ) const;

	bool TestIfCondBlock( const char *psz );

	static void SendInput( CBaseEntity *pEntity, const char *pszInput, const char *pszValue, MapHackType_t typeOverride = MapHackType_t::TYPE_NONE );

	// Helper functions
	CBaseEntity *GetEntityByTargetName( const char *pszTargetName );
	static CBaseEntity *GetEntityByHammerID( int hammerID );
	static CBaseEntity *GetFirstEntityByClassName( const char *pszClassName );
	CBaseEntity *GetEntityHelper( KeyValues *pKV, bool bRestrict = false );
	CBaseEntity *RespawnEntity( CBaseEntity *pEntity ) const;

	// For $modify and $filter functions
	// Templated for both entity variants (pre/post)
	template <class T>
	static bool HasMatches( KeyValues *pParentNode, T *pEntity );

	void BuildEntityList( const char *pszEntData );
	static MapHackEntityData_t *ParseEntityData( const char *pszEntData, int size );
	void FinalizeEntData();
	static int GetEntDataString( const MapHackEntityData_t *pEntData, char *pszOut, int outSize );

	int GetEntDataIndexHelper( KeyValues *pKV );
	int GetEntDataIndexByTargetName( const char *pszTargetName );
	int GetEntDataIndexByHammerID( int id );

	KeyValues *m_pMapHack;

	CUtlDict<MapHackFunctionType_t> m_dictFunctions;

	CUtlDict<EHANDLE> m_dictSpawnedEnts;
	CUtlDict<MapHackEvent_t*> m_dictEvents;
	CUtlDict<MapHackVariable_t*> m_dictVars;

	CUtlVector<MapHackDelayedEvent_t> m_vecEventQueue;

	// Entity data
	CUtlVector<MapHackEntityData_t*> m_vecEntData;
	char *m_pNewMapData;

	bool m_bPreEntity;

	const char *m_pszIdentifier;
};

//-----------------------------------------------------------------------------
const char *MapHack_VariableValueHelper( const char *pszValue, MapHackType_t *pType = NULL );

//-----------------------------------------------------------------------------
template <class T>
bool CMapHackManager::HasMatches( KeyValues *pParentNode, T *pEntity )
{
	int totalKeys = 0;
	int totalMatches = 0;
	KeyValues *pMatchNode = pParentNode->GetFirstSubKey();
	while ( pMatchNode )
	{
		const char *pszMatchName = pMatchNode->GetName();
		const char *pszMatchValue = MapHack_VariableValueHelper( pMatchNode->GetString() );

		char szTempValue[256];
		const bool bFound = pEntity->GetKeyValue( pszMatchName, szTempValue, sizeof( szTempValue ) );

		if ( bFound && FStrEq( szTempValue, pszMatchValue ) )
		{
			++totalMatches;
		}

		++totalKeys;
		pMatchNode = pMatchNode->GetNextKey();
	}

	return totalKeys != 0 && totalKeys == totalMatches;
}

//-----------------------------------------------------------------------------
extern CMapHackManager *const g_pMapHackManager;
inline CMapHackManager *GetMapHackManager()
{
	return g_pMapHackManager;
}

#endif
