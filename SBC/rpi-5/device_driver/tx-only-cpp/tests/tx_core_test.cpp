// SPDX-License-Identifier: GPL-2.0-only
#include "../bridge.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
std::vector<std::uint32_t> words;
int full_before_success;
int pause_error;
int close_calls;
int unpin_calls;

void expect(bool value, const char *message)
{
    if (!value) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}
}

extern "C" int rb_hw_open(void) { return 0; }
extern "C" int rb_hw_claim(void) { return 2; }
extern "C" int rb_hw_program(const unsigned short *, unsigned int n, unsigned int o)
{ return n == 9 && o == 0 ? 0 : -5; }
extern "C" int rb_hw_pin(unsigned int, int, int, unsigned int) { return 0; }
extern "C" void rb_hw_unpin(unsigned int) { ++unpin_calls; }
extern "C" int rb_hw_config(unsigned int sm, const rb_pio_config *) { return sm == 2 ? 0 : -5; }
extern "C" int rb_hw_enable(unsigned int, int) { return 0; }
extern "C" int rb_hw_try_put(unsigned int, unsigned int word)
{
    if (full_before_success-- > 0)
        return 1;
    words.push_back(word);
    return 0;
}
extern "C" int rb_hw_pause(void) { return pause_error; }
extern "C" void rb_hw_close(void) { ++close_calls; }

int main()
{
    expect(rb_core_start() == 0, "core starts");
    expect(rb_core_write("101", 3) == 3, "three bits accepted");
    expect(words.size() == 1 && words[0] == 0xa0000000U,
           "short write is MSB aligned and zero padded");

    words.clear();
    const char thirty_three[] = "111111111111111111111111111111111";
    expect(rb_core_write(thirty_three, 33) == 33, "33 bits accepted");
    expect(words.size() == 2 && words[0] == 0xffffffffU && words[1] == 0x80000000U,
           "32-bit boundary packs into two words");

    words.clear();
    expect(rb_core_write("10x1", 4) == -22 && words.empty(),
           "whole input is validated before output");

    full_before_success = 2;
    expect(rb_core_write("1", 1) == 1 && words.back() == 0x80000000U,
           "FIFO backpressure is retried");

    rb_core_stop();
    expect(close_calls == 1 && unpin_calls == 3, "cleanup releases client and all pins");
    expect(rb_core_write("1", 1) == -19, "write after stop fails");
    std::cout << "all tx_core tests passed\n";
}
