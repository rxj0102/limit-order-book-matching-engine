#pragma once
// SYNTHETIC feed generator (not market data): a random limit order book process
// that emits FeedMsg records (adds near the mid, partial and full cancels, executes
// at the front of the best queues). It keeps its own model of the feed book so every
// message is consistent (a valid id, executes only hit the oldest order at a level)
// and the book is never crossed. Nothing here is market data.

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "lobkit/md/feed.hpp"
#include "lobkit/md/status.hpp"

namespace lobkit::sim::synthetic {

// Same algorithm as the test workload PRNG (SplitMix64), kept local so this header stands alone.
struct SplitMix64 {
  std::uint64_t s;
  explicit SplitMix64(std::uint64_t seed) : s(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }
};

struct SynthParams {
  std::uint64_t seed = 1;
  std::size_t messages = 20000;
  Price mid0 = 1000000;  // $100.00 in 1e-4 dollars
  Price tick = 100;
  Timestamp t0 = 34'200'000'000'000ull;
  Timestamp max_gap_ns = 2'000'000;  // average gap about 1 ms
  double exec_share = 0.30;          // share of messages that are executions
};

struct FeedBook {
  struct Ord {
    OrderId id;
    Qty qty;
  };
  std::map<Price, std::deque<Ord>> bids, asks;
};

inline std::vector<md::FeedMsg> make_synth_feed(const SynthParams& sp) {
  SplitMix64 rng(sp.seed);
  std::vector<md::FeedMsg> out;
  out.reserve(sp.messages);
  FeedBook book;
  std::map<OrderId, std::pair<Side, Price>> where;
  std::vector<OrderId> live;
  OrderId next_id = 1000;
  Timestamp t = sp.t0;
  Price mid = sp.mid0;

  auto best_bid = [&] { return book.bids.empty() ? Price(0) : book.bids.rbegin()->first; };
  auto best_ask = [&] { return book.asks.empty() ? Price(0) : book.asks.begin()->first; };
  auto emit = [&](md::FeedType ty, OrderId id, Side side, Price px, Qty q, std::uint8_t aux) {
    md::FeedMsg m{};
    m.ts = t;
    m.id = id;
    m.type = ty;
    m.side = side;
    m.price = px;
    m.qty = q;
    m.aux = aux;
    out.push_back(m);
  };
  auto remove_live = [&](OrderId id) {
    for (std::size_t i = 0; i < live.size(); ++i)
      if (live[i] == id) {
        live[i] = live.back();
        live.pop_back();
        return;
      }
  };
  auto reduce = [&](OrderId id, Qty q, bool all) {
    auto [side, px] = where[id];
    auto& lv = side == Side::Bid ? book.bids : book.asks;
    auto& dq = lv[px];
    for (auto it = dq.begin(); it != dq.end(); ++it)
      if (it->id == id) {
        if (all || q >= it->qty) {
          dq.erase(it);
          where.erase(id);
          remove_live(id);
        } else
          it->qty -= q;
        break;
      }
    if (dq.empty()) lv.erase(px);
  };

  for (std::size_t i = 0; i < sp.messages; ++i) {
    t += 1000 + rng.below(sp.max_gap_ns);
    if (rng.below(40) == 0) mid += sp.tick * (rng.below(2) ? 1 : -1);
    const bool need_bid = book.bids.empty(), need_ask = book.asks.empty();
    const unsigned r = static_cast<unsigned>(rng.below(1000));
    const unsigned exec_cut = static_cast<unsigned>(sp.exec_share * 1000);
    if (need_bid || need_ask || r >= exec_cut + 250 || live.size() < 6) {
      // add
      const Side side = need_bid   ? Side::Bid
                        : need_ask ? Side::Ask
                                   : (rng.below(2) ? Side::Bid : Side::Ask);
      Price off = sp.tick * static_cast<Price>(1 + rng.below(6));
      Price px = side == Side::Bid ? mid - off : mid + off;
      if (side == Side::Bid && best_ask() != 0 && px >= best_ask()) px = best_ask() - sp.tick;
      if (side == Side::Ask && best_bid() != 0 && px <= best_bid()) px = best_bid() + sp.tick;
      const Qty q = static_cast<Qty>(50 + rng.below(450));
      const OrderId id = next_id++;
      (side == Side::Bid ? book.bids : book.asks)[px].push_back({id, q});
      where[id] = {side, px};
      live.push_back(id);
      emit(md::FeedType::Add, id, side, px, q, 1);
    } else if (r < exec_cut) {
      // execute the oldest order at the best bid or best ask
      const Side side = rng.below(2) ? Side::Bid : Side::Ask;
      auto& lv = side == Side::Bid ? book.bids : book.asks;
      if (lv.empty()) {
        --i;
        continue;
      }
      auto& dq = side == Side::Bid ? lv.rbegin()->second : lv.begin()->second;
      const Price px = side == Side::Bid ? lv.rbegin()->first : lv.begin()->first;
      const FeedBook::Ord head = dq.front();
      const Qty q = rng.below(100) < 60 ? head.qty : static_cast<Qty>(1 + rng.below(head.qty));
      emit(md::FeedType::Execute, head.id, side, px, q, 4);
      reduce(head.id, q, false);
    } else {
      // cancel or reduce a random live order
      const OrderId id = live[rng.below(live.size())];
      auto [side, px] = where[id];
      if (rng.below(3) == 0) {
        auto& dq = (side == Side::Bid ? book.bids : book.asks)[px];
        Qty cur = 0;
        for (auto& o : dq)
          if (o.id == id) cur = o.qty;
        if (cur > 1) {
          const Qty q = static_cast<Qty>(1 + rng.below(cur - 1));
          emit(md::FeedType::Reduce, id, side, px, q, 2);
          reduce(id, q, false);
          continue;
        }
      }
      auto& dq = (side == Side::Bid ? book.bids : book.asks)[px];
      Qty cur = 0;
      for (auto& o : dq)
        if (o.id == id) cur = o.qty;
      emit(md::FeedType::Delete, id, side, px, cur, 3);
      reduce(id, 0, true);
    }
  }
  return out;
}

// A reader over a message array it does not own (for example the buffer of a numpy array),
// with the same interface as the file readers.
class SpanFeed {
 public:
  SpanFeed(const md::FeedMsg* data, std::size_t n) : p_(data), n_(n) {}
  md::ParseStatus next(md::FeedMsg& m) {
    if (i_ >= n_) return md::ParseStatus::End;
    m = p_[i_++];
    return md::ParseStatus::Ok;
  }

 private:
  const md::FeedMsg* p_;
  std::size_t n_;
  std::size_t i_ = 0;
};

// A reader over a fixed message vector, with the same interface as the file readers.
class VectorFeed {
 public:
  explicit VectorFeed(std::vector<md::FeedMsg> m) : msgs_(std::move(m)) {}
  md::ParseStatus next(md::FeedMsg& m) {
    if (i_ >= msgs_.size()) return md::ParseStatus::End;
    m = msgs_[i_++];
    return md::ParseStatus::Ok;
  }
  const std::vector<md::FeedMsg>& messages() const { return msgs_; }

 private:
  std::vector<md::FeedMsg> msgs_;
  std::size_t i_ = 0;
};

}  // namespace lobkit::sim::synthetic
