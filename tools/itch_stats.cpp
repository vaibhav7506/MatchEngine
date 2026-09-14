#include "itch/parser.hpp"
#include "exchange/reconciler.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

int main(int argc, char** argv) {
    std::array<std::uint64_t, 256> counts{};
    std::uint64_t total = 0, normalized = 0;
    try {
        if (argc < 2) throw std::invalid_argument("Usage: itch_stats FILE|- [--sample-orders N] [--audit FILE.csv] [--require-terminator]");
        std::size_t sample_limit = 10000;
        std::string audit_path;
        bool require_terminator = false;
        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--require-terminator") require_terminator = true;
            else if ((arg == "--sample-orders" || arg == "--audit") && i + 1 < argc) {
                const std::string value = argv[++i];
                if (arg == "--audit") audit_path = value;
                else {
                    if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                        throw std::invalid_argument("sample-orders must be a positive integer");
                    const auto parsed = std::stoull(value);
                    if (parsed == 0 || parsed > 10000000) throw std::invalid_argument("sample-orders must be 1..10000000");
                    sample_limit = static_cast<std::size_t>(parsed);
                }
            } else throw std::invalid_argument("Unknown/incomplete argument: " + arg);
        }
        std::ifstream file;
        std::istream* input = &std::cin;
        if (std::string(argv[1]) != "-") {
            file.open(argv[1], std::ios::binary);
            if (!file) throw std::runtime_error("Cannot open input file");
            input = &file;
        }
#ifdef _WIN32
        else _setmode(_fileno(stdin), _O_BINARY);
#endif
        itch::Reader reader(*input);
        exchange::Reconciler reconciler(sample_limit);
        bool start_seen = false, end_seen = false;
        std::uint64_t regressions = 0, previous_timestamp = 0;
        const auto started = std::chrono::steady_clock::now();
        while (auto message = reader.next()) {
            if (end_seen) throw std::runtime_error("Message after end-of-messages system event");
            ++counts[static_cast<unsigned char>(message->type)];
            ++total;
            normalized += itch::normalized_type(message->type) ? 1 : 0;
            const auto timestamp = message->event.metadata.timestamp_ns;
            if (timestamp < previous_timestamp) ++regressions;
            previous_timestamp = timestamp; // retain file sequence; do not sort timestamps
            if (const auto* system = std::get_if<exchange::SystemEvent>(&message->event.payload)) {
                if (system->state == exchange::SessionState::MessagesStart) {
                    if (total != 1) throw std::runtime_error("Start-of-messages is not the first event");
                    start_seen = true;
                }
                if (system->state == exchange::SessionState::MessagesEnd) end_seen = true;
            }
            reconciler.apply(message->event);
        }
        reconciler.verify();
        if (require_terminator && !reader.terminator_seen()) throw std::runtime_error("Missing BinaryFILE zero-length terminator");
        if (!start_seen || !end_seen) throw std::runtime_error("Incomplete session: missing start/end-of-messages system event");
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        std::cout << "messages=" << total << " normalized=" << normalized
                  << " administrative=" << total - normalized << " parse_failures=0\n";
        for (std::size_t type = 0; type < counts.size(); ++type)
            if (counts[type]) std::cout << "type=" << static_cast<char>(type) << " count=" << counts[type]
                << " scope=" << (itch::normalized_type(static_cast<char>(type)) ? "normalized" : "length-validated-only") << '\n';
        std::cout << "sampled_roots=" << reconciler.audit().size() << " checked_updates=" << reconciler.checked_updates()
                  << " live_sample_orders=" << reconciler.live_orders() << " remaining_sample_shares=" << reconciler.remaining_quantity()
                  << " reconciliation=" << (reconciler.audit().empty() ? "NOT_APPLICABLE_NO_ADDS" : "PASS") << '\n';
        std::cout << "start_event=" << start_seen << " end_event=" << end_seen
                  << " binaryfile_terminator=" << reader.terminator_seen() << " timestamp_regressions=" << regressions << '\n';
        std::cout << std::fixed << std::setprecision(3) << "bytes=" << reader.offset()
                  << " seconds=" << seconds << " messages_per_second=" << static_cast<double>(total) / seconds << '\n';
        if (!audit_path.empty()) {
            std::ofstream audit(audit_path);
            if (!audit) throw std::runtime_error("Cannot write audit file");
            audit << "root_reference,current_reference,added,replacement_added,executed,cancelled,deleted,replaced_out,remaining,balanced\n";
            for (const auto& row : reconciler.audit())
                audit << row.root_reference << ',' << row.current_reference << ',' << row.added << ',' << row.replacement_added
                      << ',' << row.executed << ',' << row.cancelled << ',' << row.deleted << ',' << row.replaced_out
                      << ',' << row.remaining << ',' << row.balanced() << '\n';
            if (!audit) throw std::runtime_error("Audit write failure");
        }
    } catch (const itch::ParseError& error) {
        std::cerr << "parse_failures=1 parsed_before_failure=" << total << " error=" << error.what() << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "run_failed=1 parsed_before_failure=" << total << " error=" << error.what() << '\n';
        return 1;
    }
}
