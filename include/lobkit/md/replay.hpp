#pragma once
// Replay harness: drives the ReplayAdapter from parsed feed messages.
//
// The wall clock exists only here (Pacer). The engine and the adapter receive the
// message timestamp as data and never read a clock. Nothing in the replay loop
// allocates or throws; reports and the CLI that print them are outside it.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>

#include "lobkit/book/engine.hpp"
#include "lobkit/md/feed.hpp"
#include "lobkit/md/lobster.hpp"
#include "lobkit/md/status.hpp"

namespace lobkit::md {

struct ReplayCounters {
  std::uint64_t messages = 0;  // parsed messages seen by the harness
  std::uint64_t adds = 0, reduces = 0, deletes = 0, executes = 0, replaces = 0;
  // Messages that by design do not change the displayed book. Kept separate so a
  // report can say how much of the feed was invisible to it.
  std::uint64_t hidden_executions = 0;
  std::uint64_t hidden_shares = 0;
  std::uint64_t nonbook_trades = 0;   // ITCH P
  std::uint64_t cross_trades = 0;     // LOBSTER type 6
  std::uint64_t halts = 0;            // LOBSTER type 7
  std::uint64_t replace_unknown = 0;  // ITCH U naming an order that is not live
};

// Applies one message to the book through the replay adapter.
template <EventSink S>
inline void apply_feed(Engine<S>& eng, ReplayCounters& c, const FeedMsg& m) noexcept {
  auto fa = eng.replay_adapter();
  ++c.messages;
  switch (m.type) {
    case FeedType::Add:
      ++c.adds;
      fa.apply_add(m.id, m.side, m.price, m.qty, m.ts);
      break;
    case FeedType::Reduce:
      ++c.reduces;
      fa.apply_reduce(m.id, m.qty, m.ts);
      break;
    case FeedType::Delete:
      ++c.deletes;
      fa.apply_delete(m.id, m.ts);
      break;
    case FeedType::Execute:
      ++c.executes;
      fa.apply_execute(m.id, m.qty, m.ts);
      break;
    case FeedType::Replace: {
      ++c.replaces;
      const Order* old = eng.book().find_order(m.id);
      if (old == nullptr) {
        ++c.replace_unknown;
        fa.apply_delete(m.id, m.ts);  // produces the UnknownOrderId reject and its count
        break;
      }
      const Side side = old->side;  // read before the delete invalidates the pointer
      fa.apply_delete(m.id, m.ts);
      fa.apply_add(m.new_id, side, m.price, m.qty, m.ts);
      break;
    }
    case FeedType::ExecuteHidden:
      ++c.hidden_executions;
      c.hidden_shares += m.qty;
      break;
    case FeedType::Trade:
      ++c.nonbook_trades;
      break;
    case FeedType::CrossTrade:
      ++c.cross_trades;
      break;
    case FeedType::Halt:
      ++c.halts;
      break;
  }
}

// Releases messages at their recorded spacing divided by a speed multiplier.
// Long gaps sleep, the last stretch spins, because sleep granularity is far
// coarser than the gaps in a busy feed. A message is never held back after its
// due time; lateness (how far behind schedule the replay fell) is tracked so a
// report can say whether the requested speed was actually achieved.
class Pacer {
 public:
  explicit Pacer(double speed) noexcept : speed_(speed > 0 ? speed : 1.0) {}

  void wait_until(Timestamp ts) noexcept {
    using clk = std::chrono::steady_clock;
    if (!started_) {
      started_ = true;
      t0_ = clk::now();
      ts0_ = ts;
      return;
    }
    const double dt = ts >= ts0_ ? static_cast<double>(ts - ts0_) / speed_ : 0.0;
    const auto target = t0_ + std::chrono::nanoseconds(static_cast<std::int64_t>(dt));
    for (;;) {
      const auto now = clk::now();
      if (now >= target) {
        const auto late =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - target).count();
        if (late > max_late_ns_) max_late_ns_ = late;
        return;
      }
      const auto left = target - now;
      if (left > std::chrono::microseconds(300)) {
        std::this_thread::sleep_for(left - std::chrono::microseconds(150));
      }
    }
  }

  std::int64_t max_lateness_ns() const noexcept { return max_late_ns_; }

 private:
  double speed_;
  bool started_ = false;
  std::chrono::steady_clock::time_point t0_{};
  Timestamp ts0_ = 0;
  std::int64_t max_late_ns_ = 0;
};

struct RunResult {
  ParseStatus status = ParseStatus::Ok;  // End on a clean run
  double seconds = 0;                    // wall time of the loop, parse and apply
};

