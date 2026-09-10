# Review findings and validation limits

Baseline: `f8e8e3b4ddfaccda92e3309e1c22168a88e3f32f`, driver 5.54. These observations describe the unmodified executable implementation. No fixes are included in the documentation patch. Priority labels are qualitative engineering triage, not CVSS scores or a claim of demonstrated exploitability.

## Directly visible findings

| ID | Priority | Location | Observation and consequence | Suggested separate change |
|---|---|---|---|---|
| R01 | High | `radioberry_read` | Replaces the requested count and may copy more than the caller asked for. Even a zero-length request is not handled conventionally. | Enforce maximum length; coordinate the bundled userspace caller, which passes sample count. |
| R02 | High | `radioberry_ioctl` | `rb_info_ret` is a stack object; `command` and `command_data` are never set before copying the entire object to userspace. | Zero-initialise the structure and explicitly define every returned field. |
| R03 | High | `radioberry_ioctl` | Allocation failure is unchecked and the result of `copy_from_user` is assigned but ignored. | Check allocation/copy and return without reading invalid or incomplete data. |
| R04 | Medium | `radioberry_write` | One-second timeout returns without freeing `tx_stream`. | Use a single cleanup path. |
| R05 | Medium | `radioberry_ioctl` | Failed `copy_to_user` returns before freeing `rb_info`. | Free allocation on all paths. |
| R06 | Medium | read/write wait handling | Negative returns from `wait_event_interruptible_timeout` are not handled. Operations continue after interruption. | Return the negative interruption status before consuming/queuing data. |
| R07 | Medium | TX stream | Writers sleep on `tx.queue`, but the consumer never calls a corresponding wakeup as space opens. A timeout wake can recheck the predicate, so the result can be a long delay rather than necessarily an error. | Wake writers after draining software FIFO; test blocked writers. |
| R08 | Medium | TX write/submission | No maximum length or four-byte framing validation. Lengths above FIFO capacity cannot become admissible. | Define and enforce byte-stream semantics, partial writes and bounds. |
| R09 | Medium | TX block handling | A final fragment below 16384 bytes is not submitted. Release supplies no flush or cancel. | Define a deliberate drain/pad/abort policy coordinated with radio TX control. |
| R10 | Medium | `tx_dma_kick_now` | Removes data before buffer lookup/submission, and does not restore it on failure. | Preserve queue contents until submission is assured, or expose an explicit dropped-data error. |
| R11 | High | RX/TX setup failure paths | Setup may `kfree(ctx)` and return to a caller that still accesses/frees the same context. RX open failure gives a concrete double-free path; TX open failure leaves outer cleanup using freed context. | Put context ownership in one layer and unwind acquired resources exactly once. |
| R12 | High | module initialisation | Several registration/mapping/loading failures are ignored or leave earlier registrations alive. RX starts before TX setup completes. | Propagate status and implement reverse-order cleanup with per-resource state. |
| R13 | High | `firmware_load` | Image-copy allocation is unchecked before memcpy. | Handle ENOMEM and propagate loader status. |
| R14 | Medium | gateware loader | Preparation errors are ignored, activation returns zero for both success/failure, missing image only logs. | Return and propagate meaningful errors; stop initialization on failure. |
| R15 | Medium | `initialize_rpi` | Derives register pointers after mapping failure; module init ignores its returned status. | Return immediately and abort initialization. |
| R16 | Medium | control path | SPI status is ignored by ioctl; receiver count and decoded response are used regardless. Global SPI pointer is set before setup success and has no remove handler here. | Validate device readiness, propagate SPI errors, and manage device lifetime. |
| R17 | Medium | RX callback | FIFO overflow resets the entire existing queue without communicating a loss count to the reader. | Track and expose overruns and define recovery semantics. |
| R18 | Medium | RX metadata handling | Accepted components are queued before the whole group is validated; a later mismatch does not retract them. | Stage complete groups or return explicit discontinuity information. |
| R19 | Medium | RX read alignment | Readiness is tested before alignment drops words; read may return fewer bytes and potentially a partial receiver group. | Align/check atomically enough to enforce the documented group contract. |
| R20 | Medium | control/RX concurrency | `nrx` changes without coordinating metadata/FIFO/DMA boundaries. | Define a stopped or versioned reconfiguration procedure. |
| R21 | Low | ioctl declaration | Encodes one-byte write despite copying a nine-int structure in both directions. | Design a versioned ABI; preserve compatibility with existing command number. |
| R22 | Low | Makefile install | Copies a root-level header that actually lives in `include/`. | Correct the source path in a separate installation change. |
| R23 | Low | TX diagnostic messages | Ready input is described as output, clock output as input, and pull-down as pull-up in some failures. | Correct misleading diagnostic strings. |
| R24 | Low | stream state | `dma_done` completion objects are initialised/signalled but unused by the transfer paths. | Remove or use them as part of an explicit lifetime design. |

