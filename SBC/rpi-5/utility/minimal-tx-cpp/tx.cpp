/*
 * LEARNING EDITION: bit strings to Radioberry transport words
 * =========================================================
 * Read LEARNING_GUIDE.md alongside this file. Suggested reading order:
 *   1. Options and Layout: name the inputs and count the output words.
 *   2. word_at(): turn one bit/symbol position into one I/Q word.
 *   3. fill_block(): turn numerical words into an explicit byte sequence.
 *   4. Sink::write_block(): cross the userspace/kernel boundary.
 *   5. main(): connect those pieces and manage lifetime/errors.
 *   6. parse(), control(), send_control(): configuration and the existing ABI.
 *
 * A 'bit' is one character of the input message. A BPSK 'symbol' holds the
 * chosen sign for samples_per_bit sample instants. One sample instant means
 * an I/Q pair, packed as a 32-bit word. One block is 4096 such words.
 * In raw mode no modulation occurs: each 32 input characters form one word.
 *
 * This executable does not manipulate GPIO registers. write() enters the
 * existing kernel module, which queues bytes and uses DMA/PIO to send them.
 * Kernel queue acceptance, FPGA consumption and RF completion are different
 * events. The present ABI reports only the first of these to this program.
 *
 * The comments describe existing behaviour, including limitations, rather
 * than silently fixing it. No source tokens were changed in this edition.
 */
// Minimal userspace transmitter for the Radioberry Pi 5 PIO kernel driver.
// The kernel module owns PIO/DMA. This program supplies sample words and,
// optionally, explicit gateware control transactions. See README.md.
#include <array> // Fixed-size, contiguous 16 KiB output buffer, with its size in the type.
#include <cerrno> // errno and EINTR from failed POSIX calls; meaningful only after failure.
#include <charconv> // from_chars: strict, locale-independent integer parsing without streams.
#include <chrono> // Typed durations and a monotonic clock for the explicit post-write hold.
#include <csignal> // Signal handler registration and sig_atomic_t for a simple stop flag.
#include <cstdint> // Exact-width integers for 16-bit I/Q and 32-bit transport words.
#include <cstring> // memcpy for the ioctl bit pattern; strerror for readable errno messages.
#include <fcntl.h> // POSIX open() and file-open flags such as O_EXCL.
#include <iomanip> // fixed/setprecision formatting of the diagnostic duration.
#include <iostream> // Human-readable help/status/errors, not the binary sample transport.
#include <limits> // Maximum uint64_t value for checked size arithmetic.
#include <stdexcept> // runtime_error for failures caught at the main() boundary.
#include <string> // Owning copies of arguments, paths and diagnostic text.
#include <string_view> // Non-owning slices used while parsing a still-live argument string.
#include <sys/ioctl.h> // Device-specific ioctl() calls through the open descriptor.
#include <sys/stat.h> // fstat() and S_ISCHR to check the opened object type.
#include <thread> // sleep_for() yields the host thread during the hold interval.
#include <unistd.h> // POSIX write()/close(), rather than C++ formatted output.
#include <vector> // Variable-length sequences of explicit start/stop control commands.
#include "radioberry_ioctl.h" // Shared ABI from this repository, not a new protocol.

/*
 * An unnamed namespace gives these helpers internal linkage: another source
 * file can use the same helper names without a linker collision. main() remains
 * outside it because the runtime needs the program entry point.
 */
