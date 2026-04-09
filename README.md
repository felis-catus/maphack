# MapHack
MapHack is a system for Source SDK mods that allows the creation and manipulation of entities at runtime. Users can create alternative versions of maps, as well as patch them without the need to recompile the map.

Implemented into both No More Room in Hell and Pirates, Vikings, and Knights II.

More details and examples:
https://developer.valvesoftware.com/wiki/Maphack_Fundamentals

# Installation
Assuming you have a fresh installation of Source SDK2013:
1. Copy `src` to either sp or mp directory (depending on your mod), so that both maphack_manager.cpp / .h files are in the `server` directory
2. Add both files to your server VPC project, e.g. `server_hl2mp.vpc`
```
$File "maphack_manager.cpp"
$File "maphack_manager.h"
```
3. Open `game/server/cbase.cpp`, then `#include "maphack_manager.h"`, and on function `CBaseEntityOutput::FireOutput()`, add this line of code at the bottom:
```
CMapHackManager::InvokeEntityOutputCallbacks( MapHackOutputCallbackParams_t( this, Value, pActivator, pCaller, fDelay ) );
```
4. Open `game/server/gameinterface.cpp`, then `#include "maphack_manager.h"`, and on function `CServerGameDLL::LevelInit()`, add this line after `g_flServerCurTime = gpGlobals->curtime;`:
```
	const char *pszHackedEntities = GetMapHackManager()->LevelInit( pMapEntities );
	if ( pszHackedEntities && GetMapHackManager()->HasEntData() )
		pMapEntities = pszHackedEntities;
```
5. (Optional) If your mod has round restarts, go to the function where the map is cleaned up and reset e.g. `CTeamplayRoundBasedRules::CleanUpMap()`, add the usual #include, and at the end of the function, you'll want to replace line `MapEntity_ParseAllEntities( engine->GetMapEntitiesString(), &filter, true );` with:
```
  if ( GetMapHackManager()->HasEntData() )
		MapEntity_ParseAllEntities( GetMapHackManager()->GetMapEntitiesString(), &filter, true );
	else
		MapEntity_ParseAllEntities( engine->GetMapEntitiesString(), &filter, true );
```
Then, after that, add this snippet
```
	if ( GetMapHackManager()->HasMapHack() )
		GetMapHackManager()->ReloadMapHack();
```
6. (Optional) Add entity `instant_trigger` to your server VPC project, it is a simple radius trigger for maphack authors
```
$File "instant_trigger.cpp"
$File "instant_trigger.h"
```
# License
0BSD, which means you can use this code in your mod without requirements.
