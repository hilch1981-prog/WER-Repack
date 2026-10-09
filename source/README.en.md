# WER source, version matching and build

[한국어](README.md) · **English**

The **v1.1.0 tag** contains the integrated source corresponding to the existing Windows repack.
The updated source contains the October 9 safety changes and **does not match the older release EXEs/source ZIP**.
The first publication is a distribution snapshot, not a substitute for upstream development history.

- Assembly base: `06234df3d5ab26c93f4f1f06f3edb828b73ecd3c` plus integrated Legends/WER patches.
- `SOURCE_MANIFEST.json`: current `WER_Source/` file hashes/sizes.
- `manifests/WER_1.1.0_SOURCE_MANIFEST.json`: preserved original-release manifest.
- `manifests/WER_20261009_SAFETY_DELTA.json`: 19 files' baseline, build-input and published hashes.
- `WER_1.1.0.patch`: historical branding/Unicode console patch; **already applied**. Do not apply it twice.
- The old release's `WER_Source.zip` remains the original 12,736-file snapshot, SHA-256 `8b16f079d4aa7c5ca3a0636f8a94502d0193280318d739c53d28a4dcf458147b`.

From the repository root, run `python tools/verify-source-manifest.py` for read-only hash/size/missing/unlisted checks.
It does not build, connect to a DB or start a server. Preserve original AUTHORS, file headers and component licenses.

## Build baseline

The runnable **v1.1.1-rc.2** includes the 19-file safety update and revised Banner.cpp. [Release/validation notes](../docs/releases/v1.1.1-rc.2.md) distinguish its tests and remaining limits. The safety delta is historical; SOURCE_MANIFEST.json is the current complete source inventory. Source and game data are not bundled in the executable ZIP.

Visual Studio 2022 C++ x64 / MSVC 14.44, Windows SDK, CMake, Boost 1.84.0, OpenSSL 4.x and MySQL 8.4.9 development files:

```powershell
.\build-source.ps1 -BoostRoot 'C:\SDK\boost_1_84_0' `
  -OpenSSLRoot 'C:\SDK\OpenSSL4' `
  -MySQLRoot 'C:\SDK\mysql-8.4.9' `
  -MySQLExecutable 'C:\SDK\mysql-8.4.9\bin\mysql.exe'
```

Run from this `source/` folder and replace example paths with installed SDK locations.
The helper does not install dependencies. Builds consume resources; do not run them against a live test server without coordination.
The standalone Ollama exclusion and Individual Progression hold remain in effect.
Treat building a fresh base DB separately from importing the repack's initialized dumps; do not overwrite production data.

OpenSSL major versions must match each dependent binary's ABI; renaming a DLL is not compatibility.
`packaging/` is historical reference, can contain old/local assumptions, and is not a general-user production launcher.

The [safety report](../docs/updates/2026-10-09-safety-review.en.md) records full auth/world linking and isolated DB/auth dry-run tests.
It does not certify your SDK combination, full world startup, client gameplay or sustained bot load.
