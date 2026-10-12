#include "lobkit/sim/report.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace lobkit::sim {

__extension__ typedef unsigned __int128 UMoney2;

std::string format_fixed(Money2 value, int decimals) {
  const bool neg = value < 0;
  UMoney2 u = neg ? static_cast<UMoney2>(-(value + 1)) + 1 : static_cast<UMoney2>(value);
  UMoney2 scale = 1;
  for (int i = 0; i < decimals; ++i) scale *= 10;
  UMoney2 whole = u / scale, frac = u % scale;
  char digits[48];
  int n = 0;
  if (whole == 0) digits[n++] = '0';
  while (whole > 0) {
    digits[n++] = static_cast<char>('0' + static_cast<int>(whole % 10));
    whole /= 10;
  }
  std::string out;
  if (neg) out += '-';
  while (n > 0) out += digits[--n];
  if (decimals > 0) {
    char fbuf[48];
    for (int i = decimals - 1; i >= 0; --i) {
      fbuf[i] = static_cast<char>('0' + static_cast<int>(frac % 10));
      frac /= 10;
    }
    out += '.';
    out.append(fbuf, static_cast<std::size_t>(decimals));
  }
  return out;
}

namespace {

// Native units are 1e-4 dollars, so a price prints with 4 decimals. Money in the
// accounting is in half native units: dollars = value / 20000 = value * 5 / 100000.
std::string price_str(std::int64_t native) {
  return format_fixed(native, 4);
}
std::string money_str(Money2 half_units) {
  return format_fixed(half_units * 5, 5);
}

}  // namespace

bool write_csv(const RunResult& r, const std::string& prefix, const char** error) {
  struct File {
    std::FILE* f;
    ~File() {
      if (f) std::fclose(f);
    }
  };
  auto open = [&](const std::string& name) {
    return File{std::fopen((prefix + name).c_str(), "w")};
  };

  File fills = open("_fills.csv"), inv = open("_inventory.csv"), pnl = open("_pnl.csv");
  if (!fills.f || !inv.f || !pnl.f) {
    *error = "cannot create output files";
    return false;
  }
  std::fputs("ts_ns,order_id,side,price,qty,role,fee,mid_ref,inventory_after\n", fills.f);
  for (const FillRecord& x : r.fills) {
    std::fprintf(fills.f, "%" PRIu64 ",%" PRIu64 ",%c,%s,%u,%s,%s,%s,%" PRId64 "\n", x.ts, x.id,
                 x.side == Side::Bid ? 'B' : 'S', price_str(x.price).c_str(), x.qty,
                 x.maker ? "maker" : "taker", price_str(x.fee_units).c_str(),
                 money_str(x.mid2_ref).c_str(), x.inventory_after);
  }
  std::fputs("ts_ns,inventory,inventory_not_yet_reported\n", inv.f);
  std::fputs("ts_ns,spread_capture,inventory_pnl,fees,total\n", pnl.f);
  for (const PathSample& s : r.path) {
    std::fprintf(inv.f, "%" PRIu64 ",%" PRId64 ",%" PRId64 "\n", s.ts, s.inventory,
                 s.known_inventory_lag);
    std::fprintf(pnl.f, "%" PRIu64 ",%s,%s,%s,%s\n", s.ts, money_str(s.spread2).c_str(),
                 money_str(s.inventory2).c_str(), money_str(s.fees2).c_str(),
                 money_str(s.spread2 + s.inventory2 - s.fees2).c_str());
  }
  if (std::ferror(fills.f) || std::ferror(inv.f) || std::ferror(pnl.f)) {
    *error = "write error";
    return false;
  }
  return true;
}

std::string summarize(const RunResult& r, const char* data_label) {
  char buf[2048];
  const Pnl& p = r.pnl;
  const SimStats& s = r.stats;
  std::snprintf(
      buf, sizeof buf,
      "data: %s\n"
      "feed messages %" PRIu64
      ", simulated span %.3f s, feed status %s\n"
      "orders sent %" PRIu64 " accepted %" PRIu64 " rejected %" PRIu64 "; cancels sent %" PRIu64
      " confirmed %" PRIu64 " too late %" PRIu64
      "\n"
      "fills %" PRIu64 " (%" PRIu64 " shares; maker %" PRIu64 ", taker %" PRIu64
      "), max |inventory| %" PRId64 ", final inventory %" PRId64
      "\n"
      "PnL in dollars (marked at the final mid):\n"
      "  spread capture  %s\n"
      "  inventory PnL   %s\n"
      "  fees paid       %s   (negative: rebate earned)\n"
      "  total           %s   = spread capture + inventory PnL - fees paid (ledger check: %s)\n"
      "engine anomalies %" PRIu64 ", feed rejects %" PRIu64
      "\n"
      "CAVEATS: the fill model is an approximation; the feed does not react to the strategy (no "
      "market "
      "impact); results on synthetic data say nothing about profitability.\n",
      data_label, s.feed_messages, static_cast<double>(r.last_ts - r.first_ts) * 1e-9,
      md::to_string(r.feed_status), s.orders_sent, s.orders_accepted, s.orders_rejected,
      s.cancels_sent, s.cancels_confirmed, s.cancels_too_late, s.fills, s.shares_filled,
      s.maker_fills, s.taker_fills, s.max_abs_inventory, p.inventory, money_str(p.spread2).c_str(),
      money_str(p.inventory2).c_str(), money_str(p.fees2).c_str(), money_str(p.total2()).c_str(),
      p.total2() == p.total2_from_ledger() ? "matches" : "MISMATCH", r.book.anomalies(), [&] {
        std::uint64_t n = 0;
        for (auto x : r.book.rejects) n += x;
        return n;
      }());
  return buf;
}

std::uint64_t fingerprint(const RunResult& r) {
  std::uint64_t h = 14695981039346656037ull;
  auto mix = [&](std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
      h ^= (v >> (8 * i)) & 0xFF;
      h *= 1099511628211ull;
    }
  };
  for (const FillRecord& f : r.fills) {
    mix(f.ts);
    mix(f.id);
    mix(static_cast<std::uint64_t>(f.side));
    mix(static_cast<std::uint64_t>(f.price));
    mix(f.qty);
    mix(f.maker);
    mix(static_cast<std::uint64_t>(f.fee_units));
    mix(static_cast<std::uint64_t>(f.mid2_ref));
    mix(static_cast<std::uint64_t>(f.inventory_after));
  }
  for (const PathSample& s : r.path) {
    mix(s.ts);
    mix(static_cast<std::uint64_t>(s.inventory));
    mix(static_cast<std::uint64_t>(s.spread2));
    mix(static_cast<std::uint64_t>(s.inventory2));
    mix(static_cast<std::uint64_t>(s.fees2));
  }
  mix(static_cast<std::uint64_t>(r.pnl.cash2));
  return h;
}

}  // namespace lobkit::sim
