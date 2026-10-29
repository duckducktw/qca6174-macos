import importlib.util
import json
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("prepare", ROOT / "scripts/prepare-firmware.py")
p = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p)


def ie(tag, data):
    return struct.pack("<II", tag, len(data)) + data + b"x" * (-len(data) % 4)


def container(magic, data):
    return magic + b"m" * (-len(magic) % 4) + data


def board_entry(key, value):
    return ie(0, ie(0, key.encode()) + ie(1, value))


class PayloadTests(unittest.TestCase):
    def test_real_inputs(self):
        board = p.select_board((ROOT / "firmware/board-2.bin").read_bytes())
        self.assertEqual(len(board), 8124)
        parts, version = p.parse_firmware((ROOT / "firmware/firmware-6.bin").read_bytes())
        self.assertEqual(version, "WLAN.RM.4.4.1-00309-")
        self.assertEqual(len(parts[3]), 681821)
        self.assertEqual(len(parts[4]), 24429)
        self.assertTrue(p.ignore_otp_result(parts))
        self.assertFalse(p.supports_skip_clock_init(parts))
        segments, entry = p.parse_segments(parts[3])
        self.assertEqual(entry, 0xa0400)
        self.assertEqual(len(segments), 6)
        self.assertEqual(sum(s['length'] for s in segments), 681749)

    def test_liteon_qca6174_profile(self):
        files, header, entry = p.prepare_profile(
            ROOT, ROOT / 'firmware', 'Ath10kPayloadLiteOn',
            p.LITEON_QCA6174_BOARD_KEY, 'QCA6174/hw3.2-LiteOn-11ad-0807',
            ('0x003e', '0x11ad', '0x0807', '10', '0x05030000u'), False,
            'QCA6174-LiteOn')
        manifest = json.loads(files[ROOT / 'firmware/prepared/QCA6174-LiteOn/manifest.json'])
        self.assertEqual(manifest['board_key'], p.LITEON_QCA6174_BOARD_KEY)
        self.assertEqual(manifest['board_selection'], 'board-api2-exact')
        self.assertEqual(manifest['board_bytes'], 8124)
        self.assertIn('0x11ad', entry)
        self.assertIn('0x0807', entry)
        self.assertIn('Ath10kPayloadLiteOn', '\n'.join(header))
        self.assertNotEqual(
            p.select_board((ROOT / 'firmware/board-2.bin').read_bytes(), p.BOARD_KEY),
            p.select_board((ROOT / 'firmware/board-2.bin').read_bytes(), p.LITEON_QCA6174_BOARD_KEY))

    def test_qca9377_profile(self):
        folder = ROOT / 'firmware/QCA9377/hw1.0'
        with self.assertRaises(p.BoardNotFound):
            p.select_board((folder / 'board-2.bin').read_bytes(), p.QCA9377_BOARD_KEY)
        files, header, entry = p.prepare_profile(ROOT, folder, 'Ath10kPayload9377',
            p.QCA9377_BOARD_KEY, 'QCA9377/hw1.1',
            ('0x0042', '0x1235', '0x0901', '1', '0x05020001u'), True)
        import json
        manifest = json.loads(files[ROOT / 'firmware/prepared/QCA9377/manifest.json'])
        self.assertEqual(manifest['board_selection'], 'board-api1-fallback')
        self.assertEqual(manifest['firmware_version'], 'WLAN.TF.2.1-00021-QCARMSWP-1')
        self.assertEqual(manifest['otp_bytes'], 24193)
        self.assertEqual(manifest['firmware_bytes'], 727125)
        self.assertEqual(manifest['firmware_segment_data_bytes'], 727053)
        self.assertEqual(manifest['board_sha256'],
            '127d35d82edb46278f30c448cbca664d755ff0d5fed57b649959cdbc4208c768')

    def test_fallback_rejects_corrupt_container_and_foreign_firmware(self):
        import tempfile
        import shutil
        folder = ROOT / 'firmware/QCA9377/hw1.0'
        with tempfile.TemporaryDirectory() as temp:
            tmp = Path(temp)
            for name in ('board-2.bin', 'board.bin', 'firmware-6.bin'):
                shutil.copyfile(folder / name, tmp / name)
            args = (ROOT, tmp, 'Ath10kPayload9377', p.QCA9377_BOARD_KEY, 'QCA9377/hw1.1',
                ('0x0042', '0x1235', '0x0901', '1', '0x05020001u'), True)
            bad = [b'corrupt', container(p.BOARD_MAGIC, board_entry(p.QCA9377_BOARD_KEY,b'a'*8124)*2),
                   container(p.BOARD_MAGIC,board_entry(p.QCA9377_BOARD_KEY,b'a'*8000))]
            for data in bad:
                (tmp/'board-2.bin').write_bytes(data)
                with self.assertRaises(ValueError): p.prepare_profile(*args)
            shutil.copyfile(folder/'board-2.bin',tmp/'board-2.bin')
            (tmp/'board.bin').write_bytes((ROOT/'firmware/prepared/board.bin').read_bytes())
            with self.assertRaises(ValueError): p.prepare_profile(*args)
            shutil.copyfile(folder/'board.bin',tmp/'board.bin')
            shutil.copyfile(ROOT/'firmware/firmware-6.bin',tmp/'firmware-6.bin')
            with self.assertRaises(ValueError): p.prepare_profile(*args)

    def test_segmented_image_validation(self):
        header = struct.pack('<II', 0x544d4753, 0)
        segment = struct.pack('<II', 0x500000, 5) + b'abcde'
        entry = struct.pack('<II', 0x500000, 0xfffffffd)
        done = struct.pack('<II', 0, 0xffffffff)
        self.assertEqual(p.parse_segments(header + segment + entry + done)[0][0]['length'], 5)
        invalid = [b'', header + segment, header + segment + done,
                   header + segment + entry + done + b'x',
                   header + segment + entry + entry + done,
                   header + struct.pack('<II', 0x500000, 0xfffffffc),
                   header + struct.pack('<II', 0x500000, 1000) + b'abc',
                   header + segment + segment + entry + done,
                   header + struct.pack('<II', 0xfffffffc, 5) + b'abcde' + entry + done,
                   header + struct.pack('<II', 0x400800, 5) + b'abcde' + entry + done,
                   header + segment + struct.pack('<II', 0x600000, 0xfffffffd) + done]
        for data in invalid:
            with self.subTest(size=len(data)), self.assertRaises(ValueError):
                p.parse_segments(data)

    def test_otp_feature_bit(self):
        self.assertFalse(p.ignore_otp_result({}))
        self.assertFalse(p.ignore_otp_result({2: b""}))
        self.assertFalse(p.ignore_otp_result({2: b"\x40\x80"}))
        self.assertTrue(p.ignore_otp_result({2: b"\x80"}))

    def test_skip_clock_feature_bit(self):
        for value in (b"", b"\x02", b"\xc0\x10"):
            self.assertFalse(p.supports_skip_clock_init({2: value}))
        self.assertFalse(p.supports_skip_clock_init({}))
        self.assertTrue(p.supports_skip_clock_init({2: b"\xc0\x12"}))

    def test_exact_not_prefix_match(self):
        data = container(p.BOARD_MAGIC,
            board_entry(p.BOARD_KEY + ",variant=other", b"b" * 8124) +
            board_entry(p.BOARD_KEY, b"a" * 8124))
        self.assertEqual(p.select_board(data), b"a" * 8124)

    def test_missing_duplicate_wrong_size(self):
        entries = [board_entry("other", b"a" * 8124),
                   board_entry(p.BOARD_KEY, b"a" * 8124) * 2,
                   board_entry(p.BOARD_KEY, b"a" * 8123)]
        for entry in entries:
            with self.subTest(size=len(entry)), self.assertRaises(ValueError):
                p.select_board(container(p.BOARD_MAGIC, entry))

    def test_truncated_and_hostile_lengths(self):
        for data in (b"", p.BOARD_MAGIC, container(p.BOARD_MAGIC, b"a"),
                     container(p.BOARD_MAGIC, struct.pack("<II", 0, 0xffffffff)),
                     container(p.BOARD_MAGIC, ie(0, struct.pack("<II", 1, 8000)))):
            with self.subTest(data=data[:25]), self.assertRaises(ValueError):
                p.select_board(data)

    def test_firmware_rejects_missing_duplicate_codeswap_wrong_ops(self):
        good = ie(0, b"version") + ie(3, b"firmware") + ie(4, b"otp") + ie(5, struct.pack("<I", 4)) + ie(6, struct.pack("<I", 3))
        for body in (good + ie(3, b"second"), good + ie(7, b"swap"),
                     ie(0, b"version"), good.replace(struct.pack("<I", 4), struct.pack("<I", 9))):
            with self.subTest(size=len(body)), self.assertRaises(ValueError):
                p.parse_firmware(container(p.FW_MAGIC, body))


if __name__ == "__main__":
    unittest.main()
