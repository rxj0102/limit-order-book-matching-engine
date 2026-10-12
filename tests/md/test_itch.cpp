#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "itch_builder.hpp"
#include "lobkit/book/invariants.hpp"
#include "lobkit/md/itch.hpp"
#include "lobkit/md/mapped_file.hpp"
#include "lobkit/md/replay.hpp"
#include "workload.hpp"

using namespace lobkit;
using namespace lobkit::md;
namespace ib = lobkit::testing::itch;

namespace {
ParseStatus first(const ib::Bytes& b, FeedMsg& m, std::int32_t locate = -1) {
  itch::Decoder d(std::span<const std::uint8_t>(b.data(), b.size()), locate);
  return d.next(m);
}
}  // namespace

// ---- the hand built fixture ----------------------------------------------------

class ItchFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* err = nullptr;
    ASSERT_TRUE(file.open(LOBKIT_DATA_DIR "/itch_fixture.bin", &err)) << err;
  }
  MappedFile file;
};

TEST_F(ItchFixture, DecodesTheStockLocateSubsetInOrder) {
  itch::Decoder d(file.bytes(), /*locate=*/1);
  FeedMsg m;
  struct Want {
    FeedType type;
    OrderId id;
    Qty qty;
    Price price;
  };
  const Want want[] = {{FeedType::Add, 1001, 100, 1500000},    {FeedType::Add, 1002, 200, 1500100},
                       {FeedType::Add, 1003, 300, 1499900},    {FeedType::Execute, 1001, 40, 0},
                       {FeedType::Execute, 1002, 50, 0},       {FeedType::Reduce, 1003, 100, 0},
                       {FeedType::Replace, 1001, 80, 1500000}, {FeedType::Trade, 0, 25, 1500050},
                       {FeedType::Delete, 1002, 0, 0}};
  for (const Want& w : want) {
    ASSERT_EQ(d.next(m), ParseStatus::Ok);
    EXPECT_EQ(m.type, w.type);
    EXPECT_EQ(m.id, w.id);
    EXPECT_EQ(m.qty, w.qty);
    EXPECT_EQ(m.price, w.price);
  }
  EXPECT_EQ(d.next(m), ParseStatus::End);
  const auto& s = d.stats();
  EXPECT_EQ(s.frames, 14u);
  EXPECT_EQ(s.delivered, 9u);
  EXPECT_EQ(s.other_locate, 1u);     // the MSFT add
  EXPECT_EQ(s.skipped_unknown, 1u);  // the H frame
  EXPECT_EQ(s.system_events, 1u);
  EXPECT_EQ(s.directory_entries, 2u);
}

TEST_F(ItchFixture, FieldsOfEveryDecodedType) {
  itch::Decoder d(file.bytes(), 1);
  FeedMsg m;
  ASSERT_EQ(d.next(m), ParseStatus::Ok);  // A 1001 B 100 @150.0000
  EXPECT_EQ(m.side, Side::Bid);
  EXPECT_EQ(m.ts, 10u);
  ASSERT_EQ(d.next(m), ParseStatus::Ok);  // A 1002 S
  EXPECT_EQ(m.side, Side::Ask);
  ASSERT_EQ(d.next(m), ParseStatus::Ok);  // F carries a 4 byte attribution after the price
  EXPECT_EQ(m.aux, 'F');
  EXPECT_EQ(m.price, 1499900);
  ASSERT_EQ(d.next(m), ParseStatus::Ok);  // E
  ASSERT_EQ(d.next(m), ParseStatus::Ok);  // C: price field is the execution price, not used
  EXPECT_EQ(m.aux, 'C');
  ASSERT_EQ(d.next(m), ParseStatus::Ok);  // X
  ASSERT_EQ(d.next(m), ParseStatus::Ok);  // U
  EXPECT_EQ(m.id, 1001u);
  EXPECT_EQ(m.new_id, 1004u);
  ASSERT_EQ(d.next(m), ParseStatus::Ok);  // P
  EXPECT_EQ(m.side, Side::Bid);
}

TEST_F(ItchFixture, WithoutAFilterEveryStockIsDelivered) {
  itch::Decoder d(file.bytes());
  FeedMsg m;
  std::uint64_t n = 0;
  while (d.next(m) == ParseStatus::Ok) ++n;
  EXPECT_EQ(n, 10u);  // the nine above plus MSFT
  EXPECT_EQ(d.stats().other_locate, 0u);
}

TEST_F(ItchFixture, FindsLocateBySymbol) {
  std::uint16_t loc = 0;
  EXPECT_TRUE(itch::find_locate(file.bytes(), "AAPL", loc));
  EXPECT_EQ(loc, 1u);
  EXPECT_TRUE(itch::find_locate(file.bytes(), "MSFT", loc));
  EXPECT_EQ(loc, 2u);
  EXPECT_FALSE(itch::find_locate(file.bytes(), "GOOG", loc));
  EXPECT_FALSE(itch::find_locate(file.bytes(), "", loc));
  EXPECT_FALSE(itch::find_locate(file.bytes(), "TOOLONGSYM", loc));
}

