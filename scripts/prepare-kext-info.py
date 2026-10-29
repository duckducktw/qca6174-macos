#!/usr/bin/env python3
"""Generate a legacy or Sequoia diagnostic personality without editing an EFI."""
import argparse
from pathlib import Path
import plistlib


def prepare(native, destination, diagnostic_stage=3):
    root = Path(__file__).resolve().parents[1]
    info = plistlib.loads((root / 'Ath10kMac.kext/Contents/Info.plist').read_bytes())
    if native:
        info['OSBundleLibraries'].update({
            'com.apple.iokit.IO80211Family': '1200.13.1',
            'com.apple.iokit.IOSkywalkFamily': '1.0',
            'com.apple.driver.corecapture': '1.0.0', 'com.apple.kec.corecrypto': '1.0',
        })
        personalities = {}
        for key, secondary, label in (
            ('QCA9377', '0x2b511a3b', 'Lunnen-QCA9377'),
            ('QCA6174', '0x080711ad', 'LiteOn-QCA6174'),
        ):
            personality = dict(info['IOKitPersonalities'][key])
            personality.update({
                'IOMatchCategory': 'WiFiDriver', 'IONetworkRootType': 'airport',
                'Ath10kNativeExperimental': True, 'IOPCISecondaryMatch': secondary,
                'Ath10kDiagnosticStage': diagnostic_stage, 'Ath10kProbeOnly': False,
            })
            personalities[label] = personality
        info['IOKitPersonalities'] = personalities
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(plistlib.dumps(info, sort_keys=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('native', type=int, choices=[0, 1])
    parser.add_argument('destination', type=Path)
    parser.add_argument('--diagnostic-stage', type=int, choices=[0, 1, 2, 3], default=3,
                        help='0 runs the full radio/native path; 1-3 are bounded diagnostics')
    args = parser.parse_args()
    prepare(bool(args.native), args.destination, args.diagnostic_stage)
