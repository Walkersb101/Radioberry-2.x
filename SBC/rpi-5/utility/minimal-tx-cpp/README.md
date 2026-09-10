# Minimal Radioberry TX in C++

`radioberry-tx` takes a string of bits and sends words to the existing Pi 5
`/dev/radioberry` driver. It is a small **userspace C++17 transmitter**, not a
replacement kernel module. The existing kernel driver continues to own GPIO,
PIO, DMA and FPGA image loading. No network client or DSP library is required.

Two interpretations of the input are provided:

| Mode | Interpretation |
|---|---|
| `bpsk` (default) | Each bit becomes a rectangular BPSK symbol: 0 gives negative I, 1 gives positive I, Q is zero. |
| `raw` | Every 32 input bits become one literal FPGA transport word, MSB first. The string length must be a multiple of 32. |

The radio gateware interprets received words as I/Q even in raw mode. Raw bits
are not automatically an RF packet. BPSK is a basic waveform demonstration,
without pulse shaping, a preamble, error correction or a receiver protocol.

## Build

On Linux, from this directory:

```sh
make
make test
```

Requires a C++17 compiler, make, Linux userspace headers and, for tests only,
Python 3. The existing driver ioctl header is included from the repository.
No kernel headers or external PIO headers are needed to compile this utility.
The binary is ignored by git. This source was compiled and tested on the host;
a Pi 5 hardware run has not been performed.

## Generate a test file first

```sh
./radioberry-tx 10100101 --output message.iq
```

This creates a new binary file containing `[I_hi, I_lo, Q_hi, Q_lo]` records.
With defaults, each bit occupies 48 samples at amplitude 4096. Eight bits
produce 384 I/Q pairs, followed by 3712 zero pairs to fill one 16384-byte block.
Existing files are not overwritten. Use a new path for another run.

```sh
./radioberry-tx 10100101 --samples-per-bit 48 --amplitude 2048 \
  --repeat 10 --tail-samples 256 --output repeated.iq
```

Bits are taken left to right exactly as typed. Whitespace and characters other
than `0` and `1` are rejected. Repeats are consecutive, and symbols remain
continuous across DMA block boundaries. Tail samples and block padding are
zero I/Q, which is distinct from a BPSK zero bit.

`--sample-rate 48000` changes the nominal duration report only. It does **not**
configure the gateware rate. If the actual rate is 48000 pairs/s, the default
48 samples per bit represents 1000 bits/s. Amplitude is a signed sample level,
not a calibrated RF power value.

## Send to an already configured Radioberry

Prepare the Pi 5 with a matching gateware image, overlay, RP1 driver and
Radioberry kernel module. Stop any userspace radio application that owns
`/dev/radioberry`, because the kernel permits only one open. Establish and
verify the required radio state for this gateware, including its TX enable,
frequency, drive and sample rate.

For a device that is already configured and accepting TX words:

```sh
./radioberry-tx 10100101 --device /dev/radioberry \
  --configured --hold-ms 1000
```

The command requires permission to open the device. Use the access policy
configured on your Pi. The utility does not change device permissions.

`--configured` means that **you own radio setup and shutdown outside this
program**. It sends no SPI controls and leaves gateware state unchanged on
exit. Stopping an existing radio application may disable TX, so merely stopping
that application is not proof the device remains configured. A file write is
fully testable without these hardware prerequisites; RF transmission is not.

The message is written in complete 16 KiB blocks. A positive write result
means accepted by the kernel queue. The explicit one-second hold above is a
bench-test choice after queuing, not measured or acknowledged RF completion.
Do not infer that increasing this number fixes stalled DMA or incorrect TX
configuration.

## Own setup and shutdown using explicit controls

The utility can issue a sequence of gateware commands before writing and a
shutdown sequence afterward. Supply each in the form:

```text
--start-control BOARD_BYTE:COMMAND_BYTE:DATA_WORD
--stop-control BOARD_BYTE:COMMAND_BYTE:DATA_WORD
```

All three fields are **hexadecimal**, optionally prefixed with `0x`. They have
width limits of 8, 8 and 32 bits. Both options may be repeated, and commands
execute in the supplied order within their respective sequence. The program
requires both start and stop sequences unless `--configured` is used. These
modes cannot be mixed.

