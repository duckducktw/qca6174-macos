#!/usr/bin/env python3
import importlib.util
import plistlib
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("prepare_kext_info", ROOT / "scripts/prepare-kext-info.py")
if SPEC is None or SPEC.loader is None:
    raise RuntimeError("cannot load prepare-kext-info.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class NativePersonalityTests(unittest.TestCase):
    def test_native_profiles_are_exact_and_stage_is_preserved(self):
        for stage in (0, 3):
            with self.subTest(stage=stage), tempfile.TemporaryDirectory() as temp:
                target = Path(temp) / "Info.plist"
                MODULE.prepare(True, target, stage)
                info = plistlib.loads(target.read_bytes())
                personalities = info["IOKitPersonalities"]
                self.assertEqual(set(personalities), {"Lunnen-QCA9377", "LiteOn-QCA6174"})
                expected = {
                    "Lunnen-QCA9377": ("0x0042168C", "0x2b511a3b"),
                    "LiteOn-QCA6174": ("0x003E168C", "0x080711ad"),
                }
                for name, (primary, secondary) in expected.items():
                    profile = personalities[name]
                    self.assertEqual(profile["IOPCIMatch"], primary)
                    self.assertEqual(profile["IOPCISecondaryMatch"], secondary)
                    self.assertEqual(profile["Ath10kDiagnosticStage"], stage)
                    self.assertEqual(profile["IOMatchCategory"], "WiFiDriver")
                    self.assertEqual(profile["IONetworkRootType"], "airport")

    def test_legacy_profiles_are_unchanged(self):
        with tempfile.TemporaryDirectory() as temp:
            target = Path(temp) / "Info.plist"
            MODULE.prepare(False, target)
            info = plistlib.loads(target.read_bytes())
            self.assertIn("QCA6174", info["IOKitPersonalities"])
            self.assertIn("QCA9377", info["IOKitPersonalities"])
            self.assertNotIn("IOPCISecondaryMatch", info["IOKitPersonalities"]["QCA6174"])


if __name__ == "__main__":
    unittest.main()
