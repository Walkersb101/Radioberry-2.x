// Minimal userspace transmitter for the Radioberry Pi 5 PIO kernel driver.
// The kernel module owns PIO/DMA. This program supplies sample words and,
// optionally, explicit gateware control transactions. See README.md.
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "radioberry_ioctl.h"

namespace {
constexpr std::size_t block_bytes = 16384; // The current kernel's TX DMA threshold.
constexpr std::size_t words_per_block = block_bytes / 4;
volatile std::sig_atomic_t interrupted = 0;
void on_signal(int) { interrupted = 1; }

struct Control { std::uint8_t board, command; std::uint32_t data; };
struct Options {
    std::string bits, output, device, mode = "bpsk";
    std::uint64_t samples_per_bit = 48, repeat = 1, tail = 0;
    std::uint64_t amplitude = 4096, sample_rate = 48000, hold_ms = 0;
    bool configured = false, have_hold = false;
    std::vector<Control> start, stop;
};

const char* usage = R"(Usage:
  radioberry-tx BITS --output samples.iq [options]
  radioberry-tx BITS --device /dev/radioberry --hold-ms MS
      (--configured | --start-control HEX:HEX:HEX --stop-control HEX:HEX:HEX)
      [options]

BITS must contain only 0 and 1. Default: rectangular BPSK, 0=-I, 1=+I, Q=0.
  --mode bpsk|raw         raw sends literal 32-bit words; length must divide by 32
  --samples-per-bit N     BPSK pairs per bit (default 48)
  --amplitude N           BPSK signed amplitude, 1..32767 (default 4096)
  --repeat N              Repeat the bit string N times (default 1)
  --tail-samples N        Append N zero words before block padding (default 0)
  --sample-rate N         Assumed pairs/second for reporting ONLY (default 48000)
  --output PATH          Write binary transport bytes to a file; no device access
  --device PATH          Write to the existing kernel character device
  --configured           You have already configured the gateware; no controls sent
  --start-control B:C:D   Repeatable raw hexadecimal board:command:data ioctl
  --stop-control B:C:D    Repeatable shutdown ioctl; required with start controls
  --hold-ms MS            Explicit wait AFTER queuing; not verified RF completion
  --help                 Show this text

Each output block is 16 KiB; partial blocks are zero padded. Device write success
means queued, not transmitted. No frequency/rate/PA configuration is guessed.
)";

