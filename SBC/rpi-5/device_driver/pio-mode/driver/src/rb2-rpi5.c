/*

Raspberry Pi 5 

GPIO support.

*/


#include "rb2-rpi5.h"

volatile uint32_t *PERIBase = NULL;
volatile uint32_t *GPIOBase = NULL;
volatile uint32_t *RIOBase = NULL;
volatile uint32_t *PADBase = NULL;

uint32_t *Pad = NULL;


/*
 * initialize_rpi
 *
 * Map the hard-coded RP1 peripheral window and derive GPIO/RIO/PAD views.
 * Offsets are bytes divided by four because base pointers are uint32_t*.
 * Mapping failure is logged, but derived pointers are still calculated.
 */
int initialize_rpi(void) {
	
	int ret = 0;
	
	printk(KERN_INFO "make GPIO ready for use\n");
		 
	if((PERIBase = ioremap(PERI_BASE, MEM_SIZE)) == NULL){
		printk(KERN_INFO "Failed mapping registers...\n");
		ret = -1;
	}
	
	GPIOBase = PERIBase + 0xD0000 / 4;
	RIOBase  = PERIBase + 0xE0000 / 4;
	PADBase  = PERIBase + 0xF0000 / 4;
	Pad = PADBase + 1;
	
	printk(KERN_INFO "GPIO ready for use\n");
	
	return ret;
}

/*
 * deinitialize_rpi
 *
 * Unmap the RP1 peripheral window previously mapped by initialize_rpi().
 */
void deinitialize_rpi(void) {	
	iounmap(PERIBase);
	printk(KERN_INFO "GPIO resources free. \n");
}

/*
 * initialize_gpio_for_output
 *
 * Select RIO function and output pad settings, then enable the pin driver.
 */
void initialize_gpio_for_output(uint32_t pin) {
    GPIO[pin].ctrl=GPIO_FUNC_RIO;
    Pad[pin] = PAD_FUNC_OUT;
    rioSET->oe = 0x01<<pin; 	// output driver
}

/*
 * initialize_gpio_for_input
 *
 * Select RIO function and input pad settings, then disable the output driver.
 */
void initialize_gpio_for_input(uint32_t pin) {
	GPIO[pin].ctrl=GPIO_FUNC_RIO;
    Pad[pin] = PAD_FUNC_IN;
    rioCLR->oe = 0x01<<pin; 	// high impedance
}

/*
 * set_pin
 *
 * Enable output and set this pin using the RIO atomic SET register alias.
 */
void set_pin(uint32_t pin) {
	rioSET->oe = 0x01<<pin; 	// output driver
	rioSET->out = 0x01<<pin;
}

/*
 * clr_pin
 *
 * Enable output and clear this pin using the RIO atomic CLR register alias.
 */
void clr_pin(uint32_t pin) {
	rioSET->oe  = 0x01<<pin; 	// output driver
	rioCLR->out = 0x01<<pin;
}

/*
 * read_pin
 *
 * Read one bit from the RIO input register. Pin is a GPIO number.
 */
uint32_t read_pin(uint32_t pin) {
	return (rio->in>>pin & 0x01);
}

/*
 * read_pin_all
 *
 * Return the full RIO input bitmap for all represented GPIO pins.
 */
uint32_t read_pin_all(void) {
	return rio->in;
}

/*
 * setPinMode
 *
 * Write the pin function selector directly. This does not configure pads
 * or output-enable state by itself.
 */
void setPinMode(uint32_t pin, uint32_t mode)
{
	GPIO[pin].ctrl = mode;
}