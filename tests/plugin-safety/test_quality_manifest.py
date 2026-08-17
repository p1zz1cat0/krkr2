import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
MANIFEST = ROOT / "tests" / "plugin-quality-gates.json"


class PluginQualityManifestTests(unittest.TestCase):
    def test_manifest_entries_are_complete_unique_and_resolvable(self):
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        self.assertEqual(1, document["schemaVersion"])
        required = {
            "name", "dll", "sourceRoots", "platforms", "buildTarget",
            "ctestLabel", "fixture", "marker", "timeoutSeconds", "anchor",
        }
        names = set()
        dlls = set()
        anchors = set()
        for entry in document["plugins"]:
            self.assertTrue(required <= entry.keys())
            self.assertNotIn(entry["name"], names)
            self.assertNotIn(entry["dll"].lower(), dlls)
            self.assertNotIn(entry["anchor"], anchors)
            names.add(entry["name"])
            dlls.add(entry["dll"].lower())
            anchors.add(entry["anchor"])
            self.assertGreater(entry["timeoutSeconds"], 0)
            self.assertEqual("plugin", entry["ctestLabel"])
            self.assertTrue(entry["marker"].startswith("PLUGIN_SMOKE_PASS plugin="))
            self.assertTrue((ROOT / entry["fixture"] / "startup.tjs").is_file())

            source_text = ""
            for source_root in entry["sourceRoots"]:
                path = ROOT / source_root
                self.assertTrue(path.is_dir(), source_root)
                source_text += "\n".join(
                    file.read_text(encoding="utf-8", errors="ignore")
                    for file in path.rglob("*") if file.suffix in {".c", ".cc", ".cpp", ".h", ".hpp"}
                )
            self.assertIn(entry["anchor"], source_text)

    def test_exclusions_are_narrow_and_auditable(self):
        document = json.loads(MANIFEST.read_text(encoding="utf-8"))
        for exclusion in document.get("sourceExclusions", []):
            self.assertIn(exclusion["classification"], {"third-party", "windows-only"})
            self.assertTrue(exclusion["path"].startswith("cpp/plugins/"))
            self.assertGreaterEqual(len(exclusion["reason"].split()), 3)


if __name__ == "__main__":
    unittest.main()
