#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "lobkit/md/lobster.hpp"
#include "workload.hpp"

using namespace lobkit;
using namespace lobkit::md;
using lobster::BookRow;
using lobster::BookRowReader;
using lobster::MessageReader;

namespace {
std::string slurp(const std::string& name) {
  std::ifstream f(std::string(LOBKIT_DATA_DIR) + "/" + name, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
ParseStatus first_status(const std::string& line) {
  MessageReader r(line);
  FeedMsg m;
  return r.next(m);
}
}  // namespace

TEST(LobsterMessages, EveryMessageTypeMapsToItsFeedType) {
  const std::string text =
      "34200.004241176,1,16113575,18,5853300,1\n"
      "34200.100000000,2,16113575,8,5853300,1\n"
      "34200.200000000,3,16113575,10,5853300,1\n"
      "34200.300000000,4,16113576,5,5853400,-1\n"
      "34200.400000000,5,0,7,5853400,-1\n"
      "34200.500000000,6,0,100,5853500,1\n"
      "34200.600000000,7,0,0,-1,-1\n";
  MessageReader r(text);
  FeedMsg m;
  const FeedType want[] = {FeedType::Add,     FeedType::Reduce,        FeedType::Delete,
                           FeedType::Execute, FeedType::ExecuteHidden, FeedType::CrossTrade,
                           FeedType::Halt};
  for (int i = 0; i < 7; ++i) {
    ASSERT_EQ(r.next(m), ParseStatus::Ok) << i;
    EXPECT_EQ(m.type, want[i]) << i;
    EXPECT_EQ(m.aux, i + 1);
  }
  EXPECT_EQ(r.next(m), ParseStatus::End);
  EXPECT_EQ(r.next(m), ParseStatus::End);  // stays at the end
}

TEST(LobsterMessages, FieldsAreDecodedExactly) {
  MessageReader r("34200.004241176,1,16113575,18,5853300,1\n34201,4,9,5,100,-1\n");
  FeedMsg m;
  ASSERT_EQ(r.next(m), ParseStatus::Ok);
  EXPECT_EQ(m.ts, 34200004241176ull);  // seconds to nanoseconds with no floating point
  EXPECT_EQ(m.id, 16113575u);
  EXPECT_EQ(m.qty, 18u);
  EXPECT_EQ(m.price, 5853300);
  EXPECT_EQ(m.side, Side::Bid);  // direction 1 is a buy order
  ASSERT_EQ(r.next(m), ParseStatus::Ok);
  EXPECT_EQ(m.ts, 34201000000000ull);  // no fraction at all is accepted
  EXPECT_EQ(m.side, Side::Ask);
}

TEST(LobsterMessages, FractionLengthsOneToNineDigits) {
  FeedMsg m;
  MessageReader a("1.5,1,1,1,1,1\n");
  ASSERT_EQ(a.next(m), ParseStatus::Ok);
  EXPECT_EQ(m.ts, 1500000000ull);
  MessageReader b("0.000000001,1,1,1,1,1\n");
  ASSERT_EQ(b.next(m), ParseStatus::Ok);
  EXPECT_EQ(m.ts, 1ull);
}

TEST(LobsterMessages, WindowsLineEndingsAndMissingFinalNewline) {
  MessageReader r("1.0,1,1,1,1,1\r\n2.0,3,1,1,1,1");
  FeedMsg m;
  ASSERT_EQ(r.next(m), ParseStatus::Ok);
  ASSERT_EQ(r.next(m), ParseStatus::Ok);  // a complete last line needs no newline
  EXPECT_EQ(m.type, FeedType::Delete);
  EXPECT_EQ(r.next(m), ParseStatus::End);
}

TEST(LobsterMessages, EmptyInputIsAnEmptyFile) {
  MessageReader r("");
  FeedMsg m;
  EXPECT_EQ(r.next(m), ParseStatus::End);
}

TEST(LobsterMessages, ErrorsAreReportedWithTheOffendingLine) {
  MessageReader r("1.0,1,1,1,1,1\n2.0,1,1,1\n3.0,1,1,1,1,1\n");
  FeedMsg m;
  ASSERT_EQ(r.next(m), ParseStatus::Ok);
  EXPECT_EQ(r.next(m), ParseStatus::BadField);  // too few fields, line is terminated
  EXPECT_EQ(r.line_number(), 2u);
  EXPECT_EQ(r.last_line(), "2.0,1,1,1");
  EXPECT_EQ(r.next(m), ParseStatus::BadField);  // sticky: it does not skip past the damage
  EXPECT_EQ(r.line_number(), 2u);
}

TEST(LobsterMessages, EachErrorCodeHasATrigger) {
  EXPECT_EQ(first_status("1.0,1,1,1,1,1,1\n"), ParseStatus::BadField);      // seventh field
  EXPECT_EQ(first_status("1.0,1,1,1,1,1x\n"), ParseStatus::BadField);       // trailing garbage
  EXPECT_EQ(first_status("1.0,1,1,1,1\n"), ParseStatus::BadField);          // missing direction
  EXPECT_EQ(first_status("\n1.0,1,1,1,1,1\n"), ParseStatus::BadTimestamp);  // blank line
  EXPECT_EQ(first_status("1.0,0,1,1,1,1\n"), ParseStatus::BadType);
  EXPECT_EQ(first_status("1.0,8,1,1,1,1\n"), ParseStatus::BadType);
  EXPECT_EQ(first_status("1.0,1,1,1,1,0\n"), ParseStatus::BadField);  // direction must be +-1
  EXPECT_EQ(first_status("1.0,1,1,1,1,2\n"), ParseStatus::BadField);
  EXPECT_EQ(first_status("1.0,1,1,-1,1,1\n"), ParseStatus::BadField);          // negative size
  EXPECT_EQ(first_status("1.0,1,1,4294967296,1,1\n"), ParseStatus::Overflow);  // size above uint32
  EXPECT_EQ(first_status("1.0,1,99999999999999999999,1,1,1\n"), ParseStatus::Overflow);
  EXPECT_EQ(first_status("1.0,1,1,1,9223372036854775808,1\n"), ParseStatus::Overflow);
  EXPECT_EQ(first_status("1.0,1,1,1,abc,1\n"), ParseStatus::BadField);
  EXPECT_EQ(first_status("1.0000000001,1,1,1,1,1\n"), ParseStatus::BadTimestamp);  // 10 decimals
  EXPECT_EQ(first_status("1.,1,1,1,1,1\n"), ParseStatus::BadTimestamp);
  EXPECT_EQ(first_status(".5,1,1,1,1,1\n"), ParseStatus::BadTimestamp);
  EXPECT_EQ(first_status("99999999999,1,1,1,1,1\n"),
            ParseStatus::BadTimestamp);                                // would overflow ns
  EXPECT_EQ(first_status("1.0 ,1,1,1,1,1\n"), ParseStatus::BadField);  // no tolerance for spaces
}

TEST(LobsterMessages, TruncatedLastLineIsTruncatedNotMalformed) {
  EXPECT_EQ(first_status("1.0,1,1,1"), ParseStatus::Truncated);  // cut mid record, no newline
  EXPECT_EQ(first_status("1.0,1,1,1,"), ParseStatus::Truncated);
  EXPECT_EQ(first_status("1.0,1,1,1\n"), ParseStatus::BadField);  // same text, but terminated
}

TEST(LobsterMessages, NegativePricesAndHaltCodesAreKept) {
  MessageReader r("1.0,7,0,0,-1,-1\n2.0,7,0,0,0,-1\n");
  FeedMsg m;
  ASSERT_EQ(r.next(m), ParseStatus::Ok);
  EXPECT_EQ(m.type, FeedType::Halt);
  EXPECT_EQ(m.price, -1);
  ASSERT_EQ(r.next(m), ParseStatus::Ok);
  EXPECT_EQ(m.price, 0);
}

// Every prefix of a real-looking file must parse to a clean prefix of the same
// messages followed by End or a truncation/format error, never anything else and
// never a read past the end (ASan and UBSan watch for that when this runs).
TEST(LobsterMessages, EveryPrefixOfTheSyntheticFileParsesCleanly) {
  const std::string full = slurp("SYNTH_lobster_1000_message_10.csv");
  ASSERT_GT(full.size(), 1000u);
  std::size_t complete_lines_before = 0;
  for (std::size_t cut = 0; cut <= 3000; ++cut) {
    if (cut > 0 && full[cut - 1] == '\n') ++complete_lines_before;
    const std::string_view prefix(full.data(), cut);
    MessageReader r(prefix);
    FeedMsg m;
    std::size_t ok = 0;
    ParseStatus st;
    while ((st = r.next(m)) == ParseStatus::Ok) ++ok;
    const bool ends_mid_line = cut > 0 && full[cut - 1] != '\n';
    if (!ends_mid_line) {
      EXPECT_EQ(st, ParseStatus::End) << cut;
      EXPECT_EQ(ok, complete_lines_before) << cut;
    } else {
      // A cut inside a line: either the partial line happens to be a complete
      // record (cut right before the newline) or it is reported as an error.
      EXPECT_TRUE(st == ParseStatus::End || st == ParseStatus::Truncated ||
                  st == ParseStatus::BadField || st == ParseStatus::BadTimestamp ||
                  st == ParseStatus::Overflow || st == ParseStatus::BadType)
          << cut << " " << to_string(st);
      EXPECT_GE(ok, complete_lines_before) << cut;
      EXPECT_LE(ok, complete_lines_before + 1) << cut;
    }
  }
}

TEST(LobsterMessages, RandomBytesNeverCrashTheParser) {
  lobkit::testing::SplitMix64 rng(99);
  const char alphabet[] = "0123456789,.-\n\r x";
  for (int iter = 0; iter < 20000; ++iter) {
    std::string s;
    const std::size_t n = rng.below(120);
    for (std::size_t i = 0; i < n; ++i) s.push_back(alphabet[rng.below(sizeof alphabet - 1)]);
    MessageReader r(s);
    FeedMsg m;
    for (int k = 0; k < 200 && r.next(m) == ParseStatus::Ok; ++k) {
    }
    BookRowReader b(s, 2);
    BookRow row;
    for (int k = 0; k < 200 && b.next(row) == ParseStatus::Ok; ++k) {
    }
  }
  SUCCEED();
}

// ---- order book file ---------------------------------------------------------

TEST(LobsterBook, DetectsLevelCount) {
  EXPECT_EQ(lobster::detect_levels("1,2,3,4\n"), 1u);
  EXPECT_EQ(lobster::detect_levels("1,2,3,4,5,6,7,8\n"), 2u);
  EXPECT_EQ(lobster::detect_levels("1,2,3\n"), 0u);
  EXPECT_EQ(lobster::detect_levels(""), 0u);
  EXPECT_EQ(lobster::detect_levels(slurp("SYNTH_lobster_1000_orderbook_10.csv")), 10u);
}

TEST(LobsterBook, ParsesRowsIncludingDummyLevels) {
  BookRowReader r("9999999999,0,5853300,18,9999999999,0,-9999999999,0\n100,5,99,4,101,6,98,3\n", 2);
  BookRow row;
  ASSERT_EQ(r.next(row), ParseStatus::Ok);
  EXPECT_EQ(row.levels, 2u);
  EXPECT_EQ(row.ask[0].price, lobster::kDummyAskPrice);
  EXPECT_EQ(row.ask[0].size, 0);
  EXPECT_EQ(row.bid[0].price, 5853300);
  EXPECT_EQ(row.bid[0].size, 18);
  EXPECT_EQ(row.bid[1].price, lobster::kDummyBidPrice);
  ASSERT_EQ(r.next(row), ParseStatus::Ok);
  EXPECT_EQ(row.ask[0].price, 100);
  EXPECT_EQ(row.bid[0].price, 99);
  EXPECT_EQ(row.ask[1].price, 101);
  EXPECT_EQ(row.bid[1].price, 98);
  EXPECT_EQ(r.next(row), ParseStatus::End);
}

TEST(LobsterBook, ErrorPaths) {
  BookRow row;
  {
    BookRowReader r("1,2,3\n", 1);
    EXPECT_EQ(r.next(row), ParseStatus::BadField);
  }
  {
    BookRowReader r("1,2,3,4,5\n", 1);
    EXPECT_EQ(r.next(row), ParseStatus::BadField);  // extra field
  }
  {
    BookRowReader r("1,2,3,4", 2);
    EXPECT_EQ(r.next(row), ParseStatus::Truncated);  // cut mid row, no newline
  }
  {
    BookRowReader r("1,2,3,x\n", 1);
    EXPECT_EQ(r.next(row), ParseStatus::BadField);
  }
  {
    BookRowReader r("1,2,3,99999999999999999999\n", 1);
    EXPECT_EQ(r.next(row), ParseStatus::Overflow);
  }
  {
    BookRowReader r("1,2,3,4\n", 0);
    EXPECT_EQ(r.next(row), ParseStatus::BadLevelCount);
  }
  {
    BookRowReader r("1,2,3,4\n", lobster::kMaxLevels + 1);
    EXPECT_EQ(r.next(row), ParseStatus::BadLevelCount);
  }
  {
    BookRowReader r("1,2,3,x\n1,2,3,4\n", 1);
    EXPECT_EQ(r.next(row), ParseStatus::BadField);
    EXPECT_EQ(r.next(row), ParseStatus::BadField);  // sticky
    EXPECT_EQ(r.line_number(), 1u);
  }
}
