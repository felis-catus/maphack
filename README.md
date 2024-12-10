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

# License
0BSD, which means you can use this code in your mod without requirements.
