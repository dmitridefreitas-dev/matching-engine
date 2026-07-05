// WebAssembly bindings for the FastBook matching engine.
//
// Exposes an interactive book (submit/cancel/snapshot with auto-assigned
// order ids, so the JS side can never trip the duplicate-id precondition)
// and an in-module benchmark that replays the exact synthetic flow used by
// the native benchmarks — the same generate_flow(), the same seed, so the
// browser numbers are directly comparable to the README's native numbers.
//
// Build (from repo root, with emsdk activated):
//   em++ -O3 -std=c++20 -Iinclude --bind \
//        -s MODULARIZE=1 -s EXPORT_ES6=1 -s EXPORT_NAME=createLobEngine \
//        -s ENVIRONMENT=web,node -s SINGLE_FILE=1 -s ALLOW_MEMORY_GROWTH=1 \
//        wasm/bindings.cpp -o wasm/lob_engine.js

#include <emscripten/bind.h>
#include <emscripten/emscripten.h>
#include <emscripten/val.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "lob/fast_book.hpp"
#include "lob/flow.hpp"
#include "lob/types.hpp"

namespace {

using namespace lob;

class WasmBook {
public:
    WasmBook(double min_price, double max_price)
        : book_(static_cast<Price>(min_price), static_cast<Price>(max_price),
                1 << 16) {}

    // Returns the assigned order id (0 when the order fully filled on entry
    // and nothing rested — fills tell the story either way).
    double submitLimit(bool is_buy, double price, double qty) {
        fills_.clear();
        const OrderId id = next_id_++;
        const Quantity remaining = book_.submit_limit(
            id, is_buy ? Side::Buy : Side::Sell, static_cast<Price>(price),
            static_cast<Quantity>(qty), fills_);
        return remaining > 0 ? static_cast<double>(id) : 0.0;
    }

    double submitMarket(bool is_buy, double qty) {
        fills_.clear();
        const OrderId id = next_id_++;
        return static_cast<double>(book_.submit_market(
            id, is_buy ? Side::Buy : Side::Sell, static_cast<Quantity>(qty),
            fills_));
    }

    bool cancel(double id) { return book_.cancel(static_cast<OrderId>(id)); }

    emscripten::val lastFills() const {
        auto out = emscripten::val::array();
        for (std::size_t i = 0; i < fills_.size(); ++i) {
            auto f = emscripten::val::object();
            f.set("taker", static_cast<double>(fills_[i].taker));
            f.set("maker", static_cast<double>(fills_[i].maker));
            f.set("price", static_cast<double>(fills_[i].price));
            f.set("quantity", static_cast<double>(fills_[i].quantity));
            out.set(i, f);
        }
        return out;
    }

    // Aggregated per-level view for rendering: [{price, qty, orders}, ...].
    emscripten::val levels(bool bids, int max_levels) const {
        auto out = emscripten::val::array();
        const auto snap = book_.snapshot(bids ? Side::Buy : Side::Sell);
        const std::size_t n =
            std::min<std::size_t>(snap.size(), static_cast<std::size_t>(max_levels));
        for (std::size_t i = 0; i < n; ++i) {
            std::uint64_t total = 0;
            for (const auto& [id, q] : snap[i].orders) total += q;
            auto level = emscripten::val::object();
            level.set("price", static_cast<double>(snap[i].price));
            level.set("qty", static_cast<double>(total));
            level.set("orders", static_cast<double>(snap[i].orders.size()));
            out.set(i, level);
        }
        return out;
    }

    double bestBid() const { return book_.has_bid() ? static_cast<double>(book_.best_bid()) : -1; }
    double bestAsk() const { return book_.has_ask() ? static_cast<double>(book_.best_ask()) : -1; }
    double openOrders() const { return static_cast<double>(book_.open_orders()); }

private:
    FastBook book_;
    std::vector<Fill> fills_;
    OrderId next_id_ = 1;
};

// Replay `operations` ops of the canonical synthetic flow (seed-stable across
// platforms) through a fresh FastBook. Returns {ops, millis, opsPerSec, fills}.
emscripten::val runBenchmark(double operations, double seed) {
    FlowConfig cfg;
    cfg.operations = static_cast<std::size_t>(operations);
    cfg.seed = static_cast<std::uint64_t>(seed);
    const std::vector<FlowOp> ops = generate_flow(cfg);

    FastBook book(cfg.min_price, cfg.max_price, 1 << 20);
    std::vector<Fill> fills;
    fills.reserve(64);
    std::uint64_t fill_count = 0;

    const double t0 = emscripten_get_now();
    for (const FlowOp& op : ops) {
        switch (op.kind) {
            case FlowOp::Kind::Limit:
                fills.clear();
                book.submit_limit(op.id, op.side, op.price, op.quantity, fills);
                fill_count += fills.size();
                break;
            case FlowOp::Kind::Market:
                fills.clear();
                book.submit_market(op.id, op.side, op.quantity, fills);
                fill_count += fills.size();
                break;
            case FlowOp::Kind::Cancel:
                book.cancel(op.id);
                break;
            case FlowOp::Kind::Reduce:
                book.reduce(op.id, op.quantity);
                break;
        }
    }
    const double ms = emscripten_get_now() - t0;

    auto out = emscripten::val::object();
    out.set("ops", static_cast<double>(ops.size()));
    out.set("millis", ms);
    out.set("opsPerSec", ms > 0 ? (static_cast<double>(ops.size()) * 1000.0 / ms) : 0);
    out.set("fills", static_cast<double>(fill_count));
    return out;
}

}  // namespace

EMSCRIPTEN_BINDINGS(lob_engine) {
    emscripten::class_<WasmBook>("WasmBook")
        .constructor<double, double>()
        .function("submitLimit", &WasmBook::submitLimit)
        .function("submitMarket", &WasmBook::submitMarket)
        .function("cancel", &WasmBook::cancel)
        .function("lastFills", &WasmBook::lastFills)
        .function("levels", &WasmBook::levels)
        .function("bestBid", &WasmBook::bestBid)
        .function("bestAsk", &WasmBook::bestAsk)
        .function("openOrders", &WasmBook::openOrders);
    emscripten::function("runBenchmark", &runBenchmark);
}
