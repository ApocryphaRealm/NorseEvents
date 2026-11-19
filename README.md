# NorseEvents

The shared event channel of the Norse frameworks - a UE4SS v4.0.0 C++ mod for NORSE: Oath of Blood. It hooks each game
moment once and tells every subscriber, so the overhaul's modules (block/stamina/poise, followers, attributes, hit
effects) and later frameworks (Conditions, Payload, HUD, Impact) listen instead of each hooking the same game function.

First of the further frameworks in `4. plans\norse-realtime-combat-20261116\ASSESSMENT-AND-PLAN.md` (survey:
`RESEARCH-more-frameworks.md`). Modelled on powerofthree's Papyrus Extender events and on UE4SS's own advice that many
separate hooks on one function slow the game.

## Events (v0.1)

| Event | subject | other | asset | values | from |
|---|---|---|---|---|---|
| UnitDamaged / UnitHealed | the unit | the cause | the ability | old -> new health; flags NE_CRIT | hook: UUnitComponent::SetRemainingHealth_Internal (NAL 6) |
| UnitDied | the victim | the killer | the ability | flags NE_EXECUTED | hook: ACombat_GM::OnUnitDeath (NAL 2) |
| AbilityExecuted | the unit | the target | the ability | | hook: ACombat_GM::ExecuteAbility (NAL 3) |
| FightStarted | the CombatArea | | | | published by NorseRealtimeNative (it owns that hook) |
| FightEnded | the CombatArea / game mode | | | | hooks: ACombat_GM::EndCombat (NAL 4), ACombatArea::EndCombat (NAL 5); real-time fights: NorseRealtimeNative |

One UnitDied per unit until it lives again (the game calls its death function twice); a death raised inside a health
change is published after that change's UnitDamaged. Real-time attacks do not go through the game's ExecuteAbility, and
real-time fights never reach its EndCombat - those two arrive from NorseRealtimeNative or later modules.

## Using it from another mod

`include/NorseEvents.h` (header only, plain C exports `NE_Subscribe`, `NE_Unsubscribe`, `NE_Publish`):

```cpp
#include "NorseEvents.h"
static void OnDeath(const NE_Event* e, void*) { /* e->subject = victim, e->other = killer */ }
if (auto* api = ne::Api()) api->Subscribe("UnitDied", &OnDeath, nullptr);   // "*" = every event
```

Callbacks run on the game thread inside the game's own call - keep them short. Console: `ne.log on|off`, `ne.stats`.
Log: `NorseEvents.log` beside `NorseEvents.ini` (or `sLogDir`), one line per event.

## Needs

The Norse Address Library 1.0.x with table IDs 2-6 (`ue4ss\NorseAddressLibrary\`). Without it every hook stays off and
the log says so. Build: `xmake f -p windows -a x64 -m release --toolchain=msvc -y`, then `xmake` (needs `../NorsePatcher`
and `../NorseAddressLibrary` beside it; MinHook is in `third_party/`).

## Licence

GPL-3.0-or-later (see LICENSE). MinHook (BSD 2-Clause), compiled into the DLL, keeps its own notice in THIRD-PARTY-NOTICES.txt.
