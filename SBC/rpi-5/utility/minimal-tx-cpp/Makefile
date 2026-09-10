CXX ?= c++
CPPFLAGS += -I../../device_driver/pio-mode/driver/include
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion
LDLIBS += -pthread

.PHONY: all test clean
all: radioberry-tx

radioberry-tx: tx.cpp ../../device_driver/pio-mode/driver/include/radioberry_ioctl.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tx.cpp $(LDFLAGS) $(LDLIBS) -o $@

test: radioberry-tx
	python3 tests.py ./radioberry-tx

clean:
	rm -f radioberry-tx
