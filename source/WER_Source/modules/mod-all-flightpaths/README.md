# mod-all-flightpaths

Aldrynth QoL module: grant **all faction-appropriate flight points** on login so characters never need to discover taxi nodes.

## Purpose / scope

| Does | Does not |
|------|----------|
| Unlock Alliance or Horde taxi hubs on every continent (EK/Kalimdor, Outland, Northrend) | Unlock opposite-faction-only hubs |
| Persist via normal `characters.taximask` save | Change taxi prices, mounts, or IP gates |
| Apply to new and existing characters on login | Require SQL |

You still must **be on that continent** at a flight master to fly there.

## Configuration

See `conf/allFlightPaths.conf.dist`:

| Key | Default | Meaning |
|-----|---------|---------|
| `AllFlightPaths.Enable` | 1 | Master switch |
| `AllFlightPaths.Announce` | 0 | Login whisper |

## Install

```bash
cd modules
git submodule add https://github.com/VenomekPL/mod-all-flightpaths.git mod-all-flightpaths
# reconfigure CMake, rebuild worldserver, copy .conf.dist → .conf
```

## License

MIT
