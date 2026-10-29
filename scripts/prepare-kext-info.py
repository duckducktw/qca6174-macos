#!/usr/bin/env python3
"""Generate a legacy or Sequoia native personality without editing an EFI."""
import argparse
from pathlib import Path
import plistlib


def prepare(native, destination, diagnostic_stage=0, p256_diagnostic=False):
    if diagnostic_stage not in range(5):
        raise ValueError('unknown diagnostic stage')
    if p256_diagnostic != (diagnostic_stage == 4) or (p256_diagnostic and not native):
        raise ValueError('stage 4 requires native build and explicit P-256 diagnostic compilation')
    root = Path(__file__).resolve().parents[1]
    info = plistlib.loads((root / 'Ath10kMac.kext/Contents/Info.plist').read_bytes())
    if native:
        info['OSBundleLibraries'].update({
            'com.apple.iokit.IO80211Family': '1200.13.1',
            'com.apple.iokit.IOSkywalkFamily': '1.0',
            'com.apple.driver.corecapture': '1.0.0', 'com.apple.kec.corecrypto': '1.0',
        })
        personality = info['IOKitPersonalities']['QCA9377']
        personality.update({
            'IOMatchCategory': 'WiFiDriver', 'IONetworkRootType': 'airport',
            'Ath10kNativeExperimental': True, 'IOPCISecondaryMatch': '0x2b511a3b',
            'Ath10kDiagnosticStage': diagnostic_stage, 'Ath10kProbeOnly': False, 'Ath10kStationDFS': True,
        })
        liteon = dict(personality)
        liteon.update({'IOPCIMatch': '0x003e168c', 'IOPCISecondaryMatch': '0x080711ad'})
        if p256_diagnostic:
            info['OSBundleLibraries'] = {key: info['OSBundleLibraries'][key] for key in (
                'com.apple.iokit.IOPCIFamily', 'com.apple.kpi.iokit', 'com.apple.kpi.libkern',
                'com.apple.kpi.mach', 'com.apple.kec.corecrypto')}
            personality.update({'IOClass': 'Ath10kP256DiagnosticService', 'Ath10kP256SelfTest': True})
            personality.pop('IONetworkRootType', None)
            personality.pop('Ath10kNativeExperimental', None)
        info['IOKitPersonalities'] = {'Lunnen-QCA9377': personality}
        if not p256_diagnostic:
            info['IOKitPersonalities']['LiteOn-QCA6174'] = liteon
    destination = Path(destination)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(plistlib.dumps(info, sort_keys=False))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('native', type=int, choices=[0, 1])
    parser.add_argument('destination', type=Path)
    parser.add_argument('--diagnostic-stage', type=int, choices=[0, 1, 2, 3, 4], default=0)
    parser.add_argument('--p256-diagnostic', action='store_true')
    args = parser.parse_args()
    prepare(bool(args.native), args.destination, args.diagnostic_stage, args.p256_diagnostic)
