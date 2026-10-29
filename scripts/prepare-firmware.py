#!/usr/bin/env python3
"""Validate ath10k containers; select per-device board/firmware profiles.

Formats: Linux v6.12 ath10k/hw.h and core.c. No network or device access.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

BOARD_MAGIC = b"QCA-ATH10K-BOARD\0"
FW_MAGIC = b"QCA-ATH10K\0"
BOARD_KEY = "bus=pci,vendor=168c,device=003e,subsystem-vendor=1028,subsystem-device=0310"
LITEON_QCA6174_BOARD_KEY = "bus=pci,vendor=168c,device=003e,subsystem-vendor=11ad,subsystem-device=0807"
BOARD_SIZE = 8124
QCA9377_BOARD_KEY = "bus=pci,vendor=168c,device=0042,subsystem-vendor=1235,subsystem-device=0901"

class BoardNotFound(ValueError):
    """Only a well-formed API2 container without a match allows API1 fallback."""



def ies(data, magic=None):
    pos = 0
    if magic is not None:
        if not data.startswith(magic):
            raise ValueError("incorrect container magic")
        pos = (len(magic) + 3) & ~3
        if len(data) < pos:
            raise ValueError("truncated magic padding")
    while pos < len(data):
        if len(data) - pos < 8:
            raise ValueError("truncated IE header")
        tag, size = struct.unpack_from("<II", data, pos)
        pos += 8
        padded = (size + 3) & ~3
        if padded > len(data) - pos:
            raise ValueError("IE length exceeds container")
        yield tag, data[pos:pos + size]
        pos += padded


def select_board(data, key=BOARD_KEY):
    matches = []
    for tag, block in ies(data, BOARD_MAGIC):
        if tag != 0:  # normal board, not extended-board IE
            continue
        names = []
        for subtag, value in ies(block):
            if subtag == 0:
                names.append(value)
            elif subtag == 1:
                if key.encode("ascii") in names:
                    matches.append(value)
                names = []
    if not matches:
        raise BoardNotFound("no exact board match")
    if len(matches) != 1:
        raise ValueError(f"expected one exact board match; found {len(matches)}")
    if len(matches[0]) != BOARD_SIZE:
        raise ValueError(f"unsupported board size: {len(matches[0])}")
    return matches[0]


def parse_firmware(data):
    parts = {}
    for tag, value in ies(data, FW_MAGIC):
        if tag in parts:
            raise ValueError(f"duplicate firmware IE {tag}")
        parts[tag] = value
    for tag in (0, 3, 4, 5, 6):
        if not parts.get(tag):
            raise ValueError(f"missing/empty firmware IE {tag}")
    for tag in (5, 6):
        if len(parts[tag]) != 4:
            raise ValueError(f"bad operation-version size for IE {tag}")
    wmi, htt = (struct.unpack("<I", parts[tag])[0] for tag in (5, 6))
    if (wmi, htt) != (4, 3):
        raise ValueError("expected TLV WMI=4 / HTT=3")
    if 7 in parts:
        raise ValueError("code-swap firmware is not supported in this milestone")
    version = parts[0].rstrip(b"\0").decode("ascii")
    if not version or any(ord(c) < 32 or ord(c) > 126 for c in version):
        raise ValueError("invalid firmware version string")
    return parts, version


def sha(data):
    return hashlib.sha256(data).hexdigest()


def ignore_otp_result(parts):
    # Linux v6.12 core.h: ATH10K_FW_FEATURE_IGNORE_OTP_RESULT = 7.
    features = parts.get(2, b"")
    return bool(features and (features[0] & 0x80))


def supports_skip_clock_init(parts):
    # Linux v6.12 core.h: ATH10K_FW_FEATURE_SUPPORTS_SKIP_CLOCK_INIT = 9.
    features = parts.get(2, b"")
    return len(features) > 1 and bool(features[1] & 0x02)


def parse_segments(image):
    """Uncompressed SGMT image, with data + one BEGINADDR + terminal DONE.

    Linux bmi.h/hw.c define this envelope. EXEC/BDDATA are deliberately
    rejected: this milestone downloads code without executing WLAN firmware.
    Offsets refer to the original image (metadata is not sent as raw RAM).
    """
    if len(image) < 8 or len(image) > 2 * 1024 * 1024:
        raise ValueError("unsupported SGMT image size")
    if struct.unpack_from("<II", image) != (0x544d4753, 0):
        raise ValueError("expected uncompressed SGMT firmware image")
    pos, entry, segments = 8, None, []
    while pos < len(image):
        if len(image) - pos < 8:
            raise ValueError("truncated SGMT metadata")
        address, size = struct.unpack_from("<II", image, pos)
        pos += 8
        if size == 0xffffffff:
            if pos != len(image) or entry is None or not segments:
                raise ValueError("invalid SGMT end / missing entry point")
            if not any(s['address'] <= entry < s['address'] + s['length'] for s in segments):
                raise ValueError("SGMT entry point is outside data segments")
            return segments, entry
        if size == 0xfffffffd:
            if entry is not None or (address & 3):
                raise ValueError("duplicate/unaligned SGMT entry point")
            entry = address
            continue
        if size & 0x80000000:
            raise ValueError("unsupported SGMT EXEC/BDDATA/metadata")
        if entry is not None or size == 0 or size > len(image) - pos:
            raise ValueError("invalid SGMT data length/order")
        # Rounded-up tail reads must also fit the target's 32-bit address space.
        if address & 3 or address + ((size + 3) & ~3) > 0x100000000:
            raise ValueError("invalid SGMT target address range")
        if len(segments) >= 64:
            raise ValueError("too many SGMT segments")
        if any(address < s['address'] + s['length'] and s['address'] < address + size for s in segments):
            raise ValueError("overlapping SGMT segments cannot be verified independently")
        # Protect the boot configuration and board area used by this milestone.
        if address < 0x404000 and address + size > 0x400800:
            raise ValueError("SGMT data overlaps Host Interest / board RAM")
        segments.append({'address': address, 'offset': pos, 'length': size})
        pos += size
    raise ValueError("SGMT DONE marker missing")


def prepare_profile(root, folder, namespace, board_key, profile, identity, allow_fallback=False,
                    output_name=None):
    board_container = (folder / "board-2.bin").read_bytes()
    fw_container = (folder / "firmware-6.bin").read_bytes()
    selection = "board-api2-exact"
    try:
        board = select_board(board_container, board_key)
    except BoardNotFound:
        if not allow_fallback:
            raise
        board = (folder / "board.bin").read_bytes()
        if len(board) != BOARD_SIZE:
            raise ValueError("invalid API1 fallback board size")
        selection = "board-api1-fallback"

    parts, version = parse_firmware(fw_container)
    if allow_fallback:
        if sha(fw_container) != "8f8b002fccfe81d42238f27dd1f56d189604f180bd4772c7c8e75ae1fef16f01":
            raise ValueError("QCA9377 firmware does not match the reviewed profile")
        if selection == "board-api1-fallback" and sha(board) != "127d35d82edb46278f30c448cbca664d755ff0d5fed57b649959cdbc4208c768":
            raise ValueError("QCA9377 fallback board does not match the reviewed profile")
    segments, entry = parse_segments(parts[3])
    manifest = {
        "profile": profile, "board_key": board_key, "board_selection": selection,
        "board_bytes": len(board), "board_sha256": sha(board),
        "board_container_sha256": sha(board_container),
        "firmware_container_sha256": sha(fw_container),
        "firmware_version": version, "firmware_bytes": len(parts[3]),
        "firmware_sha256": sha(parts[3]), "otp_bytes": len(parts[4]),
        "otp_sha256": sha(parts[4]), "wmi_op": 4, "htt_op": 3,
        "otp_ignore_result_feature": ignore_otp_result(parts),
        "supports_skip_clock_init_feature": supports_skip_clock_init(parts),
        "firmware_segments": segments, "firmware_entry_address": entry,
        "firmware_segment_data_bytes": sum(s['length'] for s in segments),
        "runtime_policy": "Host Interest + board + OTP + WLAN readback + BMI_DONE + HTC READY + WMI/HTT runtime; verified reset on failure",
    }
    header = ["// Generated by scripts/prepare-firmware.py. Do not edit.",
              "#pragma once", "#include <IOKit/IOTypes.h>", f"namespace {namespace} {{",
              f"static constexpr char kBoardKey[] = {json.dumps(board_key)};",
              f"static constexpr char kBoardSHA256[] = {json.dumps(sha(board))};",
              f"static constexpr char kFirmwareVersion[] = {json.dumps(version)};",
              f"static constexpr UInt32 kFirmwareSize = {len(parts[3])};",
              f"static constexpr char kFirmwareSHA256[] = {json.dumps(sha(parts[3]))};",
              f"static constexpr UInt32 kOTPSize = {len(parts[4])};",
              f"static constexpr char kOTPSHA256[] = {json.dumps(sha(parts[4]))};",
              f"static constexpr bool kOTPIgnoreResult = {str(ignore_otp_result(parts)).lower()};",
              f"static constexpr bool kSupportsSkipClockInit = {str(supports_skip_clock_init(parts)).lower()};",
              f"static constexpr char kBoardSelection[] = {json.dumps(selection)};",
              "static constexpr UInt8 kBoard[] = {"]
    for pos in range(0, len(board), 16):
        header.append("    " + ", ".join(f"0x{x:02x}" for x in board[pos:pos + 16]) + ",")
    header += ["};", "static_assert(sizeof(kBoard) == 8124, \"QCA6174 board size\");",
               "static constexpr UInt8 kOTP[] = {"]
    for pos in range(0, len(parts[4]), 16):
        header.append("    " + ", ".join(f"0x{x:02x}" for x in parts[4][pos:pos + 16]) + ",")
    header += ["};", "static_assert(sizeof(kOTP) == kOTPSize, \"OTP payload size\");",
               "static constexpr UInt8 kFirmware[] = {"]
    for pos in range(0, len(parts[3]), 16):
        header.append("    " + ", ".join(f"0x{x:02x}" for x in parts[3][pos:pos + 16]) + ",")
    header += ["};", "static_assert(sizeof(kFirmware) == kFirmwareSize, \"WLAN payload size\");",
               "// FirmwareSegment is defined by Ath10kPayloadProfile.hpp.",
               "static constexpr Ath10kPayload::FirmwareSegment kFirmwareSegments[] = {"]
    for s in segments:
        header.append(f"    {{0x{s['address']:08x}u, {s['offset']}u, {s['length']}u}},")
    header += ["};", f"static constexpr UInt32 kFirmwareSegmentCount = {len(segments)};",
               f"static constexpr UInt32 kFirmwareSegmentDataBytes = {sum(s['length'] for s in segments)};",
               f"static constexpr UInt32 kFirmwareEntryAddress = 0x{entry:08x}u;", "}", ""]
    outdir = root / "firmware/prepared" / (output_name if output_name is not None else ("QCA9377" if allow_fallback else ""))
    outputs = {
        outdir / "board.bin": board,
        outdir / "otp.bin": parts[4],
        outdir / "firmware-image.bin": parts[3],
        outdir / "manifest.json": (json.dumps(manifest, indent=2) + "\n").encode(),
    }
    dev, subvendor, subdevice, chip, target = identity
    profile_line = (f"    {{{dev}, {subvendor}, {subdevice}, {chip}, {target}, "
        f"true, {namespace}::kBoardKey, {namespace}::kBoardSelection, "
        f"{namespace}::kBoardSHA256, {namespace}::kBoard, sizeof({namespace}::kBoard), "
        f"{namespace}::kFirmwareVersion, {namespace}::kFirmwareSize, {namespace}::kFirmwareSHA256, "
        f"{namespace}::kFirmware, {namespace}::kOTPSize, {namespace}::kOTPSHA256, {namespace}::kOTP, "
        f"{namespace}::kOTPIgnoreResult, {namespace}::kSupportsSkipClockInit, "
        f"{namespace}::kFirmwareSegments, {namespace}::kFirmwareSegmentCount, "
        f"{namespace}::kFirmwareSegmentDataBytes, {namespace}::kFirmwareEntryAddress}},")
    print(f"Prepared {profile}: {selection} ({len(board)} bytes); firmware {version}; OTP {len(parts[4])} bytes")
    return outputs, header, profile_line


def prepare(root):
    outputs = {}
    header = ['// Generated by scripts/prepare-firmware.py. Do not edit.',
              '#pragma once', '#include "Ath10kPayloadProfile.hpp"']
    entries = []
    profiles = [
        (root / 'firmware', 'Ath10kPayload', BOARD_KEY, 'QCA6174/hw3.2-Dell-1028-0310',
         ('0x003e', '0x1028', '0x0310', '10', '0x05030000u'), False),
        (root / 'firmware', 'Ath10kPayloadLiteOn', LITEON_QCA6174_BOARD_KEY,
         'QCA6174/hw3.2-LiteOn-11ad-0807',
         ('0x003e', '0x11ad', '0x0807', '10', '0x05030000u'), False,
         'QCA6174-LiteOn'),
        (root / 'firmware/QCA9377/hw1.0', 'Ath10kPayload9377', QCA9377_BOARD_KEY, 'QCA9377/hw1.1',
         ('0x0042', '0x1235', '0x0901', '1', '0x05020001u'), True),
    ]
    for args in profiles:
        files, lines, entry = prepare_profile(root, *args)
        outputs.update(files)
        header.extend(lines)
        entries.append(entry)
    header += ['namespace Ath10kPayload {', 'static constexpr Profile kProfiles[] = {', *entries, '};',
               'static constexpr Profile kUnavailable = {};',
               'const Profile &selectProfile(UInt16 vendor, UInt16 device, UInt16 subvendor, UInt16 subdevice, UInt8 chip) {',
               '    if (vendor == 0x168c) for (const auto &p : kProfiles)',
               '        if (p.device == device && p.subvendor == subvendor && p.subdevice == subdevice && p.chip == chip) return p;',
               '    return kUnavailable;', '}', '}']
    outputs[root / 'src/kext/Ath10kPayload.hpp'] = ('\n'.join(header) + '\n').encode()
    # All profiles validated before any output is written; build aborts on failure.
    for path, content in outputs.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists() and path.read_bytes() == content:
            continue
        tmp = path.with_name(path.name + '.tmp')
        tmp.write_bytes(content)
        tmp.replace(path)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    try:
        prepare(args.root)
    except (OSError, ValueError) as error:
        print(f"Payload preparation FAILED: {error}", file=sys.stderr)
        sys.exit(1)
