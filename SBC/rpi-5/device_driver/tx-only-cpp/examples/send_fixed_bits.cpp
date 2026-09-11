// SPDX-License-Identifier: GPL-2.0-only
/*
 * Smallest useful caller: no command-line interface and no parser.
 * Edit kBits, rebuild, then run the program. The write system call crosses
 * into radioberry_tx.ko; ASCII '0'/'1' characters never reach the FPGA.
 */
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <unistd.h>

namespace {
constexpr char kDevice[] = "/dev/radioberry-tx";
constexpr char kBits[] = "10110011100011110000111101010101";
}

int main()
{
    const int fd = ::open(kDevice, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        std::cerr << "open " << kDevice << ": " << std::strerror(errno) << '\n';
        return 1;
    }

    constexpr std::size_t total = sizeof(kBits) - 1; // Exclude C string NUL.
    std::size_t sent = 0;
    while (sent < total) {
        const ssize_t result = ::write(fd, kBits + sent, total - sent);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0) {
            std::cerr << "write: " << (result < 0 ? std::strerror(errno) : "zero bytes") << '\n';
            ::close(fd);
            return 1;
        }
        sent += static_cast<std::size_t>(result);
    }

    if (::close(fd) < 0) {
        std::cerr << "close: " << std::strerror(errno) << '\n';
        return 1;
    }
    std::cout << "Queued " << sent << " bits\n";
    return 0;
}
