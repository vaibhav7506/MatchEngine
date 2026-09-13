#include "matchengine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

using namespace matchengine;
using Clock = std::chrono::steady_clock;

struct Memory { std::uint64_t resident{}, private_bytes{}; };
static Memory memory() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX m{};
    m.cb = sizeof(m);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m), sizeof(m)))
        throw std::runtime_error("GetProcessMemoryInfo failed");
    return {static_cast<std::uint64_t>(m.WorkingSetSize), static_cast<std::uint64_t>(m.PrivateUsage)};
#elif defined(__linux__)
    std::ifstream input("/proc/self/statm");
    std::uint64_t pages, resident;
    if (!(input >> pages >> resident)) throw std::runtime_error("Cannot read process memory");
    return {resident * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE)), 0};
#else
    throw std::runtime_error("Memory measurement is supported on Windows and Linux");
#endif
}

static void memory_benchmark(std::size_t n) {
    // Run in a fresh process: no latency buffers or prior benchmark book.
    (void)memory(); // initialize OS measurement machinery before baseline
    const auto before = memory();
    Engine engine;
    for (std::size_t i = 0; i < n; ++i) {
        Order o;
        o.order_id = i + 1; o.trader_id = i + 1;
        o.side = i % 2 ? Side::BUY : Side::SELL;
        const double offset = static_cast<double>((i / 2) % 100) / 100.0;
        o.price = o.side == Side::BUY ? 99.0 - offset : 101.0 + offset;
        o.quantity = 100;
        engine.add_order(o);
    }
    const auto after = memory();
    const auto snapshot = engine.get_book_snapshot(1000);
    std::cout << "resting_orders=" << engine.resting_order_count()
              << " price_levels=" << snapshot.bids.size() + snapshot.asks.size()
              << " resident_bytes=" << after.resident
              << " resident_delta_bytes=" << static_cast<std::int64_t>(after.resident) - static_cast<std::int64_t>(before.resident)
              << " private_bytes=" << after.private_bytes
              << " private_delta_bytes=" << static_cast<std::int64_t>(after.private_bytes) - static_cast<std::int64_t>(before.private_bytes) << '\n';
}

static void throughput_benchmark(std::size_t n) {
    std::mt19937_64 rng(42);
    std::normal_distribution<double> price(100.0, 0.50);
    std::lognormal_distribution<double> quantity(3.0, 0.8);
    std::vector<Order> orders;
    orders.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        Order o;
        o.order_id = i + 1; o.trader_id = 1 + rng() % 1000;
        o.side = rng() % 2 ? Side::BUY : Side::SELL;
        o.price = std::max(0.01, std::round(price(rng) * 100) / 100);
        o.quantity = static_cast<Quantity>(std::clamp(std::round(quantity(rng)), 1.0, 10000.0));
        const auto type = rng() % 100;
        o.type = type < 80 ? OrderType::LIMIT : type < 90 ? OrderType::MARKET : type < 95 ? OrderType::IOC : OrderType::FOK;
        orders.push_back(o);
    }
    {
        Engine warmup;
        for (std::size_t i = 0; i < std::min(n, std::size_t{10000}); ++i) warmup.add_order(orders[i]);
    }
    Engine engine;
    std::vector<double> latency(n);
    std::uint64_t trade_count = 0;
    const auto start = Clock::now();
    for (std::size_t i = 0; i < n; ++i) {
        const auto begin = Clock::now();
        const auto trades = engine.add_order(orders[i]);
        const auto end = Clock::now();
        latency[i] = std::chrono::duration<double, std::micro>(end - begin).count();
        trade_count += trades.size();
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::sort(latency.begin(), latency.end());
    const auto percentile = [&](double p) { return latency[static_cast<std::size_t>(std::ceil(p * static_cast<double>(n))) - 1]; };
    std::cout << std::fixed << std::setprecision(3)
              << "orders=" << n << " seed=42 seconds=" << elapsed
              << " orders_per_second=" << static_cast<double>(n) / elapsed
              << " p50_us=" << percentile(0.50) << " p95_us=" << percentile(0.95)
              << " p99_us=" << percentile(0.99) << " trades=" << trade_count
              << " resting_orders=" << engine.resting_order_count() << '\n';
}

int main(int argc, char** argv) {
    try {
        bool measure_memory = argc > 1 && std::string(argv[1]) == "--memory";
        if (argc > 3 || (argc > 1 && std::string(argv[1]) != "--orders" && !measure_memory) || argc == 2)
            throw std::invalid_argument("Usage: matchengine_benchmark [--orders N | --memory N]");
        std::size_t n = 1000000;
        if (argc == 3) {
            const std::string value(argv[2]);
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::invalid_argument("N must be a positive integer");
            const auto parsed = std::stoull(value);
            if (parsed == 0 || parsed > 100000000) throw std::invalid_argument("N must be between 1 and 100000000");
            n = static_cast<std::size_t>(parsed);
        }
        if (measure_memory) memory_benchmark(n);
        else throughput_benchmark(n);
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
