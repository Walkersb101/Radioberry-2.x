
#include <linux/printk.h>

#include "rb2-trx-control.h"
#include "rb2-rpi5.h"

struct spi_device *spi_ctrl_dev = NULL;


/*
 * rb2_trx_initialize
 *
 * Select SPI pin functions and initialize sample-interface GPIO directions.
 * PIO configuration later takes ownership of the sample pins. This routine
 * only sets pins; it does not exchange radio register values with the FPGA.
 */
int rb2_trx_initialize() {	

	printk(KERN_INFO "initialize_firmware: make GPIO ready for rx and tx streaming...\n");
	
	// Radioberry control using SPI Mode Pins
	/* These are RP1 pin function selectors, not SPI transactions. Sample
	 * pins are subsequently switched to the PIO function during stream setup. */
	setPinMode(RPI_SPI_CE0,  GPIO_FUNC_SPI);
	setPinMode(RPI_SPI_CE1,  GPIO_FUNC_SPI);

	setPinMode(RPI_SPI_SCLK, GPIO_FUNC_SPI);
	setPinMode(RPI_SPI_MISO, GPIO_FUNC_SPI);
	setPinMode(RPI_SPI_MOSI, GPIO_FUNC_SPI);
	

	//RX IO init.
	initialize_gpio_for_output(RPI_RX_CLK);
	clr_pin(RPI_RX_CLK); 			// init pi-rx_clk
	initialize_gpio_for_input(25);	// available samples.
	
	initialize_gpio_for_input(21);	// rx iq data
	initialize_gpio_for_input(20);	// rx iq data
	initialize_gpio_for_input(19);	// rx iq data
	initialize_gpio_for_input(18);	// rx iq data
	
	//TX IO init.	
    initialize_gpio_for_input(12);
    initialize_gpio_for_output(4);
	clr_pin(4);
    initialize_gpio_for_output(5);
	clr_pin(5);

	printk(KERN_INFO "GPIO ready for using control and the rx and tx streaming...\n");
	
	return 0;
}


/*
 * rb2_trx_control
 *
 * Perform one synchronous full-duplex SPI transaction of cnt bytes.
 * The caller may supply the same buffer for transmit and receive; control
 * ioctl uses six bytes. Return the SPI status. This function requires a
 * successfully probed spi_ctrl_dev and can sleep in the SPI subsystem.
 */
int rb2_trx_control(char *txBuf, char *rxBuf, unsigned cnt){
	
	/* One full-duplex transfer: each transmitted byte clocks in a response byte.
	 * The control ioctl uses the same six-byte storage for TX and RX. */
	struct spi_transfer t = {
		.tx_buf = txBuf,
		.rx_buf = rxBuf,
		.len = cnt,
	};
	struct spi_message m;

	spi_message_init(&m);
	/* Attach the transfer descriptor; its stack lifetime lasts through spi_sync. */
	spi_message_add_tail(&t, &m);
	
	// Send and receive the control message to/from the Radioberry SPI device
	/* Synchronous call can sleep until the controller completes or reports error. */
	int ret = spi_sync(spi_ctrl_dev, &m);
	if (ret) {
		pr_err("SPI transfer failed\n");
		return ret;
	} 
	
   return 0;
}