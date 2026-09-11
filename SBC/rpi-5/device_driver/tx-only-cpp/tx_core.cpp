// SPDX-License-Identifier: GPL-2.0-only
// This C++ file is linked INSIDE radioberry_tx.ko. There is no userspace
// process here and no libstdc++, exceptions, RTTI, heap or static constructor.
#include "bridge.h"

static_assert(sizeof(unsigned int) == 4, "PIO words must be 32 bits");
static_assert(sizeof(unsigned short) == 2, "PIO instructions must be 16 bits");

namespace {
constexpr int invalid = -22;       // Linux EINVAL
constexpr int no_device = -19;     // Linux ENODEV
constexpr int timeout = -110;      // Linux ETIMEDOUT
constexpr int busy = -16;          // Linux EBUSY
constexpr int io_error = -5;       // Linux EIO
constexpr unsigned int clock_pin = 4, data_pin = 5, ready_pin = 12;
constexpr unsigned int max_bits = 4096;
constexpr unsigned int full_retries = 1000;

// The original Radioberry TX handshake, with absolute jumps based at zero.
// Fixed origin deliberately avoids relying on external JMP relocation.
// Data bits go MSB first. Clock also pulses while polling READY.
constexpr unsigned short program[] = {
    0x1ac2, // 0: jmp pin,2 side 1 [2] -- test READY with CLK high
    0x1200, // 1: jmp 0 side 0 [2]     -- not ready: lower CLK and retry
    0xb242, // 2: nop side 0 [2]       -- lower CLK before preparing data
    0xe03f, // 3: set x,31             -- 32 data-bit iterations
    0x80a0, // 4: pull block           -- wait for one CPU-supplied FIFO word
    0x6001, // 5: out pins,1           -- drive next bit on GPIO 5
    0xba42, // 6: nop side 1 [2]       -- clock high phase
    0x1245, // 7: jmp x--,5 side 0 [2] -- clock low; loop until all bits sent
    0x0000, // 8: jmp 0                -- next word's ready handshake
};

class Transmitter {
    // Zero-initialised static storage; no constructor executes at module load.
    bool acquired;
    bool running;
    unsigned int pins; // Which pins this attempt may have changed.
    int sm;
public:
    int start()
    {
        if (acquired) return busy;
        int rc = rb_hw_open();
        if (rc < 0) return rc;
        acquired = true; // All later failures must close the RP1 client.
        sm = -1;
        sm = rb_hw_claim();
        if (sm < 0) { rc = sm; goto fail; }
        rc = rb_hw_enable(static_cast<unsigned int>(sm), 0);
        if (rc < 0) goto fail;
        rc = rb_hw_program(program, 9, 0);
        if (rc < 0) goto fail;
        if (rc != 0) { rc = io_error; goto fail; }

        // Mark each pin before its multi-step setup so partial failure also
        // parks that pin during cleanup. No RX GPIO is touched.
        pins |= 1u << clock_pin;
        rc = rb_hw_pin(clock_pin, 1, 0, static_cast<unsigned int>(sm));
        if (rc < 0) goto fail;
        pins |= 1u << data_pin;
        rc = rb_hw_pin(data_pin, 1, 0, static_cast<unsigned int>(sm));
        if (rc < 0) goto fail;
        pins |= 1u << ready_pin;
        rc = rb_hw_pin(ready_pin, 0, 1, static_cast<unsigned int>(sm));
        if (rc < 0) goto fail;
        {
            const rb_pio_config config = {
                0x00080000, // Integer clock divisor 8, not the I/Q sample rate.
                0x4c008000, // Optional side-set; JMP PIN=12; wrap 0..8.
                0x00000000, // Shift left (MSB first); explicit PULL, no autopull.
                0x40101005, // OUT base=5/count=1; optional clock side-set base=4.
            };
            rc = rb_hw_config(static_cast<unsigned int>(sm), &config);
        }
        if (rc < 0) goto fail;
        rc = rb_hw_enable(static_cast<unsigned int>(sm), 1);
        if (rc < 0) goto fail;
        running = true;
        return 0;
    fail:
        stop();
        return rc;
    }
    void stop()
    {
        if (!acquired) return;
        // Park the pins while the RP1 client still exists. rb_hw_unpin() is
        // itself an RP1 operation, so doing this after close would be a real
        // use-after-release bug in the hardware abstraction.
        if (sm >= 0) rb_hw_enable(static_cast<unsigned int>(sm), 0);
        if (pins & (1u << clock_pin)) rb_hw_unpin(clock_pin);
        if (pins & (1u << data_pin)) rb_hw_unpin(data_pin);
        if (pins & (1u << ready_pin)) rb_hw_unpin(ready_pin);
        // Closing releases the claimed SM and instruction-memory allocation.
        // No DMA callbacks, worker threads or outstanding heap objects exist.
        rb_hw_close();
        acquired = false;
        running = false;
        pins = 0;
        sm = -1;
    }
    long write(const char *bits, unsigned long count)
    {
        if (!running) return no_device;
        if (!count) return 0;
        if (!bits || count > max_bits) return invalid;
        // Validate the WHOLE input before emitting any word. ASCII is just
        // our simple userspace ABI; those ASCII bytes are not sent to FPGA.
        for (unsigned long i = 0; i < count; ++i)
            if (bits[i] != '0' && bits[i] != '1') return invalid;

        unsigned long accepted = 0;
        while (accepted < count) {
            const unsigned long remaining = count - accepted;
            const unsigned int n = remaining < 32 ?
                static_cast<unsigned int>(remaining) : 32;
            unsigned int word = 0;
            for (unsigned int j = 0; j < n; ++j)
                word = (word << 1) | (bits[accepted + j] == '1' ? 1u : 0u);
            // Tail bits occupy the MSB end, then zeros fill the final word.
            // Never shift by 32: that would be undefined for a 32-bit value.
            if (n < 32) word <<= (32 - n);
            int rc = 1;
            for (unsigned int attempt = 0; attempt < full_retries; ++attempt) {
                rc = rb_hw_try_put(static_cast<unsigned int>(sm), word);
                if (rc != 1) break; // Success or external RP1 error.
                rc = rb_hw_pause(); // Sleep/check signals in normal kernel context.
                if (rc < 0) break;
                rc = 1;
            }
            if (rc == 1) rc = timeout;
            // A short positive result counts original ASCII characters,
            // not words or padded bits. Earlier words cannot be rolled back.
            if (rc < 0) return accepted ? static_cast<long>(accepted) : rc;
            accepted += n;
        }
        return static_cast<long>(accepted);
    }
};
Transmitter tx; // Trivial zero-initialisation, no _GLOBAL__sub_I / .init_array.
}
extern "C" int rb_core_start(void) { return tx.start(); }
extern "C" void rb_core_stop(void) { tx.stop(); }
extern "C" long rb_core_write(const char *p, unsigned long n) { return tx.write(p,n); }
