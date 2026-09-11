/* SPDX-License-Identifier: GPL-2.0-only
 * Private C ABI between Linux glue and a freestanding C++ kernel core.
 * No Linux or C++ standard-library headers cross this boundary.
 */
#ifndef RB_TX_BRIDGE_H
#define RB_TX_BRIDGE_H
#ifdef __cplusplus
extern "C" {
#endif
struct rb_pio_config {
    unsigned int clkdiv, execctrl, shiftctrl, pinctrl;
};
/* C++ core entry points. Linux glue serialises all calls with one mutex. */
int rb_core_start(void);
void rb_core_stop(void);
long rb_core_write(const char *bits, unsigned long count);
/* C wrappers around RP1 primitives. Return 0 or negative Linux errno,
 * except claim/program (nonnegative index) and try_put (1 means full).
 */
int rb_hw_open(void);
int rb_hw_claim(void);
int rb_hw_program(const unsigned short *words, unsigned int count,
                  unsigned int origin);
int rb_hw_pin(unsigned int pin, int output, int pulldown, unsigned int sm);
void rb_hw_unpin(unsigned int pin);
int rb_hw_config(unsigned int sm, const struct rb_pio_config *config);
int rb_hw_enable(unsigned int sm, int enabled);
int rb_hw_try_put(unsigned int sm, unsigned int word);
int rb_hw_pause(void);
void rb_hw_close(void);
#ifdef __cplusplus
}
#endif
#endif
