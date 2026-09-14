#define CATCH_CONFIG_MAIN
#include <new>
#include <catch2/catch.hpp>
#include "itch/parser.hpp"
#include "exchange/reconciler.hpp"
#include <sstream>
#include <vector>

using namespace exchange;
using Bytes = std::vector<std::uint8_t>;

// Independent, literal hex fixtures: production encoder/offset tables are not used.
static Bytes hex(const std::string& text) {
    std::istringstream input(text);
    std::string byte;
    Bytes result;
    while (input >> byte) result.push_back(static_cast<std::uint8_t>(std::stoul(byte, nullptr, 16)));
    return result;
}
static Bytes fixture(char type) {
    // Header: locate 0x1234, tracking 0xABCD, 48-bit time 0x010203040506.
    auto bytes = hex("00 12 34 ab cd 01 02 03 04 05 06");
    bytes[0] = static_cast<std::uint8_t>(type);
    std::string body;
    switch (type) {
    case 'S': body = "4f"; break;
    case 'R': body = "41 41 50 4c 20 20 20 20 51 4e 00 00 00 64 4e 43 43 20 50 4e 4e 31 4e 00 00 00 03 4e"; break;
    case 'A': body = "01 23 45 67 89 ab cd ef 42 01 02 03 04 41 41 50 4c 20 20 20 20 00 12 d6 87"; break;
    case 'F': body = "01 23 45 67 89 ab cd ef 53 01 02 03 04 41 41 50 4c 20 20 20 20 00 12 d6 87 4e 53 44 51"; break;
    case 'E': body = "01 23 45 67 89 ab cd ef 01 02 03 04 fe dc ba 98 76 54 32 10"; break;
    case 'C': body = "01 23 45 67 89 ab cd ef 01 02 03 04 fe dc ba 98 76 54 32 10 4e 00 12 d6 88"; break;
    case 'X': body = "01 23 45 67 89 ab cd ef 01 02 03 04"; break;
    case 'D': body = "01 23 45 67 89 ab cd ef"; break;
    case 'U': body = "01 23 45 67 89 ab cd ef fe dc ba 98 76 54 32 10 01 02 03 04 00 12 d6 88"; break;
    case 'P': body = "00 00 00 00 00 00 00 00 42 01 02 03 04 41 41 50 4c 20 20 20 20 00 12 d6 87 fe dc ba 98 76 54 32 10"; break;
    default: FAIL("Unknown fixture tag");
    }
    auto suffix = hex(body);
    bytes.insert(bytes.end(), suffix.begin(), suffix.end());
    return bytes;
}
static itch::Message decode(const Bytes& bytes) { return itch::parse_message(bytes.data(), bytes.size()); }
static std::string frame(const Bytes& bytes) {
    std::string result;
    result += static_cast<char>(bytes.size() >> 8U);
    result += static_cast<char>(bytes.size() & 255U);
    result.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return result;
}

TEST_CASE("All required ITCH messages decode header bytes exactly") {
    const char type = GENERATE('S', 'R', 'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P');
    const auto result = decode(fixture(type));
    REQUIRE(result.type == type);
    REQUIRE(result.event.metadata.instrument_id == 0x1234);
    REQUIRE(result.event.metadata.tracking_number == 0xabcd);
    REQUIRE(result.event.metadata.timestamp_ns == 0x010203040506ULL);
}

TEST_CASE("System event states use their documented meanings") {
    const auto pair = GENERATE(std::make_pair('O', SessionState::MessagesStart),
        std::make_pair('S', SessionState::SystemOpen), std::make_pair('Q', SessionState::MarketOpen),
        std::make_pair('M', SessionState::MarketClose), std::make_pair('E', SessionState::SystemClose),
        std::make_pair('C', SessionState::MessagesEnd));
    auto bytes = fixture('S'); bytes.back() = static_cast<std::uint8_t>(pair.first);
    REQUIRE(std::get<SystemEvent>(decode(bytes).event.payload).state == pair.second);
}

TEST_CASE("Directory preserves every field and trims only right padding") {
    const auto d = std::get<InstrumentDirectory>(decode(fixture('R')).event.payload);
    REQUIRE(d.symbol == "AAPL"); REQUIRE(d.market_category == 'Q');
    REQUIRE(d.financial_status == 'N'); REQUIRE(d.round_lot_size == 100);
    REQUIRE(d.round_lots_only == 'N'); REQUIRE(d.issue_classification == 'C');
    REQUIRE(d.issue_subtype == "C"); REQUIRE(d.authenticity == 'P');
    REQUIRE(d.short_sale_threshold == 'N'); REQUIRE(d.ipo_flag == 'N');
    REQUIRE(d.price_tier == '1'); REQUIRE(d.etp_flag == 'N');
    REQUIRE(d.leverage_factor == 3); REQUIRE(d.inverse_indicator == 'N');
}

