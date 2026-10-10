#!/usr/bin/env python3
"""Read-only, no-extraction verification of a WER runnable ZIP (Python 3.9+)."""

import hashlib
import json
import re
import sys
import zipfile
from pathlib import PurePosixPath


def verify(archive_path):
    errors = []
    with zipfile.ZipFile(archive_path) as archive:
        names = archive.namelist()
        if len(names) != len(set(names)):
            errors.append("duplicate archive members")
        roots = {PurePosixPath(name).parts[0] for name in names if name}
        if len(roots) != 1:
            raise ValueError("expected one repack root")
        prefix = next(iter(roots)) + "/"
        relative_files = set()
        for item in archive.infolist():
            name = item.filename
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts or "\\" in name or ":" in name:
                errors.append("unsafe member")
            if ((item.external_attr >> 16) & 0o170000) == 0o120000:
                errors.append("symlink member")
            if item.is_dir():
                continue
            relative = name[len(prefix):]
            relative_files.add(relative)
            if re.match(r"(?:source|WER_Source|mysql/data|data/(?:dbc|maps|vmaps|mmaps|Cameras|pathways))(/|$)", relative, re.I):
                errors.append("excluded data/source: " + relative)
            if relative.lower().endswith((
                ".cpp", ".h", ".cxx", ".mpq", ".dbc", ".map", ".vmap", ".vmtree",
                ".vmtile", ".vmo", ".mmap", ".mmtile", ".wlp", ".wmo", ".m2",
                ".adt", ".pdb", ".log",
            )):
                errors.append("excluded source/client-data/cache/log: " + relative)
        required = {
            "authserver.exe", "worldserver.exe", "map_extractor.exe", "vmap4_extractor.exe",
            "vmap4_assembler.exe", "mmaps_generator.exe", "mmaps-config.yaml",
            "1_MYSQL.bat", "2_AUTHSERVER.bat", "3_WORLDSERVER.bat",
            "scripts/wer-console-prepare.ps1", "scripts/settings.json",
            "mysql/bin/mysql.exe", "mysql/bin/mysqld.exe", "QUICKSTART.md", "DATA_EXTRACTION.md",
            "dump/wl_auth.sql", "dump/wl_characters.sql", "dump/wl_playerbots.sql", "dump/wl_world.sql",
            "REPACK_MANIFEST.json", "SHA256SUMS.txt",
        }
        errors.extend("missing: " + name for name in sorted(required - relative_files))
        launchers = {name for name in relative_files if "/" not in name and name.endswith(".bat")}
        if launchers != {"1_MYSQL.bat", "2_AUTHSERVER.bat", "3_WORLDSERVER.bat"}:
            errors.append("expected exactly three server launchers")
        manifest = json.loads(archive.read(prefix + "REPACK_MANIFEST.json"))
        rows = manifest["files"]
        listed = set()
        for row in rows:
            relative = row["path"]
            if relative in listed or relative not in relative_files:
                errors.append("duplicate/missing manifest row: " + relative)
                continue
            listed.add(relative)
            content = archive.read(prefix + relative)
            if len(content) != row["size"] or hashlib.sha256(content).hexdigest() != row["sha256"].lower():
                errors.append("hash/size mismatch: " + relative)
        if relative_files - listed != {"REPACK_MANIFEST.json", "SHA256SUMS.txt"}:
            errors.append("unlisted payload files")
        checksums = {}
        for line in archive.read(prefix + "SHA256SUMS.txt").decode("utf-8-sig").splitlines():
            match = re.fullmatch(r"([0-9a-fA-F]{64})  (.+)", line)
            if not match or match[2] in checksums:
                errors.append("invalid/duplicate checksum row")
                continue
            checksums[match[2]] = match[1].lower()
        expected_checksums = {row["path"]: row["sha256"].lower() for row in rows}
        expected_checksums["REPACK_MANIFEST.json"] = hashlib.sha256(archive.read(prefix + "REPACK_MANIFEST.json")).hexdigest()
        if checksums != expected_checksums:
            errors.append("checksum inventory mismatch")
        config = archive.read(prefix + "configs/modules/playerbots.conf").decode("utf-8-sig")
        for key in ("MinRandomBots", "MaxRandomBots"):
            if not re.search(r"^AiPlayerbot\." + key + r"\s*=\s*2000\s*$", config, re.M):
                errors.append("shipped bot target mismatch")
        llm = archive.read(prefix + "configs/modules/mod_wowlegends.conf").decode("utf-8-sig")
        keys = re.findall(r"^WowLegends\.AiChat\.ApiKey\s*=\s*(.*)$", llm, re.M)
        if keys != ['""\r'] and keys != ['""']:
            errors.append("missing/duplicate/nonempty API key")
        settings = json.loads(archive.read(prefix + "scripts/settings.json"))
        if settings["RealmAddress"] != "auto" or settings["MySqlPort"] != 3306:
            errors.append("operator/test endpoint leaked into defaults")
        if manifest.get("version") == "1.1.1-rc.3":
            migration_hashes = {
                "01_equipment_notice_koKR.sql": "0a600c16cd3dca590255b0ae2234271de621454c52aaaa0924faf68f033c745d",
                "02_duration_custom_flags.sql": "68d9c92b86cfaf9a83434fbabe199ad1899368ab8d2c312b3d657c9c8abc7d1d",
                "03_eye_missing_path_idle.sql": "76d74f5f5c6d5312d4bd2699228058e1b4e407f41c3961b1d8149e42f048b800",
            }
            rc3_required = {"scripts/wer-migrate-rc3.ps1"}
            rc3_required.update("scripts/migrations/rc3/" + name for name in migration_hashes)
            rc3_required.update("scripts/migrations/rc3/" + name for name in (
                "rollback_equipment_notice_koKR.sql", "rollback_duration_custom_flags.sql",
                "rollback_eye_missing_path_idle.sql",
            ))
            errors.extend("missing RC3 migration: " + name for name in sorted(rc3_required - relative_files))
            for name, expected in migration_hashes.items():
                relative = "scripts/migrations/rc3/" + name
                if relative in relative_files and hashlib.sha256(archive.read(prefix + relative)).hexdigest() != expected:
                    errors.append("RC3 migration hash mismatch: " + name)
            if settings.get("MinFreeCommitMB", 0) < 16384:
                errors.append("RC3 free-commit safety threshold missing or lowered")
        for name in launchers:
            text = archive.read(prefix + name).decode("utf-8-sig")
            if "start /b" in text.lower() or "start-process" in text.lower():
                errors.append("background launcher")
        bad_crc = archive.testzip()
        if bad_crc:
            errors.append("CRC mismatch")
    for error in errors:
        print(error)
    print("{}: {} hashed payload files; source/client data excluded".format("FAIL" if errors else "PASS", len(listed)))
    return 1 if errors else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("Usage: python tools/verify-runnable-archive.py <runnable.zip>")
    try:
        sys.exit(verify(sys.argv[1]))
    except (ValueError, KeyError, zipfile.BadZipFile) as error:
        sys.exit("FAIL: " + str(error))
