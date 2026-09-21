# mod-wowlegends

Custom commands for the WOW Legends server (AzerothCore 3.3.5a).

## `.gear` (Game Master)
Wraps the mod-playerbots gearing engine (`.playerbots bot initself=<quality>`), which already does
spec-aware item selection + enchants + gems. Targets the **selected player**, or yourself if none.

| Command | Effect |
|---|---|
| `.gear level` | Gear to uncommon (green) — leveling gear |
| `.gear rare` | Gear to rare (blue) |
| `.gear epic` | Gear to epic (purple) |
| `.gear max` | Best available (epic) for the current level |
| `.gear undress` | Move all equipped items to bags |

> Requires mod-playerbots to be present (it is, on WOW Legends).

## Roadmap (v2)
- `.gear save/load <name>` — outfit presets
- Explicit "stash old gear to bank" before re-gearing
