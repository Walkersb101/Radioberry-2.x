CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Werror

.PHONY: test clean

test: tx_core_test
	./tx_core_test
	@if nm tx_core.o | grep -Eq '(__cxa|_Zn|_GLOBAL__sub_I|init_array)'; then \
		echo "unexpected C++ runtime dependency"; exit 1; \
	fi

tx_core_test: tx_core_test.cpp tx_core.o
	$(CXX) $(CXXFLAGS) -o $@ $^

tx_core.o: ../tx_core.cpp ../bridge.h
	$(CXX) $(CXXFLAGS) -ffreestanding -nostdinc -nostdinc++ -fno-builtin \
		-fno-exceptions -fno-rtti \
		-fno-threadsafe-statics -fno-use-cxa-atexit -c -o $@ $<

clean:
	$(RM) tx_core_test tx_core.o
