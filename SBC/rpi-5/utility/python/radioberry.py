# SPDX-License-Identifier: GPL-2.0-only
"""Small Python wrapper for the original Pi 5 PIO /dev/radioberry driver.

I/Q components are signed 16-bit integers. Control values are the original
protocol fields, not invented frequency/power abstractions. No dependencies.
"""

import errno
import fcntl
import os
import struct

# Original header: _IOW('x', 1, __u8). Despite the encoded size of ONE byte,
# the handler copies nine native 32-bit ints both ways. Preserve that ABI.
_COMMAND = 0x40017801
_INFO = struct.Struct("=9i")
_IQ = struct.Struct(">hh")  # I high, I low, Q high, Q low.
_BLOCK = 16384             # Original TX DMA starts at 4096 I/Q pairs.


class Radioberry:
    """One device owner; use from one thread, preferably with a with-block."""

    def __init__(self, device="/dev/radioberry"):
        self._fd = os.open(device, os.O_RDWR | os.O_CLOEXEC)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.close()

    def close(self):
        """Release the descriptor. This neither drains TX nor disables RF."""
        if self._fd >= 0:
            fd, self._fd = self._fd, -1
            os.close(fd)

    def control(self, rb_command, command, data):
        """Send two byte fields and unsigned 32-bit data; return driver status.

        The kernel emits SPI bytes [rb_command, command, data MSB .. LSB].
        Supply protocol values appropriate to your gateware configuration.
        """
        if not (0 <= rb_command <= 255 and 0 <= command <= 255
                and 0 <= data <= 0xffffffff):
            raise ValueError("control needs two unsigned bytes and a uint32")
        # The C struct uses signed int, but all 32 data bits must survive.
        signed_data = data if data < 0x80000000 else data - 0x100000000
        payload = bytearray(_INFO.pack(0, 0, 0, 0, 0, 0,
                                       rb_command, command, signed_data))
        fcntl.ioctl(self._fd, _COMMAND, payload, True)
        values = _INFO.unpack(payload)
        # The original driver leaves its last two return fields uninitialised.
        # Expose only the seven fields it actually assigns.
        return dict(zip(("major", "minor", "fpga", "nr", "nt", "version",
                         "status"), values[:7]))

    def send_iq(self, samples, *, pad=False):
        """Queue a finite iterable of (I, Q) int16 pairs; return input count.

        Require a multiple of 4096 samples, or explicitly choose pad=True to
        append zero I/Q samples to the last DMA block. No automatic scaling.
        A successful return acknowledges queuing, not completed transmission.
        If a write fails, earlier bytes may already have been queued.
        """
        payload = b"".join(_IQ.pack(i, q) for i, q in samples)
        count = len(payload) // 4
        missing = (-len(payload)) % _BLOCK
        if missing and not pad:
            raise ValueError("need a multiple of 4096 I/Q pairs; or use pad=True")
        payload += bytes(missing)
        # Keep each request within one DMA block, below the kernel FIFO limit.
        for start in range(0, len(payload), _BLOCK):
            remaining = memoryview(payload)[start:start + _BLOCK]
            while remaining:
                try:
                    written = os.write(self._fd, remaining)
                except InterruptedError:
                    continue
                if written <= 0:
                    raise OSError(errno.EIO, "driver write made no progress")
                remaining = remaining[written:]
        return count
