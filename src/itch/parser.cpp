#include "itch/parser.hpp"

#include <algorithm>
#include <string>

namespace itch {
using namespace exchange;

ParseError::ParseError(std::uint64_t offset, const std::string& message)
    : std::runtime_error("ITCH byte " + std::to_string(offset) + ": " + message), offset_(offset) {}

namespace {
class Fields {
public:
    Fields(const std::uint8_t* data, std::size_t size, std::uint64_t offset)
        : data_(data), size_(size), offset_(offset) {}
    std::uint64_t number(std::size_t pos, std::size_t width) const {
        check(pos, width);
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < width; ++i) value = (value << 8U) | data_[pos + i];
        return value;
    }
    std::uint32_t u32(std::size_t pos) const { return static_cast<std::uint32_t>(number(pos, 4)); }
    char character(std::size_t pos) const { return static_cast<char>(number(pos, 1)); }
    std::string text(std::size_t pos, std::size_t width) const {
        check(pos, width);
        for (std::size_t i = 0; i < width; ++i)
            if (data_[pos + i] < 32 || data_[pos + i] > 126)
                throw ParseError(offset_ + pos + i, "non-ASCII text field");
        std::string result(reinterpret_cast<const char*>(data_ + pos), width);
        while (!result.empty() && result.back() == ' ') result.pop_back();
        return result;
    }
    Side side(std::size_t pos) const {
        const auto value = character(pos);
        if (value != 'B' && value != 'S') throw ParseError(offset_ + pos, "invalid order side");
        return value == 'B' ? Side::Buy : Side::Sell;
    }
private:
    void check(std::size_t pos, std::size_t width) const {
        if (pos > size_ || width > size_ - pos) throw ParseError(offset_ + pos, "field exceeds declared payload");
    }
    const std::uint8_t* data_;
    std::size_t size_;
    std::uint64_t offset_;
};

const char* administrative_name(char type) {
    switch (type) {
    case 'H': return "Trading action";
    case 'Y': return "Short-sale restriction";
    case 'L': return "Participant position";
    case 'V': return "Circuit-breaker levels";
    case 'W': return "Circuit-breaker status";
    case 'K': return "IPO quoting update";
    case 'J': return "LULD auction collar";
    case 'h': return "Operational halt";
    case 'Q': return "Cross trade";
    case 'B': return "Broken trade";
    case 'I': return "Net order imbalance";
    case 'N': return "Retail interest";
    default: return "Other administrative message";
    }
}
} // namespace

std::size_t message_length(char type) noexcept {
    // Nasdaq ITCH 5.0 payload lengths, excluding the BinaryFILE prefix.
    switch (type) {
    case 'S': return 12; case 'R': return 39;
    case 'A': return 36; case 'F': return 40;
    case 'E': return 31; case 'C': return 36;
    case 'X': return 23; case 'D': return 19; case 'U': return 35; case 'P': return 44;
    case 'H': return 25; case 'Y': return 20; case 'L': return 26;
    case 'V': return 35; case 'W': return 12; case 'K': return 28;
    case 'J': return 35; case 'h': return 21;
    case 'Q': return 40; case 'B': return 19; case 'I': return 50; case 'N': return 20;
    default: return 0;
    }
}

bool normalized_type(char type) noexcept {
    return type == 'S' || type == 'R' || type == 'A' || type == 'F' || type == 'E' ||
        type == 'C' || type == 'X' || type == 'D' || type == 'U' || type == 'P';
}