std::uint64_t number(std::string_view s, int base, const char* name) {
    if (base == 16 && s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s.remove_prefix(2);
    std::uint64_t value = 0;
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (s.empty() || result.ec != std::errc{} || result.ptr != s.data() + s.size())
        throw std::runtime_error(std::string("Invalid ") + name);
    return value;
}
Control control(const std::string& s) {
    const auto a = s.find(':');
    const auto b = a == std::string::npos ? a : s.find(':', a + 1);
    if (a == std::string::npos || b == std::string::npos || s.find(':', b+1) != std::string::npos)
        throw std::runtime_error("Control must be hexadecimal board:command:data");
    auto board = number(std::string_view(s).substr(0,a),16,"board control");
    auto cmd = number(std::string_view(s).substr(a+1,b-a-1),16,"command");
    auto data = number(std::string_view(s).substr(b+1),16,"command data");
    if (board > 255 || cmd > 255 || data > UINT32_MAX)
        throw std::runtime_error("Control exceeds 8:8:32-bit field sizes");
    return {static_cast<std::uint8_t>(board),static_cast<std::uint8_t>(cmd),
            static_cast<std::uint32_t>(data)};
}
Options parse(int argc, char** argv) {
    Options o;
    if (argc < 2) throw std::runtime_error("Missing bit string; use --help");
    o.bits = argv[1];
    if (o.bits.empty() || o.bits.find_first_not_of("01") != std::string::npos)
        throw std::runtime_error("BITS must be a nonempty string of 0 and 1");
    for (int i=2; i<argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--configured") { o.configured = true; continue; }
        if (i+1 == argc) throw std::runtime_error("Missing value for " + arg);
        std::string value = argv[++i];
        if (arg == "--output") o.output = value;
        else if (arg == "--device") o.device = value;
        else if (arg == "--mode") o.mode = value;
        else if (arg == "--start-control") o.start.push_back(control(value));
        else if (arg == "--stop-control") o.stop.push_back(control(value));
        else {
            auto n = number(value,10,arg.c_str());
            if (arg == "--samples-per-bit") o.samples_per_bit = n;
            else if (arg == "--amplitude") o.amplitude = n;
            else if (arg == "--repeat") o.repeat = n;
            else if (arg == "--tail-samples") o.tail = n;
            else if (arg == "--sample-rate") o.sample_rate = n;
            else if (arg == "--hold-ms") { o.hold_ms = n; o.have_hold = true; }
            else throw std::runtime_error("Unknown option: " + arg);
        }
    }
    if (o.output.empty() == o.device.empty())
        throw std::runtime_error("Choose exactly one of --output or --device");
    if (o.mode != "bpsk" && o.mode != "raw") throw std::runtime_error("Unknown mode");
    if (o.mode == "raw" && o.bits.size()%32 != 0)
        throw std::runtime_error("Raw mode requires complete 32-bit words");
    if (!o.samples_per_bit || !o.repeat || !o.sample_rate || !o.amplitude || o.amplitude>32767)
        throw std::runtime_error("Counts/rate must be positive; amplitude must be 1..32767");
    if (o.hold_ms>3600000) throw std::runtime_error("hold-ms must be <= 3600000");
    if (!o.device.empty()) {
        if (!o.have_hold) throw std::runtime_error("Device mode requires explicit --hold-ms");
        if (o.configured) {
            if (!o.start.empty() || !o.stop.empty())
                throw std::runtime_error("--configured cannot be combined with control commands");
        } else if (o.start.empty() || o.stop.empty()) {
            throw std::runtime_error("Provide both start/stop controls, or --configured");
        }
    } else if (o.configured || !o.start.empty() || !o.stop.empty() || o.have_hold) {
        throw std::runtime_error("Device controls/hold are not used with --output");
    }
    return o;
}
std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if (b && a > std::numeric_limits<std::uint64_t>::max()/b)
        throw std::runtime_error("Requested sample count overflows");
    return a*b;
}
struct Layout { std::uint64_t payload, total, padded; };
Layout layout(const Options& o) {
    const auto per_message = o.mode == "raw" ? o.bits.size()/32 :
        multiply(o.bits.size(),o.samples_per_bit);
    const auto payload = multiply(per_message,o.repeat);
    const auto max = std::numeric_limits<std::uint64_t>::max();
    if (o.tail>max-payload || payload+o.tail>max-(words_per_block-1))
        throw std::runtime_error("Requested tail/padding overflows");
    const auto total = payload+o.tail;
    const auto padded = ((total+words_per_block-1)/words_per_block)*words_per_block;
    (void)multiply(padded,4); // Also bound the byte-count report.
    return {payload,total,padded};
}

// Produces a numerical FIFO word; packing below controls the wire byte order.
std::uint32_t word_at(const Options& o, const Layout& l, std::uint64_t n) {
    if (n >= l.payload) return 0; // Zero tail and padding are silence, not bit 0.
    if (o.mode == "raw") {
        const std::size_t start = static_cast<std::size_t>(n%(o.bits.size()/32))*32;
        std::uint32_t w = 0;
        for (std::size_t j=0; j<32; ++j) w = (w<<1) | (o.bits[start+j]=='1' ? 1u : 0u);
        return w;
    }
    const auto bit = static_cast<std::size_t>((n/o.samples_per_bit)%o.bits.size());
    const auto amplitude = static_cast<std::int16_t>(o.amplitude);
    const auto i = static_cast<std::int16_t>(o.bits[bit]=='1' ? amplitude : -amplitude);
    return static_cast<std::uint32_t>(static_cast<std::uint16_t>(i))<<16; // Q = 0.
}
void fill_block(const Options& o, const Layout& l, std::uint64_t start,
                std::array<std::uint8_t,block_bytes>& out) {
    for (std::size_t j=0; j<words_per_block; ++j) {
        const auto w = word_at(o,l,start+j);
        out[4*j] = static_cast<std::uint8_t>(w>>24);
        out[4*j+1] = static_cast<std::uint8_t>(w>>16);
        out[4*j+2] = static_cast<std::uint8_t>(w>>8);
        out[4*j+3] = static_cast<std::uint8_t>(w);
    }
}
std::string failure(const char* operation) {
    return std::string(operation)+": "+std::strerror(errno);
}
void send_control(int fd, const Control& c) {
    static_assert(sizeof(int)==4 && sizeof(rb_info_arg_t)==36, "Unsupported ioctl ABI");
    rb_info_arg_t info{}; // Fresh input each time: ioctl overwrites the structure.
    info.rb_command = c.board;
    info.command = c.command;
    std::memcpy(&info.command_data,&c.data,sizeof(c.data));
    if (::ioctl(fd,RADIOBERRY_IOC_COMMAND,&info)<0)
        throw std::runtime_error(failure("ioctl"));
    // The current kernel ignores SPI errors, so success is not a verified ACK.
}

