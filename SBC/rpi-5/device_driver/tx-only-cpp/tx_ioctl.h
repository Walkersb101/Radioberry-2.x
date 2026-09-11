/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RB_TX_IOCTL_H
#define RB_TX_IOCTL_H
#include <linux/ioctl.h>
#include <linux/types.h>
/* Full-duplex SPI command bytes: board, command, data[31:24]..data[7:0].
 * The kernel copies the request into trusted memory, shifts it through SPI
 * mode 3, then replaces all six bytes with the returned SPI bytes. Unlike the
 * original driver's ioctl, size and direction match the payload. Semantic
 * names are deliberately omitted because they depend on the gateware protocol.
 */
struct rb_tx_control { __u8 bytes[6]; };
#define RB_TX_CONTROL _IOWR('T', 1, struct rb_tx_control)
#endif
