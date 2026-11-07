"""Negative and positive tests for the migration ledger checker."""

import copy
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "check_migration", ROOT / "tools/check_migration.py"
)
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)
DOCUMENT = json.loads((ROOT / "docs/migration-assets.json").read_text())


class MigrationChecks(unittest.TestCase):
    def setUp(self):
        self.data = copy.deepcopy(DOCUMENT)

    def rejected(self, message, sources=False):
        with self.assertRaisesRegex(ValueError, message):
            CHECK.validate(self.data, ROOT, sources)

    def test_current_metadata(self):
        self.assertEqual(
            CHECK.validate(self.data, ROOT),
            (len(DOCUMENT["source_files"]), len(DOCUMENT["assets"])),
        )

    def test_duplicate_asset(self):
        self.data["assets"].append(copy.deepcopy(self.data["assets"][0]))
        self.rejected("Duplicate asset ID")

    def test_missing_asset(self):
        self.data["source_files"][0]["asset_id"] = "MISSING"
        self.rejected("Missing asset")

    def test_missing_batch_dependency(self):
        self.data["batches"][1]["depends_on"] = ["MISSING"]
        self.rejected("Missing batch dependency")

    def test_dependency_cycle(self):
        self.data["batches"][0]["depends_on"] = ["R01"]
        self.rejected("Dependency cycle")

    def test_unfinished_prerequisite(self):
        self.data["batches"][2]["status"] = "in_progress"
        self.rejected("Unfinished prerequisite")

    def test_missing_target(self):
        asset = self.data["assets"][-1]
        asset["status"] = "migrated"
        asset["target_paths"] = ["missing/new.c"]
        self.rejected("Missing target/evidence file")

    def test_unreviewed_migrated_group(self):
        self.data["assets"][0]["status"] = "migrated"
        self.rejected("Unreviewed source")

    def test_no_pass_without_evidence(self):
        self.data["assets"][-1]["verification"]["state"] = "passed"
        self.rejected("Missing passed evidence")

    def test_changed_baseline(self):
        self.data["repositories"][0]["commit"] = "0" * 40
        self.rejected("Pinned commit changed")

    def test_gitlink_mismatch(self):
        file = next(
            f
            for f in self.data["source_files"]
            if f["path"] == "agent" and f["repository"] == "legacy"
        )
        file["git_object"] = "0" * 40
        self.rejected("Forge gitlink mismatch")

    def test_source_selector_mismatch(self):
        file = self.data["source_files"][0]
        asset = next(a for a in self.data["assets"] if a["id"] == file["asset_id"])
        asset["source_selector"] = {"kind": "exact", "path": "missing.c"}
        self.rejected("Source selector mismatch")

    def test_target_escape(self):
        with self.assertRaisesRegex(ValueError, "Unsafe path"):
            CHECK.local_file(ROOT, "../entelea-legacy/LICENSE")

    def test_symlink_escape(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "outside").symlink_to(ROOT / "README.md")
            with self.assertRaisesRegex(ValueError, "Escaping path"):
                CHECK.local_file(root, "outside")

    def test_current_sources(self):
        self.assertEqual(
            CHECK.validate(self.data, ROOT, sources=True),
            (len(DOCUMENT["source_files"]), len(DOCUMENT["assets"])),
        )

    def test_digest_mismatch(self):
        file = next(f for f in self.data["source_files"] if f["git_type"] == "blob")
        file["sha256"] = "0" * 64
        self.rejected("Source digest mismatch", sources=True)

    def test_source_identity_mismatch(self):
        file = next(f for f in self.data["source_files"] if f["git_type"] == "blob")
        file["git_object"] = "0" * 40
        self.rejected("Source identity mismatch", sources=True)

    def test_incomplete_coverage(self):
        file = self.data["source_files"].pop()
        asset = next(a for a in self.data["assets"] if a["id"] == file["asset_id"])
        asset["source_file_count"] -= 1
        self.rejected("Source coverage mismatch", sources=True)

    def test_missing_migration_commit(self):
        file = self.data["source_files"][0]
        file["disposition"] = "migrate"
        file["target_paths"] = ["LICENSE"]
        self.rejected("Missing migration commit")

    def test_missing_decision_record(self):
        self.data["design_decisions"][0]["status"] = "approved"
        self.data["design_decisions"][0]["record_path"] = "missing/approval.md"
        self.rejected("Missing target/evidence file")

    def test_new_asset_with_fake_source(self):
        asset = self.data["assets"][-1]
        asset["source_selector"] = {"kind": "exact", "path": "memory.c"}
        self.rejected("New asset has old source")


if __name__ == "__main__":
    unittest.main()