TEST_F(ItchFixture, ReplayReconstructsTheHandDerivedBook) {
  BookConfig cfg;
  cfg.tick_size = 100;
  cfg.base_price = 1400000;
  cfg.num_levels = 1024;
  cfg.max_orders = 1024;
  Engine<NullSink> eng(cfg);
  itch::Decoder d(file.bytes(), 1);
  ReplayCounters c;
  const RunResult r = replay(eng, d, c);
  EXPECT_EQ(r.status, ParseStatus::End);
  EXPECT_EQ(c.messages, 9u);
  EXPECT_EQ(c.nonbook_trades, 1u);
  EXPECT_EQ(c.replaces, 1u);
  const auto& b = eng.book();
  EXPECT_EQ(b.stats().feed_problems(), 0u);
  EXPECT_FALSE(b.has_ask());
  ASSERT_TRUE(b.has_bid());
  EXPECT_EQ(b.best_bid(), 1500000);
  EXPECT_EQ(b.depth_at(1500000), 80u);   // order 1004, the replacement
  EXPECT_EQ(b.depth_at(1499900), 200u);  // order 1003 after the cancel of 100
  EXPECT_EQ(b.live_orders(), 2u);
  EXPECT_NE(b.find_order(1004), nullptr);
  EXPECT_EQ(b.find_order(1001), nullptr);  // replaced orders are gone
  EXPECT_FALSE(check_invariants(b).has_value());
}

// ---- framing errors -------------------------------------------------------------

TEST(ItchFraming, EmptyInputIsAnEmptyStream) {
  itch::Decoder d(std::span<const std::uint8_t>{});
  FeedMsg m;
  EXPECT_EQ(d.next(m), ParseStatus::End);
}

TEST(ItchFraming, ZeroLengthFrame) {
  ib::Bytes b{0, 0};
  FeedMsg m;
  EXPECT_EQ(first(b, m), ParseStatus::ZeroLength);
}

TEST(ItchFraming, HalfALengthPrefixIsTruncated) {
  ib::Bytes b{0};
  FeedMsg m;
  EXPECT_EQ(first(b, m), ParseStatus::Truncated);
}

TEST(ItchFraming, FrameLongerThanTheRestOfTheInputIsTruncated) {
  ib::Bytes b = ib::framed(ib::add(1, 5, 1, 'B', 10, 100));
  b.pop_back();  // lose the last byte of the price
  FeedMsg m;
  EXPECT_EQ(first(b, m), ParseStatus::Truncated);
  ib::Bytes huge{0xFF, 0xFF, 'A'};  // claims 65535 bytes, has 1
  EXPECT_EQ(first(huge, m), ParseStatus::Truncated);
}

TEST(ItchFraming, KnownTypeWithWrongLengthIsRejected) {
  FeedMsg m;
  ib::Bytes shorter = ib::add(1, 5, 1, 'B', 10, 100);
  shorter.pop_back();
  EXPECT_EQ(first(ib::framed(shorter), m), ParseStatus::BadLength);
  ib::Bytes longer = ib::add(1, 5, 1, 'B', 10, 100);
  longer.push_back(0);  // an oversized A frame that is fully present
  EXPECT_EQ(first(ib::framed(longer), m), ParseStatus::BadLength);
  ib::Bytes del = ib::del(1, 5, 9);
  del.push_back(0);
  EXPECT_EQ(first(ib::framed(del), m), ParseStatus::BadLength);
}

TEST(ItchFraming, UnknownTypesAreSkippedByLength) {
  ib::Bytes b;
  ib::append(b, ib::framed(ib::Bytes{'Z'}));                          // one byte message
  ib::append(b, ib::framed(ib::Bytes{'Q', 1, 2, 3, 4, 5, 6, 7, 8}));  // nine bytes
  ib::append(b, ib::framed(ib::add(1, 5, 7, 'S', 3, 300)));
  itch::Decoder d(std::span<const std::uint8_t>(b.data(), b.size()));
  FeedMsg m;
  ASSERT_EQ(d.next(m), ParseStatus::Ok);
  EXPECT_EQ(m.id, 7u);
  EXPECT_EQ(d.stats().skipped_unknown, 2u);
  EXPECT_EQ(d.next(m), ParseStatus::End);
}

TEST(ItchFraming, BadBuySellIndicator) {
  FeedMsg m;
  EXPECT_EQ(first(ib::framed(ib::add(1, 5, 1, 'X', 10, 100)), m), ParseStatus::BadField);
}

TEST(ItchFraming, ErrorsAreSticky) {
  ib::Bytes b = ib::framed(ib::add(1, 5, 1, 'B', 10, 100));
  ib::append(b, ib::Bytes{0, 0});  // zero length frame after a good one
  ib::append(b, ib::framed(ib::add(1, 6, 2, 'B', 10, 100)));
  itch::Decoder d(std::span<const std::uint8_t>(b.data(), b.size()));
  FeedMsg m;
  ASSERT_EQ(d.next(m), ParseStatus::Ok);
  EXPECT_EQ(d.next(m), ParseStatus::ZeroLength);
  EXPECT_EQ(d.next(m), ParseStatus::ZeroLength);  // does not resynchronize and read the next frame
}