TEST_CASE("Adds decode 64-bit references shares side exact price and optional MPID") {
    const char type = GENERATE('A', 'F');
    const auto add = std::get<OrderAdded>(decode(fixture(type)).event.payload);
    REQUIRE(add.reference == 0x0123456789abcdefULL);
    REQUIRE(add.quantity == 0x01020304);
    REQUIRE(add.symbol == "AAPL");
    REQUIRE(add.price == 1234567);
    REQUIRE(decimal_price(add.price) == Approx(123.4567));
    REQUIRE(decimal_price(1) == Approx(0.0001));
    REQUIRE(decimal_price(2000000000) == Approx(200000.0));
    REQUIRE(add.side == (type == 'A' ? Side::Buy : Side::Sell));
    if (type == 'F') REQUIRE(add.participant == "NSDQ");
    else REQUIRE_FALSE(add.participant);
}

TEST_CASE("E and C executions retain match identity printable flag and optional price") {
    const char type = GENERATE('E', 'C');
    const auto e = std::get<OrderExecuted>(decode(fixture(type)).event.payload);
    REQUIRE(e.reference == 0x0123456789abcdefULL);
    REQUIRE(e.quantity == 0x01020304);
    REQUIRE(e.match_id == 0xfedcba9876543210ULL);
    REQUIRE(e.printable == (type == 'E'));
    if (type == 'C') REQUIRE(e.execution_price == 1234568);
    else REQUIRE_FALSE(e.execution_price);
}

TEST_CASE("Cancel delete and replace have distinct normalized semantics") {
    const auto cancel = std::get<OrderCancelled>(decode(fixture('X')).event.payload);
    REQUIRE(cancel.reference == 0x0123456789abcdefULL);
    REQUIRE(cancel.quantity == 0x01020304);
    const auto removed = std::get<OrderDeleted>(decode(fixture('D')).event.payload);
    REQUIRE(removed.reference == cancel.reference);
    const auto replace = std::get<OrderReplaced>(decode(fixture('U')).event.payload);
    REQUIRE(replace.reference == cancel.reference);
    REQUIRE(replace.new_reference == 0xfedcba9876543210ULL);
    REQUIRE(replace.quantity == 0x01020304);
    REQUIRE(replace.price == 1234568);
}

TEST_CASE("Non-cross trade is a print not a resting order update") {
    const auto trade = std::get<Trade>(decode(fixture('P')).event.payload);
    REQUIRE(trade.quantity == 0x01020304);
    REQUIRE(trade.price == 1234567); REQUIRE(trade.symbol == "AAPL");
    REQUIRE(trade.match_id == 0xfedcba9876543210ULL);
    Reconciler reconciler;
    reconciler.apply(decode(fixture('P')).event);
    REQUIRE(reconciler.audit().empty());
}

TEST_CASE("Every truncation and oversized declared payload fails safely") {
    const char type = GENERATE('S', 'R', 'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P');
    auto bytes = fixture(type);
    for (std::size_t n = 0; n < bytes.size(); ++n)
        REQUIRE_THROWS_AS(itch::parse_message(bytes.data(), n), itch::ParseError);
    bytes.push_back(0);
    REQUIRE_THROWS_AS(decode(bytes), itch::ParseError);
    REQUIRE_THROWS_AS(itch::parse_message(nullptr, 20), itch::ParseError);
}

TEST_CASE("Unknown tags invalid sides text flags and timestamps are errors") {
    auto bytes = fixture('A'); bytes[0] = '?'; REQUIRE_THROWS_AS(decode(bytes), itch::ParseError);
    bytes = fixture('A'); bytes[19] = '?'; REQUIRE_THROWS_AS(decode(bytes), itch::ParseError);
    bytes = fixture('A'); bytes[24] = 0xff; REQUIRE_THROWS_AS(decode(bytes), itch::ParseError);
    bytes = fixture('C'); bytes[31] = '?'; REQUIRE_THROWS_AS(decode(bytes), itch::ParseError);
    bytes = fixture('S'); bytes[11] = '?'; REQUIRE_THROWS_AS(decode(bytes), itch::ParseError);
    bytes = fixture('A'); bytes[5] = 0xff; REQUIRE_THROWS_AS(decode(bytes), itch::ParseError);
}

