// LOBSTER message-file replay.
//
// LOBSTER (lobsterdata.com) message files are CSV rows:
//     time,type,order_id,size,price,direction
// with types 1=new limit, 2=partial cancel, 3=delete, 4=execute visible,
// 5=execute hidden, 6=cross, 7=halt, and direction 1=buy, -1=sell.
// Prices are in units of $0.0001 — already integer ticks, which is exactly
// what the engine wants.
//
// Mapping into engine operations:
//   type 1 -> submit_limit           type 3 -> cancel
//   type 2 -> reduce (size DOWN by the given amount, priority preserved —
//             LOBSTER's semantics for partial cancellation)
//   type 4 -> the historical tape says a resting order executed; we
//             synthesise the aggressor: a marketable limit on the OPPOSITE
//             side at the printed price/size. Approximation, stated openly:
//             our book then matches whatever is at the front of that level,
//             which reproduces realistic *load* (bursts of executions at
//             the touch) rather than replaying LOBSTER's exact fills.
//   types 5-7 -> skipped (hidden liquidity and halts are out of scope).

#pragma once

#include <charconv>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "lob/flow.hpp"
#include "lob/types.hpp"

namespace lob {

struct LobsterStats {
    std::size_t submits = 0;
    std::size_t reduces = 0;
    std::size_t cancels = 0;
    std::size_t synthesized_executions = 0;
    std::size_t skipped = 0;
    Price min_price = 0;
    Price max_price = 0;
};

namespace detail {
inline std::int64_t parse_int_field(std::string_view line, std::size_t& pos) {
    const std::size_t comma = line.find(',', pos);
    std::string_view field = line.substr(pos, comma - pos);
    pos = (comma == std::string_view::npos) ? line.size() : comma + 1;
    // Tolerate a leading '-' and scientific-notation-free integers.
    std::int64_t value = 0;
    auto [ptr, ec] = std::from_chars(field.data(), field.data() + field.size(), value);
    if (ec != std::errc{}) throw std::runtime_error("bad integer field in LOBSTER row");
    (void)ptr;
    return value;
}
}  // namespace detail

// Parse a LOBSTER message file into engine ops. Synthetic aggressor ids
// start far above the file's own id space to avoid collisions. LOBSTER
// files can re-use exchange order ids across the day after deletion; ids
// that would collide with a live order are remapped to fresh ids.
inline std::vector<FlowOp> load_lobster(const std::string& path, LobsterStats& stats) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open LOBSTER file: " + path);

    std::vector<FlowOp> ops;
    std::string line;
    OrderId synthetic_id = 1'000'000'000'000ULL;
    stats = LobsterStats{};

    // Track live sizes so type-2 rows (cancel BY amount) become reduce-to.
    std::unordered_map<OrderId, Quantity> live;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::size_t pos = 0;
        std::string_view view{line};
        // time field: may contain a '.', skip to first comma without parsing
        pos = view.find(',') + 1;
        const std::int64_t type = detail::parse_int_field(view, pos);
        const std::int64_t raw_id = detail::parse_int_field(view, pos);
        const std::int64_t size = detail::parse_int_field(view, pos);
        const std::int64_t price = detail::parse_int_field(view, pos);
        const std::int64_t direction = detail::parse_int_field(view, pos);
        const Side side = (direction == 1) ? Side::Buy : Side::Sell;
        const OrderId id = static_cast<OrderId>(raw_id);

        if (price <= 0) { ++stats.skipped; continue; }
        if (stats.min_price == 0 || price < stats.min_price) stats.min_price = price;
        if (price > stats.max_price) stats.max_price = price;

        switch (type) {
            case 1: {
                if (size <= 0 || live.count(id)) { ++stats.skipped; break; }
                ops.push_back({FlowOp::Kind::Limit, id, side, price,
                               static_cast<Quantity>(size)});
                live[id] = static_cast<Quantity>(size);
                ++stats.submits;
                break;
            }
            case 2: {  // partial cancel: size DOWN by `size`
                auto it = live.find(id);
                if (it == live.end() || size <= 0) { ++stats.skipped; break; }
                const Quantity cut = static_cast<Quantity>(size);
                const Quantity new_qty = (cut >= it->second) ? 0 : it->second - cut;
                ops.push_back({FlowOp::Kind::Reduce, id, side, Price{0}, new_qty});
                if (new_qty == 0) live.erase(it); else it->second = new_qty;
                ++stats.reduces;
                break;
            }
            case 3: {
                if (!live.count(id)) { ++stats.skipped; break; }
                ops.push_back({FlowOp::Kind::Cancel, id, side, Price{0}, Quantity{0}});
                live.erase(id);
                ++stats.cancels;
                break;
            }
            case 4: {  // visible execution -> synthesise the aggressor
                if (size <= 0) { ++stats.skipped; break; }
                ops.push_back({FlowOp::Kind::Limit, synthetic_id++, opposite(side), price,
                               static_cast<Quantity>(size)});
                // The passive side loses that quantity in our book too via
                // matching; keep `live` roughly in sync by reducing it.
                auto it = live.find(id);
                if (it != live.end()) {
                    const Quantity cut = static_cast<Quantity>(size);
                    if (cut >= it->second) live.erase(it); else it->second -= cut;
                }
                ++stats.synthesized_executions;
                break;
            }
            default:
                ++stats.skipped;
        }
    }
    return ops;
}

}  // namespace lob