TEST(ItchFraming, BigEndianDecodingAtExtremeValues) {
  ib::Bytes p =
      ib::add(0xFFFF, 0xFFFFFFFFFFFFull, 0xFFEEDDCCBBAA9988ull, 'S', 0xFFFFFFFFu, 0xFFFFFFFFu);
  FeedMsg m;
  ASSERT_EQ(first(ib::framed(p), m), ParseStatus::Ok);
  EXPECT_EQ(m.ts, 0xFFFFFFFFFFFFull);
  EXPECT_EQ(m.id, 0xFFEEDDCCBBAA9988ull);
  EXPECT_EQ(m.qty, 0xFFFFFFFFu);
  EXPECT_EQ(m.price, 0xFFFFFFFFll);  // unsigned on the wire, never sign extended
  ib::Bytes q =
      ib::add(0x0102, 0x010203040506ull, 0x0102030405060708ull, 'B', 0x01020304u, 0x0A0B0C0Du);
  ASSERT_EQ(first(ib::framed(q), m), ParseStatus::Ok);
  EXPECT_EQ(m.ts, 0x010203040506ull);
  EXPECT_EQ(m.id, 0x0102030405060708ull);
  EXPECT_EQ(m.qty, 0x01020304u);
  EXPECT_EQ(m.price, 0x0A0B0C0D);
}

TEST(ItchFraming, LocateFilterAppliesToEveryBookMessageType) {
  ib::Bytes b;
  ib::append(b, ib::framed(ib::add(2, 1, 1, 'B', 1, 1)));
  ib::append(b, ib::framed(ib::executed(2, 2, 1, 1)));
  ib::append(b, ib::framed(ib::cancel(2, 3, 1, 1)));
  ib::append(b, ib::framed(ib::del(2, 4, 1)));
  ib::append(b, ib::framed(ib::replace(2, 5, 1, 2, 1, 1)));
  ib::append(b, ib::framed(ib::add(1, 6, 9, 'B', 1, 1, 'F')));
  itch::Decoder d(std::span<const std::uint8_t>(b.data(), b.size()), 1);
  FeedMsg m;
  ASSERT_EQ(d.next(m), ParseStatus::Ok);
  EXPECT_EQ(m.id, 9u);
  EXPECT_EQ(d.stats().other_locate, 5u);
}

// Every prefix of the fixture must end in a clean End or a Truncated, and never
// deliver a message that was not complete in the prefix.
TEST_F(ItchFixture, EveryPrefixDecodesCleanly) {
  const auto all = file.bytes();
  std::size_t pos = 0;
  std::vector<std::size_t> boundaries{0};
  while (pos < all.size()) {
    pos += 2u + ((static_cast<std::size_t>(all[pos]) << 8) | all[pos + 1]);
    boundaries.push_back(pos);
  }
  ASSERT_EQ(pos, all.size());
  for (std::size_t cut = 0; cut <= all.size(); ++cut) {
    itch::Decoder d(all.first(cut));
    FeedMsg m;
    ParseStatus st;
    while ((st = d.next(m)) == ParseStatus::Ok) {
    }
    const bool on_boundary =
        std::find(boundaries.begin(), boundaries.end(), cut) != boundaries.end();
    EXPECT_EQ(st, on_boundary ? ParseStatus::End : ParseStatus::Truncated) << cut;
  }
}

TEST(ItchFraming, RandomBytesNeverCrashTheDecoder) {
  lobkit::testing::SplitMix64 rng(1234);
  const char types[] = {'A', 'F', 'E', 'C', 'X', 'D', 'U', 'P', 'S', 'R', 'Z'};
  for (int iter = 0; iter < 30000; ++iter) {
    ib::Bytes b;
    const std::size_t n = rng.below(200);
    for (std::size_t i = 0; i < n; ++i) b.push_back(static_cast<std::uint8_t>(rng.below(256)));
    // Seed some plausible frames so decoding goes deeper than the length check.
    if (rng.below(2) && b.size() > 3) {
      b[0] = 0;
      b[1] = static_cast<std::uint8_t>(rng.below(45));
      b[2] = static_cast<std::uint8_t>(types[rng.below(sizeof types)]);
    }
    itch::Decoder d(std::span<const std::uint8_t>(b.data(), b.size()), rng.below(3) ? -1 : 1);
    FeedMsg m;
    for (int k = 0; k < 100 && d.next(m) == ParseStatus::Ok; ++k) {
    }
    std::uint16_t loc;
    (void)itch::find_locate(std::span<const std::uint8_t>(b.data(), b.size()), "AAPL", loc);
  }
  SUCCEED();
}