The fields map to the repository's `rb_info_arg_t` and
`RADIOBERRY_IOC_COMMAND` ABI. They are sent as a board-control byte, a protocol
command byte and four data bytes in high-to-low order. The program initialises
a fresh structure for each call because the ioctl overwrites it.

There are intentionally no guessed frequency, PA or MOX register values in
this implementation. Use commands verified for your selected gateware. In the
existing application, the board-control `running` bit and the protocol MOX bit
are different fields. A board value of one is not a verified complete TX-on
sequence. See the [transmit review](../../device_driver/pio-mode/driver/docs/REVIEW.md#minimum-viable-custom-transmit-path)
for the exact source references and what remains to be verified.

Stop controls are attempted after the hold, on ordinary write/start failure,
and on handled SIGINT/SIGTERM. They are also attempted after a partially
successful start sequence. A stop failure returns nonzero. This is best-effort
control, not guaranteed shutdown if the kernel is stuck, SPI fails or the
process receives SIGKILL. The current kernel ioctl ignores its SPI result, so
a successful ioctl is not independently verified board acknowledgement.

## Send literal 32-bit words

This bit string represents `0xDEADBEEF`:

```sh
./radioberry-tx 11011110101011011011111011101111 \
  --mode raw --output raw-word.iq
```

The first four output bytes are `DE AD BE EF`; the remaining 16380 bytes are
zero padding. To send it to the board instead, replace `--output raw-word.iq`
with `--device /dev/radioberry --configured --hold-ms 1000` after the setup
requirements above are met.

To fill one DMA block entirely with that word, add `--repeat 4096`. The raw
mode accepts several concatenated words as well. `--samples-per-bit` and
`--amplitude` affect BPSK only. In raw mode, `--tail-samples` appends zero words.

GPIO 5 carries serial data, GPIO 4 carries clock, and GPIO 12 is ready.
The existing PIO program clocks 32 bits per word and also pulses clock while
polling ready. This is the existing Radioberry handshake, not a generic serial
protocol or arbitrary GPIO waveform generator.

## Minimal source path

1. `parse()` validates the bit string, mode, output selection and controls.
2. `layout()` calculates payload, zero tail and padded size with overflow checks.
3. `word_at()` maps a bit to signed I/Q or assembles a literal 32-bit word.
4. `fill_block()` packs 4096 words into one big-endian 16 KiB block.
5. `Sink::write_block()` writes that block to a file or the character device.
6. The existing kernel performs TX FIFO insertion, DMA and PIO serialisation.

Only one block is held in memory regardless of repetition count. Positive
short writes advance by the accepted length; an unaligned device short write
is treated as a transport error. Zero-progress writes fail. The utility does
not busy-loop retry `EAGAIN`: the kernel's current timeout path leaks its
allocation, so repeated timeouts would compound that defect.

## Current limits

- No kernel changes are included. The original RX engine still runs even if
  this program never reads samples, and original driver defects remain.
- There is no TX drain/idle API. Closing the device does not drain or cancel
  queued samples. `--hold-ms` is explicit timing, not completion detection.
- A shutdown command issued before the FPGA consumes the final samples can
  truncate a burst. Hardware timing and pipeline settling must be established
  before relying on finite-message RF boundaries.
- Long streams can encounter the existing missing TX-space wakeup and timeout
  defects. This utility is a minimal prototype, not a validated continuous
  modem. A first single block into an empty FIFO avoids initially waiting for
  software FIFO space but does not prove the FPGA is ready.
- No device or RF tests have been run. Tests cover bit-to-byte behaviour,
  buffering and argument handling. Raw gateware configuration remains explicit.

## Host validation

`make test` performs 42 checks without opening a radio or sending ioctl
commands. It verifies signed big-endian I/Q, symbol and message continuity
across blocks, repetition, zero tails, exact block sizing, raw word order,
invalid/overflowing inputs, protection against overwriting files and rejection
of ordinary files passed as a device.

Exit status zero in file mode means the bytes were written. In device mode it
means bytes were queued, the requested hold ended and any requested stop
ioctls returned success. It does not mean the RF message was received or even
that the hardware accepted it. Handled interruption returns 130; other errors
return 1.
