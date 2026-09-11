ifneq ($(KERNELRELEASE),)

obj-m += radioberry_tx.o
radioberry_tx-y := linux_shim.o tx_core.o
targets += tx_core.o

# The RP1 PIO API is maintained outside a normal upstream kernel tree. Set
# RP1_HEADERS if pio_rp1.h is not already installed as <linux/pio_rp1.h>.
ccflags-y += $(if $(RP1_HEADERS),-I$(RP1_HEADERS))

# Kbuild has no general C++ kernel rule. This one compiles only the isolated,
# freestanding core. It deliberately sees bridge.h, not Linux or STL headers.
RB_CXX ?= $(CROSS_COMPILE)g++
RB_CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror \
	-ffreestanding -nostdinc -nostdinc++ -fno-builtin \
	-fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-use-cxa-atexit -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-fno-stack-protector -fno-strict-aliasing -fno-common \
	-fno-delete-null-pointer-checks -fno-pic -fno-pie -mcmodel=large \
	-mgeneral-regs-only

quiet_cmd_rb_cxx = CXX [M] $@
      cmd_rb_cxx = $(RB_CXX) $(RB_CXXFLAGS) -c -o $@ $<

$(obj)/tx_core.o: $(src)/tx_core.cpp $(src)/bridge.h FORCE
	$(call if_changed,rb_cxx)

else

KERNEL_HEADERS ?= /lib/modules/$(shell uname -r)/build
RP1_SYMVERS ?= /home/pi/git/rp1-driver/Module.symvers

.PHONY: all module overlay example test clean install uninstall

all: module overlay example

module:
	$(MAKE) -C $(KERNEL_HEADERS) M=$(CURDIR) \
		KBUILD_EXTRA_SYMBOLS=$(RP1_SYMVERS) modules

overlay: radioberry-tx.dtbo

radioberry-tx.dtbo: radioberry-tx.dts
	dtc -@ -I dts -O dtb -o $@ $<

example: examples/send_fixed_bits

examples/send_fixed_bits: examples/send_fixed_bits.cpp tx_ioctl.h
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Werror -I. -o $@ $<

test:
	$(MAKE) -C tests test

clean:
	$(MAKE) -C $(KERNEL_HEADERS) M=$(CURDIR) clean
	$(RM) radioberry-tx.dtbo examples/send_fixed_bits
	$(MAKE) -C tests clean

install: radioberry_tx.ko radioberry-tx.dtbo
	install -m 0644 radioberry-tx.dtbo /boot/firmware/overlays/
	insmod radioberry_tx.ko

uninstall:
	rmmod radioberry_tx

endif
