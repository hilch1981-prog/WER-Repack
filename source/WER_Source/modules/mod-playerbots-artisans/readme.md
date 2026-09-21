# mod-playerbots-artisans

Makes crafter bots feel like **players with a trade**. On a timer, bots with a
crafting profession advertise in Trade (and optionally General) chat, naming
**real recipes they actually know** — so the ads are level-appropriate for free:

```
[Trade] Enchanter (300) LFW - [Enchant Weapon - Crusader], [Enchant Cloak - Greater Resistance]. Whisper me for enchants!
[Trade] Alchemist (285) LFW - [Greater Arcane Elixir], [Elixir of the Mongoose]. Whisper me for potions/transmutes!
```

For [AzerothCore](https://www.azerothcore.org/) +
[Playerbots](https://github.com/liyunfan1223/mod-playerbots).

## Why it's automatically level-appropriate

You don't need a "bots grind their profession" system — the Playerbots factory
already sets each bot's skill to `level x 5` (level-20 -> 100, level-60 -> 300) and
re-runs as bots level. This module reads each bot's **learned recipes** and current
skill straight from its spellbook, so a low-level bot only advertises the cheap
recipes it truly knows and a 300 bot advertises its best (ads are sorted by recipe
difficulty). Enchanters advertise enchant names; crafters advertise the item they
produce.

## Features

- Recipe-driven, level-appropriate Trade/General advertising.
- Reliable channel posting - posts straight to the faction-wide "Trade - City"
  channel bots join at login, **bypassing the Playerbots `SayToChannel` zone
  filter** that otherwise silently drops every Trade message.
- Per-bot cooldown and a per-tick cap to keep Trade from being spammed.
- Coordinates via the shared `BotActivityRegistry.h`: bots busy in a dungeon run,
  raid sim, or world-PvP event (or in combat / an instance) don't advertise.

## Requirements

- AzerothCore + mod-playerbots.
- (Optional) the dungeon-sim / world-PvP mods if you want the shared reservation
  to actually gate anything - otherwise the registry is simply always "free."

## Installation

```bash
cd azerothcore/modules
git clone https://github.com/<your-user>/mod-playerbots-artisans
# re-run CMake configure, then rebuild
```

Keep `src/BotActivityRegistry.h` byte-identical to the copies shipped with the
other mods. Copy the `.conf.dist` to your config.

> **Note on naming:** AzerothCore derives the loader function from the *folder*
> name, so this module's folder must be `mod-playerbots-artisans` (matching
> `Addmod_playerbots_artisansScripts`). The `.cpp` filename doesn't matter.

## Configuration

`conf/mod_playerbots_artisans.conf.dist`:

| Key | Default | Meaning |
|---|---|---|
| `PlayerbotArtisans.Enable` | 1 | Master switch |
| `PlayerbotArtisans.TickSeconds` | 45 | Seconds between ad batches |
| `PlayerbotArtisans.AdsPerTick` | 2 | Ads per batch |
| `PlayerbotArtisans.MinSkill` | 100 | Min skill to advertise |
| `PlayerbotArtisans.AdvertiseCount` | 2 | Recipes named per ad |
| `PlayerbotArtisans.PerBotCooldownSeconds` | 900 | Per-bot re-advertise delay |
| `PlayerbotArtisans.UseTradeChannel` | 1 | Post in Trade |
| `PlayerbotArtisans.UseGeneralChannel` | 0 | Post in zone General |

## Roadmap

This is the **advertising** layer. Fulfilment (a player takes a bot up on its ad)
is a natural next step: Playerbots already has craft + trade actions with a fee, so
whisper/trade -> craft with the player's mats + fee -> trade it back is the intended
path. The auction house is left to AHBot on purpose.

## Credits

Built on AzerothCore and mod-playerbots.