## Questions requiring additional implementation or hardware evidence

These are not presented as confirmed runtime failures.

### DMA callback lifetime and teardown

Cleanup cancels work and disables/removes/unclaims the PIO program, then closes the RP1 client. A callback can itself queue work or submit another transfer. The Radioberry code has no explicit stopping flag or DMA-terminate call. Inspect `rp1_pio_close`, transfer cancellation and callback synchronization before deciding whether a callback can access the freed global context. `dma_running=0` does not prove no DMA is active.

### Callback context and reentrancy

TX calls its next transfer submission directly from a DMA callback. Confirm whether that external API can sleep, take incompatible locks or synchronously invoke callbacks. Verify the ownership of the selected buffer and the meaning of transfer completion before adding pipelining.

### PIO jump relocation

Wrap bounds are adjusted to the allocated instruction offset, but the caller supplies unmodified branch words. Verify whether `rp1_pio_add_program` relocates JMP addresses, particularly when TX follows the 18-instruction RX program. Do not infer an addressing defect solely from seeing raw branch targets in this client.

### Effective SPI settings

The overlay uses `spi-mode = <3>` and `spi-bits-per-word = <8>`. Establish whether the target kernel consumes these properties and what the actual `spi_device` settings become. Confirm CPOL/CPHA on the pins if control fails. The configured maximum clock is a limit, not evidence of measured 48 MHz operation.

### Shared-descriptor and close semantics

A mutex is acquired in open and released in file release. Kernel mutex ownership is associated with tasks, whereas a file can be shared, passed or finally released by another task. Review this lifetime locking pattern even though it blocks a second open in the simple case. Concurrent operations on the same descriptor are also possible, so individual FIFO locks do not make compound protocol operations atomic.

### Direct MMIO and resource discovery

Hard-coded RP1 mapping, volatile structure access and pad writes avoid standard resource-managed register access. Review ordering, device address discovery and pin ownership for the intended kernel/platform. No claim is made here that the exact current access pattern fails on the target Pi.

### Hardware timing and numerical meaning

The PIO clock divider and instruction delays establish a relative schedule. They do not verify the current RP1 source clock, FPGA launch/sample edges, setup/hold margins, FIFO depths, radio sample rate or RF amplitude scaling. Confirm the matching gateware and measure those properties if modifying transport timing.

## Focused validation for a future fixes branch

1. Build against the actual target kernel and matched external RP1 driver.
2. Exercise read lengths 0, short and nominal, and verify no bytes beyond the requested region change.
3. Exercise invalid ioctl pointers, allocation failure and interrupted reads/writes; check errors, allocations and returned structure contents.
4. Fill TX FIFO and confirm blocked writers wake promptly after a drain.
5. Send short TX tails and explicitly check the chosen completion/abort policy.
6. Inject RX metadata loss and FIFO overflow; require complete groups or documented discontinuity reporting.
7. Change receiver count at a controlled boundary and check component order.
8. Test initialization failures at each acquired resource and repeated load/unload under traffic, with appropriate kernel diagnostics.
9. Capture TX DATA/CLK/READY and RX nibble/clock/ready signals with known patterns.

