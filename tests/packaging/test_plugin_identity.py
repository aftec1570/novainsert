"""Built-bundle identity: the studio name shown in hosts, the bundle id, and the VST3 class IDs that saved projects
resolve by. The class IDs must never change (they derive from PLUGIN_MANUFACTURER_CODE Nvin / PLUGIN_CODE Nvi1).
Run after a release build: python3 -I tests/packaging/test_plugin_identity.py [path/to/NovaInsert.vst3]"""
import os, pathlib, plistlib, re, sys, unittest
ROOT = pathlib.Path(__file__).resolve().parents[2]
BUNDLE = pathlib.Path(os.environ.get("NI_BUNDLE", ROOT / "build-release/src/NovaInsert_artefacts/Release/VST3/NovaInsert.vst3"))
VENDOR, BUNDLE_ID = "Hillside Studios", "com.hillsidestudios.NovaInsert"
VERSION = re.search(r'kVersion = "([^"]+)"', (ROOT / "src/engine/version.hpp").read_text()).group(1)
CIDS = {"ABCDEF019182FAEB4E76696E4E766931", "ABCDEF011234ABCD4E76696E4E766931"}   # shipped since v0.1.0 — never change

class PluginIdentity(unittest.TestCase):
    def setUp(self):
        self.info = (BUNDLE / "Contents/Resources/moduleinfo.json").read_text()   # JSON5 (trailing commas): read by regex
    def test_vendor_is_the_studio(self):
        self.assertEqual(set(re.findall(r'"Vendor": "([^"]*)"', self.info)), {VENDOR})
    def test_bundle_identifier(self):
        with open(BUNDLE / "Contents/Info.plist", "rb") as f: self.assertEqual(plistlib.load(f)["CFBundleIdentifier"], BUNDLE_ID)
    def test_bundle_version_is_the_release_version(self):
        self.assertEqual(VERSION, "1.0.1")
        with open(BUNDLE / "Contents/Info.plist", "rb") as f: info = plistlib.load(f)
        self.assertEqual((info["CFBundleShortVersionString"], info["CFBundleVersion"]), (VERSION, VERSION))
    def test_class_ids_unchanged_so_saved_projects_load(self):
        self.assertEqual(set(re.findall(r'"CID": "([0-9A-F]{32})"', self.info)), CIDS)
    def test_no_old_publisher_name_in_the_bundle(self):
        for p in BUNDLE.rglob("*"):
            if p.is_file(): self.assertNotIn(b"aftec1570", p.read_bytes(), str(p))

if __name__ == "__main__":
    if len(sys.argv) > 1: BUNDLE = pathlib.Path(sys.argv.pop(1))
    unittest.main()