// Plain replay: parse, optionally pace, apply. Stops at the first parse error
// and returns its code. pacer == nullptr means as fast as possible. limit == 0
// means no limit.
template <EventSink S, class Reader>
RunResult replay(Engine<S>& eng, Reader& reader, ReplayCounters& counters, Pacer* pacer = nullptr,
                 std::uint64_t limit = 0) noexcept {
  FeedMsg m;
  std::uint64_t n = 0;
  const auto t0 = std::chrono::steady_clock::now();
  ParseStatus st;
  while ((limit == 0 || n < limit) && (st = reader.next(m)) == ParseStatus::Ok) {
    if (pacer) pacer->wait_until(m.ts);
    apply_feed(eng, counters, m);
    ++n;
  }
  if (limit != 0 && n >= limit) st = ParseStatus::End;
  const auto t1 = std::chrono::steady_clock::now();
  return {st, std::chrono::duration<double>(t1 - t0).count()};
}

// Parse only, no book: isolates parser cost.
template <class Reader>
RunResult parse_only(Reader& reader, std::uint64_t& count) noexcept {
  FeedMsg m;
  ParseStatus st;
  std::uint64_t n = 0;
  const auto t0 = std::chrono::steady_clock::now();
  while ((st = reader.next(m)) == ParseStatus::Ok) ++n;
  const auto t1 = std::chrono::steady_clock::now();
  count = n;
  return {st, std::chrono::duration<double>(t1 - t0).count()};
}

// ---- grid helpers ------------------------------------------------------------

// Range of displayed-order prices in a message stream (Add and Replace). Takes
// the reader by value, so the caller's reader is not consumed.
template <class Reader>
bool scan_price_range(Reader reader, Price& lo, Price& hi) noexcept {
  FeedMsg m;
  bool any = false;
  while (reader.next(m) == ParseStatus::Ok) {
    if ((m.type != FeedType::Add && m.type != FeedType::Replace) || m.price <= 0) continue;
    if (!any) {
      lo = hi = m.price;
      any = true;
    } else {
      lo = std::min(lo, m.price);
      hi = std::max(hi, m.price);
    }
  }
  return any;
}

// A base price that centers the band on [lo, hi], aligned to the tick.
inline Price suggest_base_price(Price lo, Price hi, Price tick, std::uint32_t num_levels) noexcept {
  const Price mid = lo + (hi - lo) / 2;
  Price base = mid - static_cast<Price>(num_levels / 2) * tick;
  if (base < 0) base = 0;
  return base - base % tick;
}

// ---- snapshot verification ---------------------------------------------------

enum class DivergenceReason : std::uint8_t {
  None = 0,
  RowMismatch,    // top-N levels differ from the LOBSTER order book row
  FeedProblem,    // the engine rejected the message or counted an anomaly
  FieldMismatch,  // message side or price disagrees with the order it names
  RowsExhausted,  // the order book file has fewer rows than the message file
  RowParseError,  // the order book file is malformed
  MessageParseError
};

const char* to_string(DivergenceReason r) noexcept;

struct Divergence {
  DivergenceReason reason = DivergenceReason::None;
  std::uint64_t message_index = 0;  // 0 based
  std::size_t line_number = 0;      // 1 based line in the message file
  FeedMsg msg{};
  char line[256] = {};  // raw message line, truncated
  ParseStatus parse_status = ParseStatus::Ok;
  RejectReason reject = RejectReason::None;  // FeedProblem: which reject counter grew
  std::uint64_t anomalies_delta = 0;         // FeedProblem: how many anomalies this message added
  std::uint32_t levels = 0;
  lobster::BookLevel expected_ask[lobster::kMaxLevels];
  lobster::BookLevel expected_bid[lobster::kMaxLevels];
  lobster::BookLevel actual_ask[lobster::kMaxLevels];
  lobster::BookLevel actual_bid[lobster::kMaxLevels];
};

struct VerifyResult {
  bool ok = false;
  ParseStatus message_status = ParseStatus::Ok;  // End when the whole file verified
  std::uint64_t verified = 0;                    // messages whose row matched
  double seconds = 0;
  Divergence div;
};

