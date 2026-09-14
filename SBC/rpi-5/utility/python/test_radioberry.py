# SPDX-License-Identifier: GPL-2.0-only
"""Exercise packing and original-driver ABI without opening a real device."""
import struct
import unittest
from unittest.mock import patch

from radioberry import Radioberry


class WrapperTests(unittest.TestCase):
    def setUp(self):
        self.open = patch("radioberry.os.open", return_value=7).start()
        self.close = patch("radioberry.os.close").start()
        self.write = patch("radioberry.os.write", side_effect=lambda fd, p: len(p)).start()
        self.ioctl = patch("radioberry.fcntl.ioctl").start()
        self.addCleanup(patch.stopall)

    def test_iq_order_padding_and_signed_limits(self):
        with Radioberry() as berry:
            self.assertEqual(berry.send_iq([(0x1234, -21555), (-32768, 32767)], pad=True), 2)
        block = bytes(self.write.call_args.args[1])
        self.assertEqual(block[:8], bytes.fromhex("1234abcd80007fff"))
        self.assertEqual(block[8:], bytes(16384 - 8))
        self.close.assert_called_once_with(7)

    def test_short_burst_requires_explicit_padding(self):
        with Radioberry() as berry:
            with self.assertRaises(ValueError):
                berry.send_iq([(1, 2)])
        self.write.assert_not_called()

    def test_bad_sample_is_rejected_before_any_write(self):
        with Radioberry() as berry:
            with self.assertRaises(struct.error):
                berry.send_iq([(1, 2)] * 4096 + [(32768, 0)], pad=True)
        self.write.assert_not_called()

    def test_blocking_and_partial_write(self):
        accepted = bytearray()
        def short_write(fd, payload):
            n = min(len(payload), 101)
            self.assertLessEqual(len(payload), 16384)
            accepted.extend(payload[:n])
            return n
        self.write.side_effect = short_write
        with Radioberry() as berry:
            berry.send_iq([(1, -1)] * 8192)
        self.assertEqual(accepted, bytes.fromhex("0001ffff") * 8192)

    def test_control_legacy_abi_and_returned_fields(self):
        def exchange(fd, number, payload, mutate):
            self.assertEqual((fd, number, mutate), (7, 0x40017801, True))
            self.assertEqual(struct.unpack("=9i", payload),
                             (0, 0, 0, 0, 0, 0, 4, 2, -1))
            payload[:] = struct.pack("=9i", 1, 2, 3, 4, 5, 6, 7, -999, -999)
        self.ioctl.side_effect = exchange
        with Radioberry() as berry:
            result = berry.control(4, 2, 0xffffffff)
        self.assertEqual(result, dict(major=1, minor=2, fpga=3, nr=4, nt=5,
                                      version=6, status=7))

    def test_close_is_idempotent(self):
        berry = Radioberry()
        berry.close()
        berry.close()
        self.close.assert_called_once_with(7)


if __name__ == "__main__":
    unittest.main()
