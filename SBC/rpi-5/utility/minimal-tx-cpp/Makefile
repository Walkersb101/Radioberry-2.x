# Use the selected compiler, or c++ if make has no existing CXX value.
# GNU make may already define CXX as g++; callers can override it explicitly.
CXX ?= c++
# Add the existing userspace ioctl header directory. These are preprocessing
# options; the program does not include the PIO kernel implementation headers.
CPPFLAGS += -I../../device_driver/pio-mode/driver/include
# Default optimisation/debug settings. -g helps source-level debugging even
# when -O2 changes the machine-code layout.
CXXFLAGS ?= -O2 -g
# Require C++17 and enable diagnostics, particularly signed/unsigned and
# narrowing conversions, because this program manipulates exact bit widths.
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
# Use the toolchain pthread option for the C++ thread/sleep facility.
# This source does not create a background worker thread.
LDLIBS += -pthread

# These names are commands, not real output files. A file named test must
# not prevent make test from running.
.PHONY: all test clean
# Default target: build the one executable.
all: radioberry-tx

# Rebuild if source or shared ABI header changes. $@ below means target name.
# CPPFLAGS/CXXFLAGS affect compilation; LDFLAGS/LDLIBS support linker overrides.
# Recipe lines must begin with a TAB, not spaces.
radioberry-tx: tx.cpp ../../device_driver/pio-mode/driver/include/radioberry_ioctl.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tx.cpp $(LDFLAGS) $(LDLIBS) -o $@

# Build first, then execute independent host-only black-box tests.
# Python receives the executable path as argv[1]. No radio device is opened.
test: radioberry-tx
	python3 tests.py ./radioberry-tx

# Remove only the built executable; keep all source and generated sample files.
clean:
	rm -f radioberry-tx
