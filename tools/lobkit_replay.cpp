// Command line replay of LOBSTER or ITCH files through the engine.
//
//   lobkit_replay --lobster messages.csv [--book orderbook.csv] [--verify]
//   lobkit_replay --itch file.bin [--symbol AAPL | --locate 13]
//
// See --help. Exit status: 0 success (a degraded plain replay still exits 0 and
// says so), 1 snapshot verification failed, 2 usage or input error.
#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "lobkit/book/names.hpp"
#include "lobkit/md/itch.hpp"
#include "lobkit/md/lobster.hpp"
#include "lobkit/md/mapped_file.hpp"
#include "lobkit/md/replay.hpp"

using namespace lobkit;
using namespace lobkit::md;

namespace {

struct Args {
  const char* lobster = nullptr;
  const char* book = nullptr;
  const char* itch = nullptr;
  const char* symbol = nullptr;
  long locate = -1;
  long levels = 0;  // 0: detect from the book file
  Price tick = 100;
  bool auto_base = true;
  Price base = 0;
  std::uint32_t num_levels = 65536;
  std::uint32_t max_orders = 1u << 20;
  bool paced = false;
  double speed = 1.0;
  bool verify = false;
  bool parse_only = false;
  std::uint64_t limit = 0;
  int repeat = 1;
};

void usage() {
  std::puts(
      "usage: lobkit_replay (--lobster MSG [--book ROWS] | --itch FILE) [options]\n"
      "  --levels N          levels in the book file (default: detect)\n"
      "  --symbol S|--locate N   ITCH: restrict to one stock\n"
      "  --tick-size T       price units per tick (default 100 = one cent at 1e-4 dollars)\n"
      "  --base-price B|auto lowest price of the ladder (default auto: centered on the data)\n"
      "  --num-levels L      ladder size, power of two (default 65536)\n"
      "  --max-orders M      order pool size (default 1048576)\n"
      "  --mode afap|paced   default afap; paced releases messages at recorded spacing / speed\n"
      "  --speed X           pacing multiplier (default 1)\n"
      "  --verify            LOBSTER: compare every row of the book file, stop at first "
      "divergence\n"
      "  --parse-only        time the parser alone\n"
      "  --limit N           stop after N messages\n"
      "  --repeat K          repeat the replay K times for timing (fresh engine each time)");
}

bool parse_args(int argc, char** argv, Args& a) {
  for (int i = 1; i < argc; ++i) {
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    const std::string k = argv[i];
    const char* v = nullptr;
    if (k == "--lobster") {
      if (!(v = need("--lobster"))) return false;
      a.lobster = v;
    } else if (k == "--book") {
      if (!(v = need("--book"))) return false;
      a.book = v;
    } else if (k == "--itch") {
      if (!(v = need("--itch"))) return false;
      a.itch = v;
    } else if (k == "--symbol") {
      if (!(v = need("--symbol"))) return false;
      a.symbol = v;
    } else if (k == "--locate") {
      if (!(v = need("--locate"))) return false;
      a.locate = std::atol(v);
    } else if (k == "--levels") {
      if (!(v = need("--levels"))) return false;
      a.levels = std::atol(v);
    } else if (k == "--tick-size") {
      if (!(v = need("--tick-size"))) return false;
      a.tick = std::atoll(v);
    } else if (k == "--base-price") {
      if (!(v = need("--base-price"))) return false;
      if (std::strcmp(v, "auto") == 0)
        a.auto_base = true;
      else {
        a.auto_base = false;
        a.base = std::atoll(v);
      }
    } else if (k == "--num-levels") {
      if (!(v = need("--num-levels"))) return false;
      a.num_levels = static_cast<std::uint32_t>(std::atol(v));
    } else if (k == "--max-orders") {
      if (!(v = need("--max-orders"))) return false;
      a.max_orders = static_cast<std::uint32_t>(std::atol(v));
    } else if (k == "--mode") {
      if (!(v = need("--mode"))) return false;
      a.paced = std::strcmp(v, "paced") == 0;
    } else if (k == "--speed") {
      if (!(v = need("--speed"))) return false;
      a.speed = std::atof(v);
    } else if (k == "--limit") {
      if (!(v = need("--limit"))) return false;
      a.limit = std::strtoull(v, nullptr, 10);
    } else if (k == "--repeat") {
      if (!(v = need("--repeat"))) return false;
      a.repeat = std::atoi(v);
    } else if (k == "--verify")
      a.verify = true;
    else if (k == "--parse-only")
      a.parse_only = true;
    else if (k == "--help" || k == "-h") {
      usage();
      std::exit(0);
    } else {
      std::fprintf(stderr, "unknown option %s\n", k.c_str());
      return false;
    }
  }
  if ((a.lobster == nullptr) == (a.itch == nullptr)) {
    std::fprintf(stderr, "give exactly one of --lobster and --itch\n");
    return false;
  }
  if (a.verify && (!a.lobster || !a.book)) {
    std::fprintf(stderr, "--verify needs --lobster and --book\n");
    return false;
  }
  return true;
}

void print_counters(const ReplayCounters& c, const BookStats& s) {
  std::printf("messages applied      %" PRIu64 "\n", c.messages);
  std::printf("  add / reduce / delete / execute / replace: %" PRIu64 " / %" PRIu64 " / %" PRIu64
              " / %" PRIu64 " / %" PRIu64 "\n",
              c.adds, c.reduces, c.deletes, c.executes, c.replaces);
  std::printf("  hidden executions (not applied): %" PRIu64 " (%" PRIu64 " shares)\n",
              c.hidden_executions, c.hidden_shares);
  std::printf("  non-book trades (not applied):   %" PRIu64 "\n", c.nonbook_trades);
  std::printf("  cross trades (not applied):      %" PRIu64 "\n", c.cross_trades);
  std::printf("  trading halts (not applied):     %" PRIu64 "\n", c.halts);
  std::printf("book trades from executes: %" PRIu64 "\n", s.trades);
  bool any = false;
  for (std::size_t r = 1; r < s.rejects.size(); ++r)
    if (s.rejects[r]) {
      std::printf("reject %-18s %" PRIu64 "\n", to_string(static_cast<RejectReason>(r)),
                  s.rejects[r]);
      any = true;
    }
  std::printf("anomalies: queue_order=%" PRIu64 " crossed_own=%" PRIu64 " crossed_feed=%" PRIu64
              " exec_overflow=%" PRIu64 "\n",
              s.queue_order_anomalies, s.feed_crossed_own_fills, s.feed_crossed_fills,
              s.feed_exec_overflow);
  if (any || s.anomalies())
    std::printf("RUN IS DEGRADED: the reconstructed book did not apply every message cleanly\n");
}

void print_levels(const char* name, const lobster::BookLevel* lv, std::uint32_t n) {
  std::printf("  %-8s", name);
  for (std::uint32_t i = 0; i < n; ++i)
    std::printf(" (%" PRId64 " x %" PRId64 ")", lv[i].price, lv[i].size);
  std::puts("");
}

void print_divergence(const Divergence& d) {
  std::printf("VERIFICATION FAILED: %s at message index %" PRIu64
              " (line %zu of the message file)\n",
              to_string(d.reason), d.message_index, d.line_number);
  std::printf("  message: %s\n", d.line);
  if (d.reason == DivergenceReason::FeedProblem) {
    if (d.reject != RejectReason::None)
      std::printf("  the engine rejected it: %s\n", to_string(d.reject));
    if (d.anomalies_delta)
      std::printf("  anomalies added by this message: %" PRIu64 "\n", d.anomalies_delta);
  }
  if (d.reason == DivergenceReason::RowParseError || d.reason == DivergenceReason::RowsExhausted ||
      d.reason == DivergenceReason::MessageParseError)
    std::printf("  parser status: %s\n", to_string(d.parse_status));
  if (d.reason == DivergenceReason::RowMismatch) {
    std::puts("  expected (price x size), best first:");
    print_levels("ask", d.expected_ask, d.levels);
    print_levels("bid", d.expected_bid, d.levels);
  }
  if (d.levels) {
    std::puts("  actual (reconstructed book):");
    print_levels("ask", d.actual_ask, d.levels);
    print_levels("bid", d.actual_bid, d.levels);
  }
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (!parse_args(argc, argv, a)) {
    usage();
    return 2;
  }
  const char* err = nullptr;
  MappedFile msg_file, book_file;
  if (!msg_file.open(a.lobster ? a.lobster : a.itch, &err)) {
    std::fprintf(stderr, "%s: %s\n", a.lobster ? a.lobster : a.itch, err);
    return 2;
  }
  if (a.book && !book_file.open(a.book, &err)) {
    std::fprintf(stderr, "%s: %s\n", a.book, err);
    return 2;
  }

  std::int32_t locate = static_cast<std::int32_t>(a.locate);
  if (a.itch && a.symbol) {
    std::uint16_t l = 0;
    if (!itch::find_locate(msg_file.bytes(), a.symbol, l)) {
      std::fprintf(stderr, "symbol %s not found in the stock directory messages\n", a.symbol);
      return 2;
    }
    locate = l;
    std::printf("symbol %s -> stock locate %d\n", a.symbol, locate);
  }

  auto make_reader_lobster = [&] { return lobster::MessageReader(msg_file.text()); };
  auto make_reader_itch = [&] { return itch::Decoder(msg_file.bytes(), locate); };

  if (a.parse_only) {
    for (int r = 0; r < a.repeat; ++r) {
      std::uint64_t n = 0;
      RunResult rr;
      if (a.lobster) {
        auto rd = make_reader_lobster();
        rr = parse_only(rd, n);
      } else {
        auto rd = make_reader_itch();
        rr = parse_only(rd, n);
      }
      std::printf("parse only: %" PRIu64 " messages in %.3f s = %.2f M msg/s, final status %s\n", n,
                  rr.seconds, static_cast<double>(n) / rr.seconds / 1e6, to_string(rr.status));
    }
    return 0;
  }

  // Grid.
  BookConfig cfg;
  cfg.tick_size = a.tick;
  cfg.num_levels = a.num_levels;
  cfg.max_orders = a.max_orders;
  cfg.base_price = a.base;
  if (a.auto_base) {
    Price lo = 0, hi = 0;
    const bool any = a.lobster ? scan_price_range(make_reader_lobster(), lo, hi)
                               : scan_price_range(make_reader_itch(), lo, hi);
    if (any) {
      cfg.base_price = suggest_base_price(lo, hi, a.tick, a.num_levels);
      const Price span = (hi - lo) / a.tick;
      std::printf("price range in file: %" PRId64 " .. %" PRId64 " (%" PRId64
                  " ticks); base price %" PRId64 "\n",
                  lo, hi, span, cfg.base_price);
      if (span >= static_cast<Price>(a.num_levels))
        std::printf(
            "WARNING: the range does not fit in %u levels; out-of-band messages are expected\n",
            a.num_levels);
    }
  }
  try {
    cfg.validate();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bad configuration: %s\n", e.what());
    return 2;
  }
  std::printf("grid: tick %" PRId64 ", base %" PRId64 ", %u levels, pool %u orders\n",
              cfg.tick_size, cfg.base_price, cfg.num_levels, cfg.max_orders);

  if (a.verify) {
    Engine<NullSink> eng(cfg);
    lobster::MessageReader mr(msg_file.text());
    std::uint32_t levels = a.levels > 0 ? static_cast<std::uint32_t>(a.levels)
                                        : lobster::detect_levels(book_file.text());
    lobster::BookRowReader br(book_file.text(), levels);
    if (br.status() != ParseStatus::Ok) {
      std::fprintf(stderr, "cannot determine the number of levels in the book file (%s)\n",
                   to_string(br.status()));
      return 2;
    }
    ReplayCounters c;
    const VerifyResult v = replay_verify(eng, mr, br, c, a.limit);
    print_counters(c, eng.book().stats());
    std::printf("%" PRIu64 " messages verified against %u level rows in %.3f s\n", v.verified,
                levels, v.seconds);
    if (!v.ok) {
      print_divergence(v.div);
      return 1;
    }
    std::puts("VERIFICATION PASSED: every row matched");
    return 0;
  }

  std::vector<double> rates;
  for (int r = 0; r < a.repeat; ++r) {
    Engine<NullSink> eng(cfg);
    ReplayCounters c;
    Pacer pacer(a.speed);
    Pacer* pp = a.paced ? &pacer : nullptr;
    RunResult rr;
    ParseStatus st;
    if (a.lobster) {
      auto rd = make_reader_lobster();
      rr = replay(eng, rd, c, pp, a.limit);
      st = rr.status;
      if (st != ParseStatus::End)
        std::printf("stopped at line %zu: %.*s\n", rd.line_number(),
                    static_cast<int>(rd.last_line().size()), rd.last_line().data());
    } else {
      auto rd = make_reader_itch();
      rr = replay(eng, rd, c, pp, a.limit);
      st = rr.status;
      if (st != ParseStatus::End) std::printf("stopped at byte offset %zu\n", rd.frame_offset());
    }
    if (r == 0) {
      print_counters(c, eng.book().stats());
      if (a.paced)
        std::printf("paced at %.2fx: max lateness %.1f us\n", a.speed,
                    static_cast<double>(pacer.max_lateness_ns()) / 1e3);
    }
    const double rate = static_cast<double>(c.messages) / rr.seconds / 1e6;
    rates.push_back(rate);
    std::printf("replay %d: %" PRIu64 " messages in %.3f s = %.2f M msg/s, final status %s\n",
                r + 1, c.messages, rr.seconds, rate, to_string(st));
    if (st != ParseStatus::End) return 2;
  }
  if (rates.size() > 1) {
    std::sort(rates.begin(), rates.end());
    std::printf("median %.2f M msg/s (min %.2f, max %.2f) over %zu runs\n", rates[rates.size() / 2],
                rates.front(), rates.back(), rates.size());
  }
  return 0;
}
