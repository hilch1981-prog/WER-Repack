# Install, connect and shut down

[한국어](INSTALL.md) · **English**

These instructions apply to the **existing v1.1.0 Windows executable repack**, not to a source-only prerelease or GitHub source archive.

## Download and extract

Download **`WER_REPACK_VER.1.1.0.zip`** from the [v1.1.0 release](https://github.com/hilch1981-prog/WER-Repack/releases/tag/v1.1.0)
and compare its hash against `SHA256SUMS.txt`. Extract completely into a **new**, writable local folder.
Do not run from inside the ZIP, use a protected folder, or overwrite an existing server/data directory.
Korean/spaced paths are supported, but a short path such as `C:\WER\WER REPACK_VER.1.1.0` is preferable.

Alternatively, download **both** `.zip.001` and `.zip.002` plus checksums to one folder.
Extract `.001` with a suitable [7-Zip](https://www.7-zip.org/) installation, or inspect and run the supplied merge helper:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Merge-Repack.ps1
```

The helper verifies the parts and creates a new combined ZIP without overwriting an existing one.
Keep both parts until extraction is complete. Split and single ZIPs have the same contents.
GitHub's automatic Source code ZIP does not supply a playable server.

## Defaults and first launch

| Setting | Existing v1.1.0 package default |
|---|---|
| Game account | `admin / admin`, GM 3 |
| MySQL | `127.0.0.1:3306`; do not expose publicly |
| Auth / world ports | `3724` / `59823` |
| Realm address | Detected Radmin `26.*` address, otherwise loopback |
| Bots | Target 2,000 online; generated on first launch |

Configure [your NVIDIA API key](NVIDIA_API.en.md) if you want external LLM chat.
It is not required for basic startup. Start once, in order:

1. **`1_MYSQL.bat`**: wait for first-install preparation and `ready for connections`.
2. **`2_AUTHSERVER.bat`**: confirm auth startup.
3. **`3_WORLDSERVER.bat`**: wait for world initialization and initial bot creation.

Keep the three visible consoles open. Do not start duplicate instances.
Normal server operation is not a background watchdog; temporary DB-setup processes are first-install preparation only.
Restarting does **not** reinstall the DB. Never delete `mysql/data` to fix a launch error.

## Connect the client

Use the matching Korean 3.3.5a / build 12340 client. For a client on the same PC, edit `Data\koKR\realmlist.wtf`:

```text
set realmlist 127.0.0.1
```

Remote Radmin users must join the operator's Radmin network and use **that server's Radmin address**.
Do not copy an old operator/private address from screenshots. A VPN connection is not equivalent to Internet port forwarding.
If necessary, the operator allows only the auth/world ports through the firewall; **keep MySQL 3306 private**.
If changing ports or RealmAddress in `scripts/settings.json`, keep the client, firewall and DB realm address consistent.
Back up before changing an existing environment.

**Change the public default admin password before enabling remote access. Do not share GM credentials.**
Optional addon folders go in `Interface/AddOns`; avoid overlapping bot-management addons.

## Shutdown and backup

Stop **world → auth → MySQL**, using Ctrl+C in each console, waiting for completion, and answering the batch prompt if shown.
Do not force-kill a server before saves finish. Back up after graceful shutdown or use a proper consistent DB backup procedure;
copying an active MySQL data directory does not guarantee a consistent backup.

## Troubleshooting

- MySQL: check existing port use, disk space, write access and `logs/mysql-error.log`; do not stop another DB indiscriminately.
- World launch guard: check Windows commit headroom. The roughly 8 GB guard is not a sustained bot-load certification.
- Login works but world entry fails: check realm address, world port, VPN and firewall consistency.
- LLM silence/errors: check [NVIDIA configuration](NVIDIA_API.en.md); a separate `.world` mode is not used.
- Review consoles and `logs/Errors.log`, `logs/AuthErrors.log`, `logs/mysql-error.log`; redact secrets and personal data before sharing.

This fresh-install package does not automatically migrate existing accounts/characters or fix every client crash.