These are proposed checks, not tests executed for the documentation work.

## Documentation validation performed

A lexical comparison removes comments and whitespace from each changed C/header file and compares the remaining token sequences against the pinned baseline. All changed files must match. This check is stronger than simply inspecting a textual diff but is not a compiler, semantic analyser or hardware test. Encoded PIO arrays are separately extracted and compared. The patch is checked for whitespace errors and reverse applicability after creation.

The documentation package records exact results in `VALIDATION.txt`. No kernel compilation, on-device load, DMA exercise or RF test was performed. Original defects remain intentionally present so the annotation patch is reviewable as documentation only.

## Minimum viable custom transmit path

### First decide what “bits” means

There are two different goals:

1. **Transmit a custom bit message over RF.** Map bits to a baseband waveform in userspace, generate I/Q samples, and feed those samples to the existing driver. No PIO or kernel change is needed for the modulation itself.
2. **Send literal digital bits over GPIO to the FPGA.** The existing TX interface clocks arbitrary 32-bit words, but the radio gateware interprets those words as I/Q. A word such as `0xDEADBEEF` is two sample bit patterns, not an RF packet containing those four bytes. A different digital protocol or FPGA interpretation requires a corresponding gateware/interface change.

For an RF message, the shortest useful path is:

**message bytes → bit order/framing → modulation → signed 16-bit I/Q pairs → four-byte packing → write to /dev/radioberry → TX kfifo → DMA → PIO → FPGA radio processing.**

Only the left-hand message-to-I/Q stages are new application logic. The FPGA and radio control must already be configured for a suitable TX frequency, sample format/rate and transmit state. The kernel write handler does not configure these for you.

### Recommended first experiment: replace sample generation in the existing application

The smallest change that retains the existing control path is in `SBC/rpi-5/device_driver/firmware/radioberry.c`, inside `txWriter()`:

1. Keep the application running with its existing SDR client and radio configuration.
2. Keep consuming all eight bytes from each queued packet group, including the four discarded leading bytes. Keep `sem_wait(&tx_full)` and `sem_post(&tx_empty)` balanced.
3. After the existing code fills `tx_iqdata[0..3]`, overwrite those four bytes with one generated I/Q pair.
4. Keep the existing MOX/CWX gating and write call, or improve its error/short-write handling separately.
5. Continue feeding packet groups from the client. This thread is paced by their arrival; changing only sample generation does not remove the dependency on `tx_full`.

This replaces the sample source while retaining discovery, frequency/control commands, packet handling and existing board integration. It is the minimum integration change, although it is not a standalone transmitter. Do not start a second process opening the device alongside this daemon: the kernel permits only one open.

A repeated rectangular BPSK test mapping can use bit 0 → `I=-A, Q=0` and bit 1 → `I=+A, Q=0`. Hold each symbol for `samples_per_symbol` samples. The nominal bit rate is `sample_rate / samples_per_symbol`. For example, **if the configured TX rate is 48000 pairs/s**, 48 samples per symbol gives 1000 bits/s. This is a conditional example, not a rate-setting command or an independently verified gateware rate.

Rectangular BPSK illustrates the data path; it is not a spectrally shaped modem. Practical over-air use needs appropriate pulse shaping, framing/preamble, receiver synchronisation and any required error detection/coding. A zero I/Q pair means zero baseband input, whereas a logical zero in this BPSK mapping means a nonzero negative I sample.

### A small sample generator for that insertion point

The following is standalone C helper code intended to be called once per I/Q pair. It repeats byte `0xA5`, most-significant bit first, at 48 samples per bit. Its amplitude of 4096 is an illustrative numeric level, not an RF power calibration.