Message parse_message(const std::uint8_t* data, std::size_t length, std::uint64_t offset) {
    if (data == nullptr || length == 0) throw ParseError(offset, "empty payload");
    Fields f(data, length, offset);
    const char type = f.character(0);
    const auto expected = message_length(type);
    if (expected == 0) throw ParseError(offset, "unrecognized message type " + std::to_string(static_cast<unsigned char>(type)));
    if (length != expected) throw ParseError(offset, std::string("wrong length for ") + type +
        ": expected " + std::to_string(expected) + ", received " + std::to_string(length));
    Metadata metadata{static_cast<std::uint32_t>(f.number(1, 2)),
                      static_cast<std::uint16_t>(f.number(3, 2)), f.number(5, 6)};
    if (metadata.timestamp_ns >= 86400000000000ULL) throw ParseError(offset + 5, "timestamp outside session day");
    Payload payload;
    switch (type) {
    case 'S': {
        SessionState state;
        switch (f.character(11)) {
        case 'O': state = SessionState::MessagesStart; break;
        case 'S': state = SessionState::SystemOpen; break;
        case 'Q': state = SessionState::MarketOpen; break;
        case 'M': state = SessionState::MarketClose; break;
        case 'E': state = SessionState::SystemClose; break;
        case 'C': state = SessionState::MessagesEnd; break;
        default: throw ParseError(offset + 11, "invalid system event code");
        }
        payload = SystemEvent{state};
        break;
    }
    case 'R':
        payload = InstrumentDirectory{f.text(11, 8), f.character(19), f.character(20), f.u32(21),
            f.character(25), f.character(26), f.text(27, 2), f.character(29), f.character(30),
            f.character(31), f.character(32), f.character(33), f.u32(34), f.character(38)};
        break;
    case 'A': case 'F': {
        OrderAdded add{f.number(11, 8), f.side(19), f.u32(20), f.text(24, 8), f.u32(32), std::nullopt};
        if (type == 'F') add.participant = f.text(36, 4);
        payload = std::move(add);
        break;
    }
    case 'E': case 'C': {
        OrderExecuted executed{f.number(11, 8), f.u32(19), f.number(23, 8), true, std::nullopt};
        if (type == 'C') {
            const char printable = f.character(31);
            if (printable != 'Y' && printable != 'N') throw ParseError(offset + 31, "invalid printable flag");
            executed.printable = printable == 'Y';
            executed.execution_price = f.u32(32);
        }
        payload = executed;
        break;
    }
    case 'X': payload = OrderCancelled{f.number(11, 8), f.u32(19)}; break;
    case 'D': payload = OrderDeleted{f.number(11, 8)}; break;
    case 'U': payload = OrderReplaced{f.number(11, 8), f.number(19, 8), f.u32(27), f.u32(31)}; break;
    case 'P':
        // These legacy wire fields are not a usable order reference or trade side.
        (void)f.number(11, 8); (void)f.side(19);
        payload = Trade{f.u32(20), f.text(24, 8), f.u32(32), f.number(36, 8)};
        break;
    default:
        payload = Administrative{administrative_name(type)};
        break;
    }
    return Message{type, Event{metadata, std::move(payload)}};
}

std::optional<Message> Reader::next() {
    if (terminator_seen_ || physical_eof_) return std::nullopt;
    char prefix[2];
    const auto frame_offset = offset_;
    input_.read(prefix, 2);
    const auto prefix_bytes = input_.gcount();
    if (input_.bad()) throw ParseError(frame_offset, "input I/O failure");
    if (prefix_bytes == 0 && input_.eof()) { physical_eof_ = true; return std::nullopt; }
    if (prefix_bytes != 2) throw ParseError(frame_offset, "truncated length prefix");
    offset_ += 2;
    const std::size_t length = (static_cast<std::size_t>(static_cast<unsigned char>(prefix[0])) << 8U) |
                               static_cast<std::size_t>(static_cast<unsigned char>(prefix[1]));
    if (length == 0) {
        terminator_seen_ = true;
        if (input_.peek() != std::char_traits<char>::eof()) throw ParseError(offset_, "data follows session terminator");
        if (input_.bad()) throw ParseError(offset_, "input I/O failure after terminator");
        return std::nullopt;
    }
    input_.read(reinterpret_cast<char*>(buffer_.data()), static_cast<std::streamsize>(length));
    if (input_.bad() || input_.gcount() != static_cast<std::streamsize>(length))
        throw ParseError(frame_offset, "truncated message payload");
    auto message = parse_message(buffer_.data(), length, offset_);
    offset_ += length;
    return message;
}

} // namespace itch
