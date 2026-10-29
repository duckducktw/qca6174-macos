#!/usr/bin/env python3
"""Focused portable regressions; not a substitute for a macOS kext/hardware test."""
import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', type=Path, help='Zig executable for Windows host tests')
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    args = parser.parse_args()
    out = ROOT / 'build/development-tests'
    out.mkdir(parents=True, exist_ok=True)
    passed = []
    def run(name, command):
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, errors='replace')
        (out / (name + '.log')).write_text(result.stdout + result.stderr, encoding='utf-8')
        if result.returncode:
            raise RuntimeError(f'{name} failed; see {out / (name + ".log")}')
        print('PASS', name, flush=True)
        passed.append(name)
    run('prepare-firmware', [sys.executable, 'scripts/prepare-firmware.py'])
    run('payload-python', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_payload.py', '-v'])
    run('diagnostic-privacy', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_native_preflight.py', '-v'])
    run('native-personality', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_native_personality.py', '-v'])
    if args.zig:
        compiler = [str(args.zig.resolve()), 'c++', '-target', 'x86_64-windows-gnu']
    else:
        compiler = shlex.split(args.cxx)
    flags = ['-std=c++17', '-O2', '-UNDEBUG', '-Wall', '-Wextra', '-Werror', '-Wno-nullability-completeness']
    if os.name == 'nt':
        flags += ['-include', str(ROOT / 'tests/bsd_memory_compat.hpp')]
    with tempfile.TemporaryDirectory(prefix='ath10k-tests-') as tmp:
        include = Path(tmp)
        (include / 'IOKit').mkdir()
        (include / 'IOKit/IOTypes.h').write_text(
            '#pragma once\n#include <cstdint>\nusing UInt8=uint8_t;using UInt16=uint16_t;'
            'using UInt32=uint32_t;using UInt64=uint64_t;\n')
        for name in ['rsn_policy', 'lunnen_payload', 'liteon_qca6174_payload', 'security', 'native_wire', 'native_status', 'native_capture']:
            executable = out / ('test_' + name + ('.exe' if os.name == 'nt' else ''))
            libraries = ['-lbcrypt'] if name == 'security' and os.name == 'nt' else (
                ['-lcrypto'] if name == 'security' and sys.platform != 'darwin' else [])
            run(name + '-compile', compiler + flags + ['-I' + str(include), '-Isrc/kext', '-Ivendor/itlwm',
                'tests/test_' + name + '.cpp'] + libraries + ['-o', str(executable)])
            run(name, [str(executable)])
    (out / 'summary.json').write_text(json.dumps({
        'passed': passed, 'assertions_enabled': True,
        'macos_kext_build': False, 'hardware_validation': False,
        'native_menu_implemented': False, 'sae_pmf_implemented': False,
    }, indent=2) + '\n')

if __name__ == '__main__':
    main()