TEST_CASE("Stream handles prefix bounds terminator and physical EOF distinctly") {
    const auto first = frame(fixture('A'));
    std::istringstream complete(first + frame(fixture('X')) + std::string(2, '\0'));
    itch::Reader reader(complete);
    REQUIRE(reader.next()->type == 'A'); REQUIRE(reader.next()->type == 'X');
    REQUIRE_FALSE(reader.next()); REQUIRE(reader.terminator_seen());
    REQUIRE_FALSE(reader.next());
    std::istringstream clean_eof(first);
    itch::Reader eof_reader(clean_eof);
    REQUIRE(eof_reader.next()); REQUIRE_FALSE(eof_reader.next());
    REQUIRE(eof_reader.physical_eof()); REQUIRE_FALSE(eof_reader.terminator_seen());
    std::istringstream short_prefix(std::string(1, '\0'));
    itch::Reader short_reader(short_prefix);
    REQUIRE_THROWS_AS(short_reader.next(), itch::ParseError);
    std::istringstream short_payload(first.substr(0, first.size() - 1));
    itch::Reader partial(short_payload);
    REQUIRE_THROWS_AS(partial.next(), itch::ParseError);
    std::istringstream trailing(std::string(2, '\0') + "x");
    itch::Reader trailing_reader(trailing);
    REQUIRE_THROWS_AS(trailing_reader.next(), itch::ParseError);
}

TEST_CASE("Administrative messages are explicitly length validated not silently unknown") {
    const char type = GENERATE('H', 'Y', 'L', 'V', 'W', 'K', 'J', 'h', 'Q', 'B', 'I', 'N');
    Bytes bytes(itch::message_length(type), 0);
    bytes[0] = static_cast<std::uint8_t>(type);
    REQUIRE(std::holds_alternative<Administrative>(decode(bytes).event.payload));
    REQUIRE_FALSE(itch::normalized_type(type));
    bytes.pop_back(); REQUIRE_THROWS_AS(decode(bytes), itch::ParseError);
}

static Event event(Payload payload, std::uint32_t instrument = 1) { return {{instrument, 0, 1}, std::move(payload)}; }
static OrderAdded add(OrderReference ref, Shares qty) { return {ref, Side::Buy, qty, "AAPL", 1000000, std::nullopt}; }

TEST_CASE("Reconciliation balances execution cancellation deletion and replacement chains") {
    Reconciler r;
    r.apply(event(add(1, 100)));
    r.apply(event(OrderExecuted{1, 20, 900, true, std::nullopt}));
    r.apply(event(OrderCancelled{1, 10}));
    r.apply(event(OrderReplaced{1, 2, 50, 2000000}));
    r.apply(event(OrderExecuted{2, 5, 901, false, 1999999}));
    r.verify();
    REQUIRE(r.remaining_quantity() == 45);
    r.apply(event(OrderDeleted{2}));
    r.verify();
    const auto row = r.audit().at(0);
    REQUIRE(row.added == 100); REQUIRE(row.replacement_added == 50);
    REQUIRE(row.executed == 25); REQUIRE(row.cancelled == 10);
    REQUIRE(row.replaced_out == 70); REQUIRE(row.deleted == 45);
    REQUIRE(row.remaining == 0); REQUIRE(row.balanced());
    REQUIRE(r.live_orders() == 0); REQUIRE(r.checked_updates() == 5);
}

TEST_CASE("Reconciliation rejects excessive reductions stale references duplicates and wrong instruments") {
    SECTION("too much") { Reconciler r; r.apply(event(add(1, 5))); REQUIRE_THROWS(r.apply(event(OrderCancelled{1, 6}))); }
    SECTION("closed") { Reconciler r; r.apply(event(add(1, 5))); r.apply(event(OrderDeleted{1})); REQUIRE_THROWS(r.apply(event(OrderDeleted{1}))); }
    SECTION("old replacement") { Reconciler r; r.apply(event(add(1, 5))); r.apply(event(OrderReplaced{1, 2, 5, 100})); REQUIRE_THROWS(r.apply(event(OrderDeleted{1}))); }
    SECTION("duplicate") { Reconciler r; r.apply(event(add(1, 5))); REQUIRE_THROWS(r.apply(event(add(1, 5)))); }
    SECTION("instrument") { Reconciler r; r.apply(event(add(1, 5))); REQUIRE_THROWS(r.apply(event(OrderDeleted{1}, 2))); }
    SECTION("replacement collision") { Reconciler r; r.apply(event(add(1, 5))); r.apply(event(add(2, 5))); REQUIRE_THROWS(r.apply(event(OrderReplaced{1, 2, 7, 100}))); }
}

TEST_CASE("Reference sample stays bounded and replacement descendants remain audited") {
    Reconciler r(1);
    r.apply(event(add(1, 10))); r.apply(event(add(2, 20)));
    r.apply(event(OrderCancelled{2, 100})); // unsampled, explicitly not audited
    r.apply(event(OrderReplaced{1, 3, 7, 200}));
    r.apply(event(OrderCancelled{3, 2}));
    r.verify();
    REQUIRE(r.audit().size() == 1); REQUIRE(r.remaining_quantity() == 5);
    REQUIRE(r.audit()[0].current_reference == 3);
}