namespace {
/*
 * constexpr makes this a compile-time value, usable as an array size.
 * std::size_t is the unsigned type used for sizes and indexing within memory.
 * This particular size is imposed by the existing TX driver, not by C++ or BPSK.
 */
constexpr std::size_t block_bytes = 16384; // The current kernel's TX DMA threshold.
/*
 * Four bytes per I/Q pair: two for I and two for Q. Thus 16384/4 = 4096.
 * Raw mode uses the same word size even though it skips the modulation step.
 */
constexpr std::size_t words_per_block = block_bytes / 4;
/*
 * Only a signal handler and ordinary program code share this flag.
 * sig_atomic_t is intended for this simple signal communication pattern.
 * volatile is not a general thread-synchronisation mechanism or a DMA barrier.
 */
volatile std::sig_atomic_t interrupted = 0;
/*
 * Keep the signal handler minimal: do not allocate, print, throw, close or
 * issue control calls here. Normal code observes the flag and performs cleanup.
 * Blocking syscalls may still delay observation; this is not instant TX abort.
 */
void on_signal(int) { interrupted = 1; }

/*
 * A plain value object representing one explicit command, before ioctl ABI
 * conversion. board and command each fit one byte; data holds all 32 data bits.
 * These fields are not a packed network structure and are not written directly.
 */
struct Control { std::uint8_t board, command; std::uint32_t data; };
/*
 * Collect command-line settings separately from running device state.
 * Default member initialisers apply when parse() constructs Options o.
 * uint64_t is used for potentially large counts; actual block indices use size_t.
 */
struct Options {
    /*
     * bits owns the input characters. Exactly one output destination must be set.
     * The default modulation maps logical zero to negative I, not to silence.
     */
    std::string bits, output, device, mode = "bpsk";
    /*
     * samples_per_bit sets symbol length. repeat repeats the whole message.
     * tail appends explicit zero words after the message and before block padding.
     */
    std::uint64_t samples_per_bit = 48, repeat = 1, tail = 0;
    /*
     * sample_rate is a reporting assumption only. hold_ms is an explicit wait
     * after the final successful queue write, not a hardware completion deadline.
     */
    std::uint64_t amplitude = 4096, sample_rate = 48000, hold_ms = 0;
    /*
     * configured means an external owner has already set the radio state.
     * have_hold distinguishes omitted --hold-ms from explicitly requested zero.
     */
    bool configured = false, have_hold = false;
    /*
     * Vectors preserve the order in which each type of control was supplied.
     * All start controls run before data; all stop controls run after it or on error.
     */
    std::vector<Control> start, stop;
};

/*
 * A raw string literal preserves line breaks and quotes without escaping.
 * This text is help only: changing it does not change parsing or defaults.
 */
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

/*
 * Parse an unsigned integer from a non-owning character range.
 * Inputs: view, numeric base (10 or 16 here), and a name for error reporting.
 * Returns the value or throws. No I/O or radio state changes occur.
 * The view must not outlive its source string; all callers use it immediately.
 */
std::uint64_t number(std::string_view s, int base, const char* name) {
    /*
     * from_chars does not consume a 0x prefix for us. Strip it only for hex
     * fields, after confirming enough characters exist for the indexed accesses.
     * remove_prefix changes the view boundaries without modifying the source.
     */
    if (base == 16 && s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        s.remove_prefix(2);
    std::uint64_t value = 0;
    /*
     * Parse the half-open range [begin,end). result.ec records conversion errors;
     * result.ptr points just after the consumed number. Both need checking.
     */
    const auto result = std::from_chars(s.data(), s.data() + s.size(), value, base);
    /*
     * Full consumption rejects inputs such as 12garbage. Empty strings, leading
     * spaces and signed negative text are also invalid for these unsigned fields.
     * The parser does not rely on a trailing NUL because it supplies the range.
     */
    if (s.empty() || result.ec != std::errc{} || result.ptr != s.data() + s.size())
        throw std::runtime_error(std::string("Invalid ") + name);
    return value;
}
/*
 * Split one BOARD:COMMAND:DATA argument into exactly three hexadecimal
 * fields, validate widths while still uint64_t, then narrow into Control.
 * No command semantics are inferred from the values.
 */
Control control(const std::string& s) {
    /*
     * find returns string::npos when no delimiter exists. Search for the second
     * colon only if the first was found, rather than adding one to npos.
     */
    const auto a = s.find(':');
    const auto b = a == std::string::npos ? a : s.find(':', a + 1);
    /*
     * Exactly two delimiters are permitted. Empty fields are rejected later by
     * number(), so 01::02 does not silently become a zero command.
     */
    if (a == std::string::npos || b == std::string::npos || s.find(':', b+1) != std::string::npos)
        throw std::runtime_error("Control must be hexadecimal board:command:data");
    /*
     * substr makes views of the fields, not new independent copies. All three
     * are parsed as hexadecimal, even if they contain only decimal-looking digits.
     */
    auto board = number(std::string_view(s).substr(0,a),16,"board control");
    auto cmd = number(std::string_view(s).substr(a+1,b-a-1),16,"command");
    auto data = number(std::string_view(s).substr(b+1),16,"command data");
    /*
     * Check before narrowing: converting 0x100 to uint8_t would otherwise keep
     * only its low eight bits. Validation prevents accidental command truncation.
     */
    if (board > 255 || cmd > 255 || data > UINT32_MAX)
        throw std::runtime_error("Control exceeds 8:8:32-bit field sizes");
    /*
     * Brace initialisation constructs the returned Control value. These casts
     * are safe because the preceding check established each target width.
     */
    return {static_cast<std::uint8_t>(board),static_cast<std::uint8_t>(cmd),
            static_cast<std::uint32_t>(data)};
}
/*
 * Interpret argc/argv and validate combinations before opening any path.
 * argv[0] is the executable name; argv[1] is BITS; later arguments are options.
 * Parsing returns an owning value, so subsequent code need not use argv.
 * Most duplicate scalar options use the last value; control options accumulate.
 */
Options parse(int argc, char** argv) {
    Options o;
    if (argc < 2) throw std::runtime_error("Missing bit string; use --help");
    /*
     * Copy the argument into owned storage. A string like 1010 contains four
     * ASCII characters here, not a single byte with binary value ten.
     */
    o.bits = argv[1];
    if (o.bits.empty() || o.bits.find_first_not_of("01") != std::string::npos)
        throw std::runtime_error("BITS must be a nonempty string of 0 and 1");
    /*
     * Walk option tokens. Most consume a following value, so their branch
     * advances i once here and the loop advances it again for the next option.
     */
    for (int i=2; i<argc; ++i) {
        std::string arg = argv[i];
        /*
         * This one flag consumes no following value. continue skips the value parser.
         */
        if (arg == "--configured") { o.configured = true; continue; }
        if (i+1 == argc) throw std::runtime_error("Missing value for " + arg);
        /*
         * The missing-value check above guarantees this indexed argument exists.
         */
        std::string value = argv[++i];
        if (arg == "--output") o.output = value;
        else if (arg == "--device") o.device = value;
        else if (arg == "--mode") o.mode = value;
        else if (arg == "--start-control") o.start.push_back(control(value));
        else if (arg == "--stop-control") o.stop.push_back(control(value));
        else {
            /*
             * All count/rate/amplitude/hold values use decimal, unlike control fields.
             * An unknown option can first fail numeric conversion if its value is not numeric.
             */
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
    /*
     * Truth table: both empty means no destination; both nonempty means two.
     * Equality detects exactly those invalid cases, leaving precisely one selected.
     */
    if (o.output.empty() == o.device.empty())
        throw std::runtime_error("Choose exactly one of --output or --device");
    if (o.mode != "bpsk" && o.mode != "raw") throw std::runtime_error("Unknown mode");
    /*
     * Require complete literal words. BPSK accepts any positive number of bits
     * because it generates I/Q samples rather than grouping payload bits into words.
     */
    if (o.mode == "raw" && o.bits.size()%32 != 0)
        throw std::runtime_error("Raw mode requires complete 32-bit words");
    /*
     * Reject zero divisors and zero repetitions. Signed amplitude is restricted
     * to 1..32767 so both +A and -A fit in int16_t. These settings are checked
     * even in raw mode, although samples_per_bit/amplitude do not affect raw output.
     */
    if (!o.samples_per_bit || !o.repeat || !o.sample_rate || !o.amplitude || o.amplitude>32767)
        throw std::runtime_error("Counts/rate must be positive; amplitude must be 1..32767");
    if (o.hold_ms>3600000) throw std::runtime_error("hold-ms must be <= 3600000");
    /*
     * Device mode makes control ownership and the post-queue wait explicit.
     * File mode rejects these options to avoid giving a false impression of controls.
     */
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
/*
 * Checked unsigned multiplication. Testing after multiplication is too late:
 * unsigned overflow already wraps modulo 2^64. For b>0, a*b <= max is
 * equivalent to a <= floor(max/b). Short-circuit && avoids division by zero.
 */
std::uint64_t multiply(std::uint64_t a, std::uint64_t b) {
    if (b && a > std::numeric_limits<std::uint64_t>::max()/b)
        throw std::runtime_error("Requested sample count overflows");
    return a*b;
}
/*
 * Counts are in transport words, not bytes: payload excludes the zero tail;
 * total includes the tail; padded includes alignment to a complete DMA block.
 */
struct Layout { std::uint64_t payload, total, padded; };
/*
 * Derive all output lengths without allocating the full waveform.
 * This function can fail on overflow before an output file is created or any
 * radio controls are sent. It assumes parse() already validated nonzero counts.
 */
Layout layout(const Options& o) {
    /*
     * Raw mode has one word per 32 characters. BPSK has one word per sample
     * instant, hence bits times samples_per_bit. Repetition is applied afterward.
     */
    const auto per_message = o.mode == "raw" ? o.bits.size()/32 :
        multiply(o.bits.size(),o.samples_per_bit);
    const auto payload = multiply(per_message,o.repeat);
    const auto max = std::numeric_limits<std::uint64_t>::max();
    /*
     * Check payload+tail before evaluating it, then check the extra 4095 needed
     * by the rounding formula. Short-circuit || prevents the unsafe second sum.
     * The rounded result cannot exceed that already checked numerator.
     */
    if (o.tail>max-payload || payload+o.tail>max-(words_per_block-1))
        throw std::runtime_error("Requested tail/padding overflows");
    const auto total = payload+o.tail;
    /*
     * Integer ceiling to a multiple of 4096: ceil(total/4096)*4096.
     * Adding 4095 before integer division rounds upward, while an exact multiple
     * stays unchanged. Padding must not be confused with additional logical bits.
     */
    const auto padded = ((total+words_per_block-1)/words_per_block)*words_per_block;
    /*
     * Discard the numerical result but retain its overflow check. This protects
     * the later byte count even though generation itself works in word units.
     */
    (void)multiply(padded,4); // Also bound the byte-count report.
    return {payload,total,padded};
}

// Produces a numerical FIFO word; packing below controls the wire byte order.
/*
 * Random-access waveform generator: given absolute sample/word index n,
 * return the numerical 32-bit value at that position. No persistent phase or
 * bit counter is needed, so block boundaries cannot reset symbol timing.
 * The const references avoid copying the message or layout and prohibit edits.
 */
std::uint32_t word_at(const Options& o, const Layout& l, std::uint64_t n) {
    /*
     * Both explicit tail and automatic padding use zero I/Q. A BPSK zero bit
     * inside the payload instead produces I=-A and is a nonzero waveform value.
     */
    if (n >= l.payload) return 0; // Zero tail and padding are silence, not bit 0.
    if (o.mode == "raw") {
        /*
         * Modulo chooses a word within one message when repeat spans multiple copies.
         * Multiplying its word index by 32 gives the starting character position.
         */
        const std::size_t start = static_cast<std::size_t>(n%(o.bits.size()/32))*32;
        std::uint32_t w = 0;
        /*
         * Shift accumulated bits left and place the next input character in bit zero.
         * For the prefix 101: accumulator progresses 1, 2, 5. After 32 characters,
         * the first character occupies bit 31. Unsigned w makes bit shifts defined.
         */
        for (std::size_t j=0; j<32; ++j) w = (w<<1) | (o.bits[start+j]=='1' ? 1u : 0u);
        return w;
    }
    /*
     * Integer division by samples_per_bit holds the same bit for that many n
     * values; modulo message length wraps at the end of a repeat. Example with
     * 48 samples/bit: n=0..47 selects bit 0, n=48..95 selects bit 1.
     */
    const auto bit = static_cast<std::size_t>((n/o.samples_per_bit)%o.bits.size());
    const auto amplitude = static_cast<std::int16_t>(o.amplitude);
    /*
     * The conditional operator selects +A or -A. Integral promotion performs
     * unary minus in int here; the validated range permits conversion to int16_t.
     */
    const auto i = static_cast<std::int16_t>(o.bits[bit]=='1' ? amplitude : -amplitude);
    /*
     * First convert signed I to its modulo-65536 unsigned representation. Then
     * widen to 32 unsigned bits before shifting into the upper half. This avoids
     * left-shifting a negative signed value. Low 16 bits stay zero for Q.
     * At A=4096, I=+4096 becomes 0x10000000; I=-4096 becomes 0xF0000000.
     */
    return static_cast<std::uint32_t>(static_cast<std::uint16_t>(i))<<16; // Q = 0.
}
/*
 * Fill exactly one existing array by reference; no allocation or I/O.
 * start is an absolute WORD index. j is local to this block; 4*j is its BYTE
 * offset. The same function feeds files and hardware, enabling offline tests.
 */
void fill_block(const Options& o, const Layout& l, std::uint64_t start,
                std::array<std::uint8_t,block_bytes>& out) {
    for (std::size_t j=0; j<words_per_block; ++j) {
        /*
         * Use the global position, not j alone: symbols and repeats can cross blocks.
         */
        const auto w = word_at(o,l,start+j);
        /*
         * Extract bytes most significant first by shifting then narrowing. The
         * uint8_t conversion retains the low eight bits of the shifted value.
         * This explicitly emits I_hi,I_lo,Q_hi,Q_lo regardless of CPU endianness.
         * Writing the memory of uint32_t w directly would be wrong on little-endian CPUs.
         */
        out[4*j] = static_cast<std::uint8_t>(w>>24);
        out[4*j+1] = static_cast<std::uint8_t>(w>>16);
        out[4*j+2] = static_cast<std::uint8_t>(w>>8);
        out[4*j+3] = static_cast<std::uint8_t>(w);
    }
}
/*
 * Turn a POSIX errno into contextual text. Call only immediately after a
 * failed syscall: errno can be stale after success or overwritten by later calls.
 * This helper is used in normal code, never from the signal handler.
 */
std::string failure(const char* operation) {
    return std::string(operation)+": "+std::strerror(errno);
}
/*
 * Bridge the small Control value into the kernel ABI structure. fd is the
 * already-open character descriptor. ioctl performs a device-specific request;
 * it does not write sample bytes. This function throws on a syscall-level error.
 */
void send_control(int fd, const Control& c) {
    /*
     * Verify key layout assumptions at compile time. The current ABI uses nine
     * four-byte int fields, despite encoding a one-byte type in the ioctl number.
     * This does not prove kernel/architecture compatibility for every platform.
     */
    static_assert(sizeof(int)==4 && sizeof(rb_info_arg_t)==36, "Unsupported ioctl ABI");
    /*
     * Value-initialise all fields to zero. Build a fresh request every time because
     * the kernel overwrites the response. Caller-side zeroing does not repair the
     * kernel defect that copies its own uninitialised output fields back to us.
     */
    rb_info_arg_t info{}; // Fresh input each time: ioctl overwrites the structure.
    info.rb_command = c.board;
    info.command = c.command;
    /*
     * Preserve all 32 input bits, including a set high bit, in the ABI int field.
     * Both objects are four bytes on the asserted ABI. This is host-ABI copying;
     * the kernel later serialises SPI bytes. It is not wire-endian conversion.
     */
    std::memcpy(&info.command_data,&c.data,sizeof(c.data));
    /*
     * Leading :: explicitly selects the global POSIX function. Pass a pointer to
     * the structure; the kernel copies its contents across the userspace boundary.
     */
    if (::ioctl(fd,RADIOBERRY_IOC_COMMAND,&info)<0)
        throw std::runtime_error(failure("ioctl"));
    // The current kernel ignores SPI errors, so success is not a verified ACK.
}

/*
 * RAII resource owner: acquire the file descriptor in construction and release
 * it in destruction. The same interface writes either samples to disk or bytes
 * to the kernel. Only device mode can have start/stop controls.
 * options_ is a borrowed reference: main() must keep Options alive longer than Sink.
 */
class Sink {
    /*
     * A descriptor is a small process-local integer handle, not a hardware address.
     * Zero is valid, so -1 represents no owned descriptor.
     */
    int fd_ = -1;
    const Options& options_;
    bool stop_needed_ = false;
public:
    /*
     * explicit prevents implicit conversion of Options into a Sink. The member
     * initializer binds options_ before the constructor body runs.
     */
    explicit Sink(const Options& o) : options_(o) {
        if (!o.device.empty()) {
            /*
             * O_RDWR permits device reads/control as well as writes; we never read here.
             * O_CLOEXEC asks the OS to close this descriptor if the process execs another
             * program. There is no create flag: a missing device must not become a file.
             */
            fd_ = ::open(o.device.c_str(),O_RDWR|O_CLOEXEC);
            if (fd_>=0) {
                struct stat st{};
                /*
                 * Inspect the opened object rather than re-resolving its path. A character
                 * device check prevents accidentally treating a regular file as hardware, but
                 * does not prove that an arbitrary character device is actually Radioberry.
                 */
                if (::fstat(fd_,&st)<0 || !S_ISCHR(st.st_mode)) {
                    /*
                     * Constructor failure does not run this object's destructor, so close the
                     * already acquired descriptor explicitly before throwing.
                     */
                    ::close(fd_); fd_=-1;
                    throw std::runtime_error("--device must name a character device");
                }
            }
        } else {
            // Exclusive creation prevents accidental overwrites during experiments.
            /*
             * O_CREAT|O_EXCL atomically requires a new file. 0644 is an octal permission
             * request further restricted by the process umask. No truncation of old files.
             * A partial file can remain after a later failure; this is not atomic publication.
             */
            fd_ = ::open(o.output.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
        }
        if (fd_<0) throw std::runtime_error(failure("open"));
    }
    /*
     * Delete copying: two owners of the same descriptor could close it twice or
     * issue duplicate stop controls. This implementation does not offer move semantics.
     */
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    /*
     * Destructor cleanup also runs when an exception unwinds main's try block.
     * Stop is attempted before close. Neither action guarantees queued samples
     * have reached the FPGA; close errors are not checked in this minimal version.
     */
    ~Sink() {
        if (fd_>=0) { stop(); ::close(fd_); }
    }
    /*
     * Arm shutdown before sending the first setup command, so partial startup
     * failure still triggers the supplied stop sequence through destruction.
     */
    void start() {
        stop_needed_ = !options_.stop.empty(); // Cleanup even after a partial start.
        for (const auto& c : options_.start) {
            if (interrupted) throw std::runtime_error("Interrupted before startup completed");
            send_control(fd_,c);
        }
    }
    /*
     * Best-effort shutdown returns a boolean instead of propagating control
     * exceptions. Destructors should not throw during stack unwinding.
     * The code catches standard exceptions per command and continues remaining
     * commands; a stuck kernel or terminated process defeats these guarantees.
     */
    bool stop() noexcept {
        if (!stop_needed_) return true;
        /*
         * Clear the flag before attempting controls. A later destructor will not
         * repeat the sequence, even if this attempt fails. Idempotence here applies
         * to attempts, not to proof that the radio is now off.
         */
        stop_needed_ = false; // Do not issue the sequence twice from destructor.
        bool ok = true;
        for (const auto& c : options_.stop) {
            try { send_control(fd_,c); }
            catch (const std::exception& e) { std::cerr<<"Stop control failed: "<<e.what()<<'\n'; ok=false; }
        }
        return ok;
    }
    /*
     * Consume the full block through one or more POSIX writes. bytes is a const
     * reference, so the data cannot be edited and the 16 KiB array is not copied.
     * A syscall may accept fewer bytes than requested; track the remaining suffix.
     */
    void write_block(const std::array<std::uint8_t,block_bytes>& bytes) {
        /*
         * offset counts bytes already accepted. It must never be advanced on failure.
         */
        std::size_t offset = 0;
        while (offset<bytes.size()) {
            if (interrupted) throw std::runtime_error("Interrupted; queued hardware samples may remain");
            /*
             * Pointer arithmetic skips the accepted prefix; requested length shrinks by
             * the same amount. POSIX returns a signed count because -1 denotes error.
             * A device write crosses into radioberry_write(), not directly to a GPIO register.
             */
            const auto n = ::write(fd_,bytes.data()+offset,bytes.size()-offset);
            /*
             * Check negativity before any conversion to unsigned size_t; otherwise -1
             * would become an enormous positive count. Device retries are conservative
             * because the existing kernel has unusual interruption/timeout behaviour.
             */
            if (n<0) {
                // Do not repeatedly retry EAGAIN: the existing driver's timeout
                // path leaks memory. Fail and run configured shutdown instead.
                if (errno==EINTR && options_.device.empty() && !interrupted) continue;
                throw std::runtime_error(failure("write"));
            }
            /*
             * Zero progress with bytes remaining would make this loop infinite.
             */
            if (!n) throw std::runtime_error("write made no progress");
            /*
             * A device result ending inside an I/Q word violates this transport assumption.
             * Abort rather than attempt to reconstruct uncertain sample alignment.
             * This is not a rollback: the accepted prefix may already be queued.
             */
            if (!options_.device.empty() && n%4 != 0)
                throw std::runtime_error("Driver accepted a partial sample word; aborting");
            /*
             * Only a positive, validated count is converted into the unsigned byte offset.
             */
            offset += static_cast<std::size_t>(n);
        }
    }
};
} // namespace

/*
 * Program entry: validate, create the sink, optionally configure the radio,
 * stream blocks, wait explicitly if requested, and attempt shutdown.
 * Exceptions become a diagnostic plus a nonzero process exit status.
 */
int main(int argc, char** argv) {
    /*
     * Handle the exact --help form before requiring a bit string in parse().
     */
    if (argc==2 && std::string(argv[1])=="--help") { std::cout<<usage; return 0; }
    try {
        /*
         * Declare Options before Sink. Destruction occurs in reverse construction
         * order, so Sink can safely use its reference to o during cleanup.
         */
        const auto o = parse(argc,argv);
        const auto l = layout(o); // Validate arithmetic before touching any output.
        /*
         * Register cooperative stop requests for Ctrl-C and ordinary termination.
         * The handler only sets a flag; main and write_block perform normal cleanup.
         */
        std::signal(SIGINT,on_signal);
        std::signal(SIGTERM,on_signal);
        std::cerr<<"Mode: "<<o.mode<<"; payload words: "<<l.payload
                 <<"; tail words: "<<o.tail<<"; padding words: "<<(l.padded-l.total)
                 <<"; total bytes: "<<multiply(l.padded,4)<<'\n';
        /*
         * Convert to floating point before division, avoiding integer truncation.
         * Duration is based on the supplied assumption, not measured hardware progress.
         * Diagnostics go to stderr; binary output goes only through the descriptor.
         */
        std::cerr<<"Nominal duration at assumed "<<o.sample_rate<<" words/s: "
                 <<std::fixed<<std::setprecision(3)
                 <<(1000.0*static_cast<double>(l.padded)/static_cast<double>(o.sample_rate))
                 <<" ms (rate is not configured by this option)\n";
        Sink sink(o);
        sink.start();
        /*
         * Allocate one fixed-size array with automatic storage; {} zero-initialises it.
         * fill_block overwrites every byte on each iteration. Output memory does not
         * grow with repeat, although the input string and control vectors are dynamic.
         */
        std::array<std::uint8_t,block_bytes> block{};
        /*
         * Advance the absolute position by one whole block. layout() ensures the
         * upper bound is a block multiple and byte counts fit, so no short tail write
         * is required and the final loop increment stays in range.
         */
        for (std::uint64_t n=0; n<l.padded; n+=words_per_block) {
            fill_block(o,l,n,block);
            sink.write_block(block);
        }
        if (!o.device.empty()) {
            std::cerr<<"Queued. Waiting "<<o.hold_ms<<" ms; no hardware completion is available.\n";
            /*
             * steady_clock is monotonic, avoiding jumps when wall-clock time is adjusted.
             * The hold starts after the final queue write, not at the first sample.
             */
            const auto deadline = std::chrono::steady_clock::now()+std::chrono::milliseconds(o.hold_ms);
            /*
             * Sleep in nominal 10 ms chunks to check the signal flag between sleeps.
             * Scheduling and blocking syscalls can extend actual time; this is not real-time
             * waveform pacing. PIO and gateware, not this sleep, control sample consumption.
             */
            while (!interrupted && std::chrono::steady_clock::now()<deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            if (o.configured)
                std::cerr<<"No stop control sent (--configured); gateware state is your responsibility.\n";
        } else std::cerr<<"Wrote "<<o.output<<'\n';
        /*
         * Explicit stop allows main to report shutdown failure. The destructor later
         * closes the descriptor but does not retry the sequence.
         */
        if (!sink.stop()) return 1;
        /*
         * 130 is this program's chosen interrupted status, including handled SIGTERM.
         * Zero means successful local steps, not verified reception or RF completion.
         */
        return interrupted ? 130 : 0;
    /*
     * On a throw after Sink construction, Sink is destroyed before this catch.
     * No Sink exists if parsing/layout/open construction failed. e.what() supplies
     * the diagnostic text; the process returns failure to a shell or calling script.
     */
    } catch (const std::exception& e) {
        std::cerr<<"radioberry-tx: "<<e.what()<<'\n';
        return interrupted ? 130 : 1;
    }
}