class Sink {
    int fd_ = -1;
    const Options& options_;
    bool stop_needed_ = false;
public:
    explicit Sink(const Options& o) : options_(o) {
        if (!o.device.empty()) {
            fd_ = ::open(o.device.c_str(),O_RDWR|O_CLOEXEC);
            if (fd_>=0) {
                struct stat st{};
                if (::fstat(fd_,&st)<0 || !S_ISCHR(st.st_mode)) {
                    ::close(fd_); fd_=-1;
                    throw std::runtime_error("--device must name a character device");
                }
            }
        } else {
            // Exclusive creation prevents accidental overwrites during experiments.
            fd_ = ::open(o.output.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
        }
        if (fd_<0) throw std::runtime_error(failure("open"));
    }
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    ~Sink() {
        if (fd_>=0) { stop(); ::close(fd_); }
    }
    void start() {
        stop_needed_ = !options_.stop.empty(); // Cleanup even after a partial start.
        for (const auto& c : options_.start) {
            if (interrupted) throw std::runtime_error("Interrupted before startup completed");
            send_control(fd_,c);
        }
    }
    bool stop() noexcept {
        if (!stop_needed_) return true;
        stop_needed_ = false; // Do not issue the sequence twice from destructor.
        bool ok = true;
        for (const auto& c : options_.stop) {
            try { send_control(fd_,c); }
            catch (const std::exception& e) { std::cerr<<"Stop control failed: "<<e.what()<<'\n'; ok=false; }
        }
        return ok;
    }
    void write_block(const std::array<std::uint8_t,block_bytes>& bytes) {
        std::size_t offset = 0;
        while (offset<bytes.size()) {
            if (interrupted) throw std::runtime_error("Interrupted; queued hardware samples may remain");
            const auto n = ::write(fd_,bytes.data()+offset,bytes.size()-offset);
            if (n<0) {
                // Do not repeatedly retry EAGAIN: the existing driver's timeout
                // path leaks memory. Fail and run configured shutdown instead.
                if (errno==EINTR && options_.device.empty() && !interrupted) continue;
                throw std::runtime_error(failure("write"));
            }
            if (!n) throw std::runtime_error("write made no progress");
            if (!options_.device.empty() && n%4 != 0)
                throw std::runtime_error("Driver accepted a partial sample word; aborting");
            offset += static_cast<std::size_t>(n);
        }
    }
};
} // namespace

int main(int argc, char** argv) {
    if (argc==2 && std::string(argv[1])=="--help") { std::cout<<usage; return 0; }
    try {
        const auto o = parse(argc,argv);
        const auto l = layout(o); // Validate arithmetic before touching any output.
        std::signal(SIGINT,on_signal);
        std::signal(SIGTERM,on_signal);
        std::cerr<<"Mode: "<<o.mode<<"; payload words: "<<l.payload
                 <<"; tail words: "<<o.tail<<"; padding words: "<<(l.padded-l.total)
                 <<"; total bytes: "<<multiply(l.padded,4)<<'\n';
        std::cerr<<"Nominal duration at assumed "<<o.sample_rate<<" words/s: "
                 <<std::fixed<<std::setprecision(3)
                 <<(1000.0*static_cast<double>(l.padded)/static_cast<double>(o.sample_rate))
                 <<" ms (rate is not configured by this option)\n";
        Sink sink(o);
        sink.start();
        std::array<std::uint8_t,block_bytes> block{};
        for (std::uint64_t n=0; n<l.padded; n+=words_per_block) {
            fill_block(o,l,n,block);
            sink.write_block(block);
        }
        if (!o.device.empty()) {
            std::cerr<<"Queued. Waiting "<<o.hold_ms<<" ms; no hardware completion is available.\n";
            const auto deadline = std::chrono::steady_clock::now()+std::chrono::milliseconds(o.hold_ms);
            while (!interrupted && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (o.configured)
                std::cerr<<"No stop control sent (--configured); gateware state is your responsibility.\n";
        } else std::cerr<<"Wrote "<<o.output<<'\n';
        if (!sink.stop()) return 1;
        return interrupted ? 130 : 0;
    } catch (const std::exception& e) {
        std::cerr<<"radioberry-tx: "<<e.what()<<'\n';
        return interrupted ? 130 : 1;
    }
}
