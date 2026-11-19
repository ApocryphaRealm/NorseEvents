# Changelog

## 1.0.0 - 2026-11-18 - untested

### Added
- First release. It contains everything built and tested in 0.1.0, listed here and kept as history below:
- Native hooks through the Norse Address Library (IDs 2-6): UnitDamaged / UnitHealed (old and new health, cause, crit, ability), UnitDied (victim, killer, executed, ability), AbilityExecuted, FightEnded.
- The C API in `include/NorseEvents.h`: `NE_Subscribe` (* = all), `NE_Unsubscribe`, `NE_Publish` - other mods publish events they own (NorseRealtimeNative: FightStarted, FightEnded for real-time fights).
- One log line per event; console `ne.log on|off`, `ne.stats`.
- NE_Event.text (read with NE_Text, guarded by size; older publishers unaffected) - NorsePayload sends the payload name in it.
- main.dll carries a Windows version resource (file and product version 1.0.0, shown in its Properties > Details).

### Fixed
- The address library is looked for in `ue4ss\`, not `ue4ss\Mods\`.
- One UnitDied per death (the game reported it twice), published after the lethal hit's UnitDamaged.

## 0.1.0 - 2026-11-17 (tested in game: TEST-RESULT-0.1.0.txt)
### Added
- Native hooks through the Norse Address Library (IDs 2-6): UnitDamaged / UnitHealed (old and new health, cause, crit,
  ability), UnitDied (victim, killer, executed, ability), AbilityExecuted, FightEnded.
- The C API in `include/NorseEvents.h`: `NE_Subscribe` ("*" = all), `NE_Unsubscribe`, `NE_Publish` - other mods publish
  events they own (NorseRealtimeNative: FightStarted, FightEnded for real-time fights).
- One log line per event; console `ne.log on|off`, `ne.stats`.
### Added (later, 2026-11-17)
- NE_Event.text (read with NE_Text, guarded by size; older publishers unaffected) - NorsePayload sends the payload name in it.
### Fixed (during the first test)
- The address library is looked for in `ue4ss\`, not `ue4ss\Mods\`.
- One UnitDied per death (the game reported it twice), published after the lethal hit's UnitDamaged.