```c
#include <stddef.h>
#include <stdint.h>

static void custom_iq_next(uint8_t out[4])
{
    static const uint8_t message[] = {0xA5};
    static size_t bit_index = 0;
    static unsigned sample_in_symbol = 0;
    const unsigned samples_per_symbol = 48;
    const int16_t amplitude = 4096;

    /* Treat payload bytes as MSB-first bits; repeat the whole message. */
    unsigned bit = (message[bit_index / 8] >>
                    (7u - (unsigned)(bit_index % 8))) & 1u;
    int16_t i = bit ? amplitude : (int16_t)-amplitude;
    int16_t q = 0;

    /* Convert signed values to their 16-bit patterns, then pack big-endian. */
    uint16_t iu = (uint16_t)i;
    uint16_t qu = (uint16_t)q;
    out[0] = (uint8_t)(iu >> 8);
    out[1] = (uint8_t)iu;
    out[2] = (uint8_t)(qu >> 8);
    out[3] = (uint8_t)qu;

    if (++sample_in_symbol == samples_per_symbol) {
        sample_in_symbol = 0;
        bit_index = (bit_index + 1) % (sizeof(message) * 8);
    }
}
```

Insertion immediately before the existing `write(fd_rb, tx_iqdata, sizeof(tx_iqdata))`, inside its MOX/CWX block:

```c
custom_iq_next(tx_iqdata);
```

This repeats indefinitely while the surrounding application supplies TX packets and enables TX. The static generator state belongs to one writer thread. It does not reset on a new TX burst, advance based on wall-clock time, set RF frequency, handle stop requests or retry writes. Decide explicitly whether a new burst should reset the bit index.

### Minimum standalone application, after replacing the existing daemon

A standalone transmitter can omit network sockets, discovery, packet construction, RX reads, temperature polling and the userspace sample ring **from the sample-generation path**. Any board/PA management actually needed by the installed hardware must be retained or deliberately replaced. The kernel still initialises its RX stream even if the application never calls read; its RX FIFO will eventually use the documented reset-on-overrun policy.

The application still needs these steps:

| Step | Required action | Existing reference |
|---|---|---|
| 1 | Ensure matching gateware, overlay, external RP1 module and Radioberry module are loaded | Driver initialization and installation setup |
| 2 | Stop the existing device-owning application and open `/dev/radioberry` once | `initRadioberry()` / exclusive `radioberry_open()` |
| 3 | Establish the gateware control configuration and radio TX state | `send_control()`, `handleCommand()` and matching gateware protocol |
| 4 | Produce four-byte I/Q pairs from your message | `custom_iq_next()` above or a real modulator |
| 5 | Supply complete 16384-byte blocks, or enough smaller writes to accumulate them | `radioberry_write()` and `tx_dma_kick_now()` |
| 6 | Maintain the stream and control state for the required duration | Application scheduling plus FPGA ready handshake |
| 7 | End the waveform and disable TX using a verified completion/control policy | No driver drain API exists in this version |

A minimal **sample transport loop**, assuming you already have an exclusively opened and correctly configured `fd_rb`, is:

```c
uint8_t block[16384];

for (size_t pair = 0; pair < sizeof(block) / 4; ++pair)
    custom_iq_next(&block[4 * pair]);

/* Queue exactly one DMA block. This is not a transmit-completion wait. */
ssize_t accepted = write(fd_rb, block, sizeof(block));
if (accepted != (ssize_t)sizeof(block)) {
    /* Stop/report failure in this minimal example. A production loop must
     * distinguish an error from a short positive result and preserve any
     * unaccepted suffix; do not resend the accepted prefix. */
}
```

This snippet also requires `<unistd.h>` and the surrounding application setup/error handling. It deliberately does not claim to be a complete runnable RF transmitter. A single initial 16 KiB write can fit into an empty 32 KiB software FIFO and start DMA without first hitting the missing-TX-wakeup issue. Sustained writes can hit that defect, so fixing R07 is a prerequisite for a reliable streaming implementation.

