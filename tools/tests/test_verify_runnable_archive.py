import contextlib
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile


SPEC = importlib.util.spec_from_file_location("runtime_verifier", Path(__file__).parents[1] / "verify-runnable-archive.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class RuntimeArchiveTests(unittest.TestCase):
    def run_fixture(self, mutation=None, bad_checksums=False, version=None):
        names = [
            "authserver.exe", "worldserver.exe", "map_extractor.exe", "vmap4_extractor.exe",
            "vmap4_assembler.exe", "mmaps_generator.exe", "mmaps-config.yaml",
            "1_MYSQL.bat", "2_AUTHSERVER.bat", "3_WORLDSERVER.bat",
            "scripts/wer-console-prepare.ps1", "scripts/settings.json",
            "mysql/bin/mysql.exe", "mysql/bin/mysqld.exe", "QUICKSTART.md", "DATA_EXTRACTION.md",
            "dump/wl_auth.sql", "dump/wl_characters.sql", "dump/wl_playerbots.sql", "dump/wl_world.sql",
        ]
        payload = {name: b"fixture" for name in names}
        payload["scripts/settings.json"] = b'{"RealmAddress":"auto","MySqlPort":3306}'
        payload["configs/modules/playerbots.conf"] = b"AiPlayerbot.MinRandomBots = 2000\nAiPlayerbot.MaxRandomBots = 2000\n"
        payload["configs/modules/mod_wowlegends.conf"] = b'WowLegends.AiChat.ApiKey = ""\n'
        if mutation:
            mutation(payload)
        rows = [{"path": name, "size": len(content), "sha256": hashlib.sha256(content).hexdigest()} for name, content in sorted(payload.items())]
        payload["REPACK_MANIFEST.json"] = json.dumps({"files": rows, "version": version}).encode()
        checksum_text = "".join(hashlib.sha256(content).hexdigest() + "  " + name + "\n" for name, content in sorted(payload.items()))
        payload["SHA256SUMS.txt"] = ("invalid" if bad_checksums else checksum_text).encode()
        with tempfile.TemporaryDirectory() as temporary:
            archive_path = Path(temporary) / "test.zip"
            with zipfile.ZipFile(archive_path, "w") as archive:
                for name, content in payload.items():
                    archive.writestr("WER_TEST/" + name, content)
            with contextlib.redirect_stdout(io.StringIO()):
                return MODULE.verify(archive_path)

    def test_clean_payload(self):
        self.assertEqual(self.run_fixture(), 0)

    def test_client_data_is_rejected(self):
        self.assertEqual(self.run_fixture(lambda p: p.update({"data/dbc/Spell.dbc": b"WDBC"})), 1)

    def test_source_is_rejected(self):
        self.assertEqual(self.run_fixture(lambda p: p.update({"source/main.cpp": b"source"})), 1)

    def test_loose_source_and_client_data_are_rejected(self):
        for name in ("Spell.dbc", "backup/tile.mmtile", "main.cpp", "backup/model.m2"):
            with self.subTest(name=name):
                self.assertEqual(self.run_fixture(lambda p: p.update({name: b"excluded"})), 1)

    def test_unsafe_member_is_rejected(self):
        for name in ("../escaped.txt", "nested/../../escaped.txt", "C:/escaped.txt", "nested\\escaped.txt"):
            with self.subTest(name=name):
                self.assertEqual(self.run_fixture(lambda p: p.update({name: b"unsafe"})), 1)

    def test_nonempty_api_key_is_rejected(self):
        self.assertEqual(self.run_fixture(lambda p: p.update({"configs/modules/mod_wowlegends.conf": b'WowLegends.AiChat.ApiKey = "fixture-only"\n'})), 1)

    def test_test_bot_target_is_rejected(self):
        self.assertEqual(self.run_fixture(lambda p: p.update({"configs/modules/playerbots.conf": b"AiPlayerbot.MinRandomBots = 20\nAiPlayerbot.MaxRandomBots = 20\n"})), 1)

    def test_bad_checksums_are_rejected(self):
        self.assertEqual(self.run_fixture(bad_checksums=True), 1)

    @staticmethod
    def rc3_payload(payload):
        package = Path(__file__).parents[2] / "source/packaging/runtime_rc2"
        payload["scripts/wer-migrate-rc3.ps1"] = (package / "wer-migrate-rc3.ps1").read_bytes()
        for path in (package / "migrations/rc3").glob("*.sql"):
            payload["scripts/migrations/rc3/" + path.name] = path.read_bytes()
        payload["scripts/settings.json"] = b'{"RealmAddress":"auto","MySqlPort":3306,"MinFreeCommitMB":16384}'

    def test_complete_rc3_payload(self):
        self.assertEqual(self.run_fixture(self.rc3_payload, version="1.1.1-rc.3"), 0)

    def test_rc3_missing_migrations_rejected(self):
        self.assertEqual(self.run_fixture(version="1.1.1-rc.3"), 1)

    def test_rc3_tampered_sql_rejected(self):
        def altered(payload):
            self.rc3_payload(payload)
            payload["scripts/migrations/rc3/01_equipment_notice_koKR.sql"] += b"-- tampered"
        self.assertEqual(self.run_fixture(altered, version="1.1.1-rc.3"), 1)

    def test_rc3_lowered_commit_guard_rejected(self):
        def altered(payload):
            self.rc3_payload(payload)
            payload["scripts/settings.json"] = b'{"RealmAddress":"auto","MySqlPort":3306,"MinFreeCommitMB":8192}'
        self.assertEqual(self.run_fixture(altered, version="1.1.1-rc.3"), 1)


if __name__ == "__main__":
    unittest.main()
