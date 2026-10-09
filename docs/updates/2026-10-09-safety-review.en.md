# WER safety source update — October 9, 2026

[한국어 / detailed provenance](2026-10-09-safety-review.md) · **English**

Baseline WER commit: `235d99f1f55fa742834dfa1fbd21a876cc5b9365`.
This is a selective **19-file source update: 16 C++ files and 3 headers**, not an upstream branch replacement.
Original v1.1.0 tags and executable/source ZIPs are retained. Production files, settings and data are not deployed by publishing this update.

## Changes

| Area | Change | Limit |
|---|---|---|
| Inventory | Snapshot identifiers/quantities before destructive, merging, banking, handing over, unequipping, opening and charge-consuming operations | Some paths are preventive hardening, not observed production fixes |
| Banking result | Return success if at least one requested operation succeeded | Not an atomic all-or-nothing transaction |
| Equipment candidates | Collect GUIDs while visiting and re-resolve before evaluation | Current invalidation/crash not demonstrated; #2895 caveat below |
| Queued commands | Store sender GUID instead of raw Player pointer; revalidate online presence and current permission | Does not solve all later Event/ActionBasket lifetime issues |
| Legends | Avoid reset/greeting repetition when selecting the same master | New-master and existing BG policies retained |
| Quest/aura | Guard missing quest interaction objects and original stolen-aura casters | Unmerged upstream proposals |
| Dungeon Clear | Explicitly engage the combat engine/attack for an otherwise idle combat-flagged leader tank | Not a threat multiplier or a complete BWL fix |
| AH Bot | Allow an item through any enabled acquisition source | Existing quantity, pricing and filters unchanged |
| AoE loot | Preserve group/roll/quest/skinning rights; require full storage before removing a quest quantity | Real hook behavior, distribution and loss/duplication testing still needed |
| Guildhouse | Match new object quaternion to its orientation | No bulk modification of existing DB objects |

Existing Korean text, locale behavior, WER branding, configuration keys and policies are retained.
No active config or SQL migration is included. The 2,000-bot target, standalone Ollama exclusion, enhanced-worldchat disablement,
warband-camp exclusion and Individual Progression stage/multiplier hold remain unchanged.

## Provenance

The Korean report lists exact SHA values and original author credits; commit authors/co-author trailers retain those credits.
Status below was checked October 9, 2026; it is a dated snapshot, not a claim about future upstream state.

- Playerbots [#2811](https://github.com/mod-playerbots/mod-playerbots/pull/2811): Terry Raimondo/Keleborn, merged into **test-staging**; WER ordering and banking result adapted separately.
- Playerbots [#2880](https://github.com/mod-playerbots/mod-playerbots/pull/2880): Vezajin, **test-staging** merge; WER permission rechecks added separately.
- Playerbots [#2898](https://github.com/mod-playerbots/mod-playerbots/pull/2898): Avirar, **open/unmerged**.
- AzerothCore [#27961](https://github.com/azerothcore/azerothcore-wotlk/pull/27961): Stroe, **open/unmerged**.
- [WOW Legends v1.6.0](https://github.com/WOWLegendsHQ/wow-legends-community/releases/tag/v1.6.0): same-master guard; individual author unavailable from the release archive.
- [Dungeon Clear be8f9d0c](https://github.com/jrad7/mod-dungeon-clear/commit/be8f9d0c857c43fc94ea5dd894c014f2c8ea5332): Jared Wright; only the two-file tank assist change selected.
- AH Bot [#166](https://github.com/azerothcore/mod-ah-bot/pull/166): SamuelHenderson, default-branch change.
- AoE Loot [#67](https://github.com/azerothcore/mod-aoe-loot/pull/67): EricksOliveira, default-branch change plus WER-specific capacity/ownership guards.
- Guildhouse [#86](https://github.com/azerothcore/mod-guildhouse/pull/86): Yagz, **open/unmerged**.

**Playerbots #2895 correction:** Avirar closed [the proposal](https://github.com/mod-playerbots/mod-playerbots/pull/2895)
without merging after withdrawing the current test-staging crash explanation: the collected bag items were not freed on the traced path,
and the original crash was attributed to a merge artifact. WER retains the GUID approach as **defensive hardening only**, not a proven crash fix.
The publication changes two explanatory comment lines only; the delta records separate build-input and published hashes.
Synthetic harness invalidation is not evidence that the real WER path invalidates an item.

## Verified versus unverified

| Check | Evidence / scope |
|---|---|
| Source integrity | 19 baseline/prepared hashes; forward/reverse byte-exact patch verification |
| Small regressions | Extracted bodies and stubs pass; not real-core concurrency or gameplay |
| Real translation units | 16/16 syntax/type checks pass |
| Full native build | Separate VS2022 x64 Release auth/world link passes; 18 registered modules match; standalone Ollama excluded |
| DLL/provider probe | Pass in diagnostic host; actual short-lived auth DLL path sampling was not captured |
| Fresh isolated MySQL 8.4.9 | SQL, UTF-8 roundtrip, Korean ICU, TLS encryption and graceful shutdown pass; not CA/hostname authentication |
| Clean release DB import | Four dumps exit 0, 538 tables; zero player characters and one default ADMIN record; not password-login or complete locale validation |
| Auth dry-run | DB pool, Korean realm loading and exit 0 pass; returns before network listen |
| GitHub CI | Read-only verifier regression tests and complete source manifest checks; no native server build or runtime deployment |
| Full world/client/2,000 bots/live LLM | **Not run for this update** |

Finite checks include 512 AH source-option combinations, 4,096 ownership-guard combinations, 65,536 full-capacity combinations
and 1,441 rotation angles. These counts are **small function/stub cases**, not thousands of gameplay runs.
Initial diagnostic assembly/logging/import errors were repaired before PASS; they are not evidence of production core fixes.
Expected self-signed test CA/default-config warnings remain. No dependency security binaries were upgraded.

Full-build input patch SHA-256: `bb7c3cb061016757d0f49584bd103c58797ebe97e2eb485499fb4ffa6ffdecd6`.
Only the equipment-selection comment correction differs from that built input; C++ behavior equivalence was checked.
No keys, passwords, private paths/IPs, DB contents, runtime logs, EXEs/DLLs or game map/DBC files are added in the source change.

## Before production deployment

Validate inventory merging/splitting/destruction/banking/last charges and loss/duplication; command logout/permission/reconnect paths;
DC pause/off/stay/follow and cross-floor targeting; all AoE group/QLP/skinning/partial-space behavior; guild-object rotation and separation;
full world initialization, actual network/client login, Korean output and sustained population/load.
For five quest items with space for two, the intended guard grants **zero and retains all five** until space is available.

Held work includes strict item-link parsing, broader Event ownership cancellation, large DC DPS waits/time wrap/scout prerequisites,
Ollama integration and OpenSSL/MySQL security binary upgrades. Existing Brewfest, city-duel, Dalaran, English/pet/guild-distribution issues
are not declared fixed by this update. User authorization and a rollback plan are required before production changes.

Ongoing upstream review covers **both integrated modules and Legends source**, tracking exact SHAs, branch/PR status,
locale/branding/local-patch impacts and unavailable evidence. It does not automatically install updates or restart a server.
Original licenses remain in force; non-commercial research use is a recommendation, not an added GPL/AGPL restriction.
