#pragma once

#include "exchange/events.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <stdexcept>

namespace itch {

class ParseError : public std::runtime_error {
public:
    ParseError(std::uint64_t offset, const std::string& message);
    std::uint64_t offset() const noexcept { return offset_; }
private:
    std::uint64_t offset_;
};

struct Message {
    char type{}; // wire tag belongs to decoder envelope, not normalized Event
    exchange::Event event;
};

// Parse one payload (without BinaryFILE's 2-byte prefix). Every read is checked.
Message parse_message(const std::uint8_t* data, std::size_t length, std::uint64_t file_offset = 0);
std::size_t message_length(char type) noexcept; // 0 means unrecognized
bool normalized_type(char type) noexcept;

class Reader {
public:
    explicit Reader(std::istream& input) : input_(input) {}
    std::optional<Message> next();
    bool terminator_seen() const noexcept { return terminator_seen_; }
    bool physical_eof() const noexcept { return physical_eof_; }
    std::uint64_t offset() const noexcept { return offset_; }
private:
    std::istream& input_;
    std::array<std::uint8_t, 65535> buffer_{};
    std::uint64_t offset_{};
    bool terminator_seen_{}, physical_eof_{};
};

} // namespace itch
