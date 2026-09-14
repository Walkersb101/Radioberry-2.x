# Minimal Python wrapper for the original Radioberry Pi 5 driver

Copy `radioberry.py` next to your Python program. It uses only the Python
standard library on Linux. This wraps the original PIO driver at
`SBC/rpi-5/device_driver/pio-mode/driver` and its `/dev/radioberry` device.
It does not use the experimental C++ kernel module or `/dev/radioberry-tx`.

```python
from radioberry import Radioberry

# With the board already configured for your intended TX operation:
with Radioberry() as berry:
    berry.send_iq([(1000, 0), (-1000, 0)] * 2048)
```

Each pair is `(I, Q)`, with integer components from -32768 to 32767.
The wrapper packs `[I_hi, I_lo, Q_hi, Q_lo]`, retaining signed two's-complement
bit patterns. It does not accept normalised complex floats or perform scaling,
modulation, frequency selection or timed playback.

The original driver starts DMA only after **4096 pairs (16384 bytes)** have
accumulated. `send_iq()` therefore requires a multiple of 4096 pairs by default.
To submit a short burst explicitly padded with zero I/Q samples:

```python
with Radioberry() as berry:
    count = berry.send_iq([(1000, -1000)], pad=True)
    # count is 1; the actual block has that pair followed by 4095 zero pairs.
```

Padding is performed on every call, so use complete blocks for a continuous
stream. The finite input is fully packed and validated before any write.
Pass manageable blocks rather than an infinite generator. Partial writes and
interrupted writes are handled; other OS errors propagate. After an error,
some data may already be queued, so do not blindly replay the entire burst.

## Control

```python
def send_control(berry, rb_command, command, command_data):
    return berry.control(rb_command, command, command_data)
```

`control(rb_command, command, data)` uses the same fields as the original
userspace `send_control()` implementation:

| Argument | Range | SPI representation |
| --- | --- | --- |
| `rb_command` | 0..255 | byte 0, board control flags |
| `command` | 0..255 | byte 1, protocol command |
| `data` | 0..4294967295 | bytes 2..5, most significant byte first |

It returns a dictionary with `major`, `minor`, `fpga`, `nr`, `nt`, `version`
and `status`. These are the original driver's decoded results, not six raw
SPI bytes. Use verified values from the control protocol for your gateware.
See `../../device_driver/firmware/radioberry.c`, especially `send_control()`
and `handleCommand()`. This wrapper deliberately does not guess a transmit
frequency, drive level, MOX setting or board state on your behalf.

The ioctl number is `0x40017801`, matching `_IOW('x', 1, __u8)` on the Pi.
The original ioctl incorrectly advertises a one-byte argument but copies a
36-byte structure of nine native 32-bit integers. The wrapper supplies the
full structure and ignores the two uninitialised returned fields. The kernel
also ignores the SPI transaction result, so successful ioctl return alone
does not establish that the board accepted a command.

## Device lifetime

Install/load the original driver, matching RP1 dependency and gateware first.
Stop the existing Radioberry application before opening this wrapper because
the driver permits only one open. Use a single thread for each wrapper instance.

`send_iq()` means queued, not physically transmitted. The original driver has
no TX completion/drain operation. Closing the descriptor neither proves that
all samples have left the FPGA nor turns off RF. A subsequent open resets the
driver queues. Keep the descriptor open across your control/data sequence and
manage transmit state through the existing control protocol.

Run the hardware-free ABI and packing checks with:

```sh
python3 -m unittest -v test_radioberry.py
```

Tests mock the OS boundary. Real GPIO timing, RF output and kernel behaviour
still require validation on your Raspberry Pi 5.