### What controls are required, and what is not yet established?

The exact standalone initialization sequence must be established against the chosen gateware. The reviewed userspace code shows:

- `rb_command` bit 0 carries `running`.
- `rb_command` bit 1 carries `CWX`.
- `rb_command` bit 2 carries the application's `pa_temp_ok` condition.
- `processPacket()` extracts MOX from bit 0 of the packet control byte.
- The SPI request contains this board-control byte, the protocol command byte and its 32-bit data.
- `rb_control_thread()` sends `send_control(MOX)` when its queued command list is empty.

These are distinct fields. Setting `rb_command=1` is not, by itself, an established instruction to key the transmitter. Likewise, simply opening the device and writing nonzero I/Q does not establish that RF output is enabled or correctly tuned.

For the first prototype, reuse a known-working control stream from the existing software. For a clean standalone implementation, trace or capture that stream for the selected gateware and document the minimal frequency, rate/configuration, gain/drive and MOX transitions. This review has not verified a complete numeric command sequence against the FPGA. It intentionally does not invent magic ioctl values. The existing ioctl input-copy/output-initialisation defects also remain relevant when exposing the device to a custom process.

### Why a tiny message still needs substantial buffering

The current TX worker only submits full blocks of **4096 I/Q pairs**. At 48 samples per bit, an eight-bit message occupies 384 pairs (1536 bytes). Writing only those 1536 bytes leaves them in the software FIFO and starts no DMA.

For a finite message, generate the intended samples and append **zero I/Q samples** to complete a 4096-pair boundary, then queue the block. At the illustrative 48000-pair/s rate, one complete block represents 85.33 ms of input waveform. Padding solves the software block threshold only. It does not tell you when samples have finished leaving the FPGA or traversing its interpolation/filter pipeline.

`write()` reports queue acceptance. `close()` does not drain TX. A DMA callback is not exposed as a userspace completion notification and does not prove RF completion. Arbitrary sleep followed by TX-off is therefore only an experiment, not a robust burst boundary. Reliable finite bursts need a defined drain/idle indication or a validated gateware timing contract, plus any zero tail required for filter settling. Fix R09 or add a suitable completion interface before treating this as a dependable packet transmitter.

### If the aim is literal GPIO data instead

To pass word `0xDEADBEEF` through the current transport, its userspace bytes are `DE AD BE EF`. Repeat or supplement words until a 16 KiB block is available. DMA and the PIO shifter send each word MSB first on GPIO 5 with GPIO 4 clock and GPIO 12 ready.

On the existing radio interface, those halves are still interpreted as I/Q. The clock also pulses during ready polling, and ready is checked once per word, so this is not a generic unframed serial-bit wire. Keeping only `tx_dma_kick_now()` is insufficient: it relies on claimed/configured RP1 resources, DMA mappings, initialized FIFO/locks/work and the PIO pin/program setup. A digital-only fork can remove radio-specific layers only after replacing the FPGA endpoint and explicitly defining the desired handshake and word framing.

### Checks for the sample helper

The helper can be compiled and its first symbol transitions checked without hardware. Those checks validate packing and message sequencing only. They do not validate TX enable, RF frequency, spectral shape, DMA continuity or finite-burst completion. No radio test is claimed for these snippets.

Validation of the included helper: compiled as C11 with `-Wall -Wextra -Werror`; checked all 768 generated samples over two complete repetitions of 0xA5, including sign patterns, zero Q, and 48-sample symbol boundaries. Passed. These are host-only checks.

## C++ prototype implementation

A minimal [C++17 command-line transmitter](../../../../utility/minimal-tx-cpp/README.md) now implements the sample path described above. It accepts bit strings, generates rectangular BPSK I/Q or literal raw 32-bit words, pads complete DMA blocks, supports file output and accepts explicit start/stop control sequences. It reuses the unchanged kernel driver. Its host tests do not establish hardware completion or a gateware control configuration.
