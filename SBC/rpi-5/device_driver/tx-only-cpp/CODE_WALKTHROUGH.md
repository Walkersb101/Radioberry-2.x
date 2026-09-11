# Code walkthrough

This document follows one write from userspace to the FPGA and then follows
module setup and teardown. It complements the comments beside the code.

## One call, end to end

`send_fixed_bits.cpp` stores the data in `kBits`. `sizeof(kBits) - 1` excludes
the C string terminator because NUL is not part of the bit alphabet. The loop is
required because POSIX permits `write()` to accept fewer bytes than requested.

The kernel enters `rb_file_write()`. It handles zero length, rejects more than
4096 characters and uses `memdup_user()` to make a trusted kernel copy. The
global mutex then prevents SPI control, another write or removal from changing
hardware state during the operation.

`rb_core_write()` is the stable C symbol for `Transmitter::write()`. The method
first checks lifecycle state and validates every byte. Doing all validation up
front prevents an input such as `"111x"` from emitting three bits before the
error is discovered.

The packing recurrence is:

```text
word(next) = (word(previous) << 1) | input_bit
```

After `n` input bits, `word <<= 32 - n` moves a short group to the most
significant end. The shift happens only for `n < 32`, avoiding undefined C++
behaviour from shifting a 32-bit object by 32.

`rb_hw_try_put()` asks RP1 for the current TX FIFO state. A negative result is
an external driver error, a response of the wrong size is treated as `EIO`, and
a full FIFO returns the private bridge value `1`. Otherwise a nonblocking PUT
queues the word. The C++ core converts repeated `1` values into bounded sleep
and retry operations.

The PIO instruction `PULL BLOCK` removes the word from the FIFO. Because
`shiftctrl` selects left shift, each `OUT PINS,1` presents the next most
significant bit on GPIO 5. Side-set pulses GPIO 4. The X register starts at 31,
so the loop performs exactly 32 outputs.

## Probe and setup ownership

The device-tree overlay creates the SPI device. Linux matches its compatible
string to `rb_of_match`, then calls `rb_probe()`.

Probe configures mode 3 and eight bits per word, records the SPI device and
calls the C++ setup entry. `Transmitter::start()` performs ownership changes in
this order:

1. Open an RP1 PIO client.
2. Claim any available state machine.
3. Explicitly disable it.
4. load nine instructions at origin zero.
5. configure GPIO 4 as a low clock output.
6. configure GPIO 5 as a low data output.
7. configure GPIO 12 as a ready input with pull-down.
8. initialise the state-machine registers and clear its FIFOs.
9. enable the state machine.

Only after every hardware operation succeeds does `running` become true. Any
failure jumps to `stop()`, which can safely clean a partially completed setup
because `acquired`, `sm` and the pin bitmap record exactly what exists.

Probe registers the misc device last. Users therefore cannot open a partially
initialised data path.

## Configuration constants

The four register values are retained from the Radioberry Pi 5 TX path:

| Field | Value | Meaning used here |
| --- | --- | --- |
| `clkdiv` | `0x00080000` | 16.8 fixed-point integer divider 8 |
| `execctrl` | `0x4c008000` | optional side-set, JMP pin 12, wrap 0 through 8 |
| `shiftctrl` | `0x00000000` | left shift, no autopull; program pulls explicitly |
| `pinctrl` | `0x40101005` | OUT GPIO 5 count 1, side-set GPIO 4 |

These control the serial link between Pi and FPGA. They do not select the
application's complex-sample rate or RF carrier by themselves.

## Teardown and why its order matters

Device removal first unregisters `/dev/radioberry-tx`, preventing new opens.
It then takes the same mutex used by active operations. `Transmitter::stop()`
disables the state machine, disconnects touched GPIOs while the RP1 client is
still valid, and finally closes the client. Closing first would make the GPIO
cleanup calls use a released client, so the order is intentional.

`rp1_pio_close()` owns the final release of resources claimed through that
client. There is no independent DMA or workqueue lifetime in this driver.

## C++ object model

`tx` has static storage and a trivial type. The loader zeroes it, which gives
false booleans, a zero pin mask and an irrelevant initial state-machine integer.
`start()` sets `sm = -1` immediately after acquiring the client, before a claim
can fail. There is no dynamic initialisation function.

The class is useful here because it keeps lifecycle state and invariants next
to the algorithms. It is not used to imitate Linux objects. The C entry points
are intentionally tiny and are the only functions called from the shim.

## Error contract

The core uses numeric negative Linux errno values because including Linux
headers would destroy its isolation. The bridge and tests define the meaning:

| Value | Meaning |
| --- | --- |
| `-5` | external response was structurally unexpected (`EIO`) |
| `-16` | core already owns hardware (`EBUSY`) |
| `-19` | TX is not running (`ENODEV`) |
| `-22` | pointer, size or bit alphabet is invalid (`EINVAL`) |
| `-110` | FIFO stayed full for all retries (`ETIMEDOUT`) |

The bridge's positive `1` is internal and means retry. It never escapes as a
successful byte count unless the C++ logic converts it by eventually queuing
the word.

## What to change for actual custom information bits

Keep this driver unchanged and add a userspace encoder in front of `write()`:

1. frame information bits, including synchronisation and error detection;
2. map framed symbols to a modulation constellation;
3. pulse-shape and sample the waveform;
4. quantise complex samples to the gateware's signed I/Q representation;
5. serialise each required 32-bit I/Q word into 32 `0`/`1` characters;
6. configure the radio using verified SPI commands;
7. write complete word-aligned strings.

The truly minimal hardware path ends at step 7. Skipping steps 1 through 5 is
valid only when the supplied bits already represent correctly packed I/Q words.