namespace detail {

template <EventSink S>
inline void actual_levels(const Book<S>& b, std::uint32_t n, lobster::BookLevel* ask,
                          lobster::BookLevel* bid) noexcept {
  typename Book<S>::LevelView v[lobster::kMaxLevels];
  std::size_t k = b.top_levels(Side::Ask, v, n);
  for (std::size_t i = 0; i < n; ++i)
    ask[i] = i < k ? lobster::BookLevel{v[i].price, static_cast<std::int64_t>(v[i].qty)}
                   : lobster::BookLevel{lobster::kDummyAskPrice, 0};
  k = b.top_levels(Side::Bid, v, n);
  for (std::size_t i = 0; i < n; ++i)
    bid[i] = i < k ? lobster::BookLevel{v[i].price, static_cast<std::int64_t>(v[i].qty)}
                   : lobster::BookLevel{lobster::kDummyBidPrice, 0};
}

inline bool same(const lobster::BookLevel& a, const lobster::BookLevel& b) noexcept {
  return a.price == b.price && a.size == b.size;
}

}  // namespace detail

// Replays a LOBSTER message file and, after every message, compares the top N
// levels of the reconstructed book with the matching row of the order book file.
// Stops at the first divergence. Hard failures, besides a row mismatch: any
// reject or anomaly counter growing, a message whose side or price disagrees with
// the order it names, a missing or malformed row.
template <EventSink S>
VerifyResult replay_verify(Engine<S>& eng, lobster::MessageReader& msgs,
                           lobster::BookRowReader& rows, ReplayCounters& counters,
                           std::uint64_t limit = 0) noexcept {
  VerifyResult res;
  FeedMsg m;
  lobster::BookRow row;
  const auto t0 = std::chrono::steady_clock::now();
  std::uint64_t idx = 0;

  auto fill = [&](DivergenceReason why) noexcept {
    Divergence& d = res.div;
    d.reason = why;
    d.message_index = idx;
    d.line_number = msgs.line_number();
    d.msg = m;
    const std::string_view ln = msgs.last_line();
    const std::size_t n = std::min(ln.size(), sizeof d.line - 1);
    std::memcpy(d.line, ln.data(), n);
    d.line[n] = '\0';
    d.levels = rows.levels();
    detail::actual_levels(eng.book(), d.levels, d.actual_ask, d.actual_bid);
  };

  for (;;) {
    if (limit != 0 && idx >= limit) {
      res.message_status = ParseStatus::End;
      break;
    }
    const ParseStatus st = msgs.next(m);
    if (st == ParseStatus::End) {
      res.message_status = st;
      break;
    }
    if (st != ParseStatus::Ok) {
      res.message_status = st;
      fill(DivergenceReason::MessageParseError);
      res.div.parse_status = st;
      goto done;
    }

    {
      // The message names the side and price of the resting order for types 2, 3
      // and 4. A mismatch with the order we hold means our book already differs.
      if (m.type == FeedType::Reduce || m.type == FeedType::Delete || m.type == FeedType::Execute) {
        if (const Order* o = eng.book().find_order(m.id)) {
          if (o->side != m.side || eng.book().price_of(o->level) != m.price) {
            fill(DivergenceReason::FieldMismatch);
            goto done;
          }
        }
      }

      const BookStats before = eng.book().stats();
      apply_feed(eng, counters, m);
      const BookStats& after = eng.book().stats();
      if (after.feed_problems() != before.feed_problems()) {
        fill(DivergenceReason::FeedProblem);
        for (std::size_t r = 0; r < after.rejects.size(); ++r)
          if (after.rejects[r] != before.rejects[r]) {
            res.div.reject = static_cast<RejectReason>(r);
            break;
          }
        res.div.anomalies_delta = after.anomalies() - before.anomalies();
        goto done;
      }

      const ParseStatus rs = rows.next(row);
      if (rs != ParseStatus::Ok) {
        fill(rs == ParseStatus::End ? DivergenceReason::RowsExhausted
                                    : DivergenceReason::RowParseError);
        res.div.parse_status = rs;
        goto done;
      }
      lobster::BookLevel aa[lobster::kMaxLevels], ab[lobster::kMaxLevels];
      detail::actual_levels(eng.book(), row.levels, aa, ab);
      for (std::uint32_t i = 0; i < row.levels; ++i) {
        if (!detail::same(aa[i], row.ask[i]) || !detail::same(ab[i], row.bid[i])) {
          fill(DivergenceReason::RowMismatch);
          std::memcpy(res.div.expected_ask, row.ask, row.levels * sizeof(lobster::BookLevel));
          std::memcpy(res.div.expected_bid, row.bid, row.levels * sizeof(lobster::BookLevel));
          goto done;
        }
      }
      ++res.verified;
      ++idx;
    }
  }
  res.ok = true;
done:
  const auto t1 = std::chrono::steady_clock::now();
  res.seconds = std::chrono::duration<double>(t1 - t0).count();
  return res;
}

}  // namespace lobkit::md
