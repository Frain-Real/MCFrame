import io
import unittest
from contextlib import redirect_stdout
import mcflash


class PackTests(unittest.TestCase):
    def test_roundtrip(self):
        image = bytes(range(256)) * 4
        packed = mcflash.pack(image, 0x01020304)
        out = mcflash.unpack(packed)
        self.assertEqual(out.image, image)
        self.assertEqual(out.fw_version, 0x01020304)
        self.assertEqual(out.crc32, __import__("zlib").crc32(image) & 0xFFFFFFFF)

    def test_corrupt(self):
        packed = bytearray(mcflash.pack(b"abc", 1))
        packed[-1] ^= 0xFF
        with self.assertRaises(ValueError):
            mcflash.unpack(bytes(packed))

    def test_frame_roundtrip(self):
        raw = mcflash.frame(mcflash.OP_INFO, 7, b"\x01")
        self.assertEqual(raw[:2], b"OT")
        self.assertEqual(raw[2], mcflash.VER)
        self.assertEqual(raw[3], mcflash.OP_INFO)
        self.assertEqual(raw[5], 7)
        self.assertEqual(raw[9:10], b"\x01")


if __name__ == "__main__":
    unittest.main()
