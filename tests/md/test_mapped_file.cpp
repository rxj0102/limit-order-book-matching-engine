#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "lobkit/md/mapped_file.hpp"

using lobkit::md::MappedFile;

TEST(MappedFile, MapsAnExistingFileWithoutCopying) {
  MappedFile f;
  const char* err = nullptr;
  ASSERT_TRUE(f.open(LOBKIT_DATA_DIR "/itch_fixture.bin", &err)) << err;
  EXPECT_EQ(f.size(), 479u);
  EXPECT_EQ(f.bytes().size(), 479u);
  EXPECT_EQ(f.bytes()[2], 'S');  // first frame is a system event: length 12, then type S
#if defined(__unix__) || defined(__APPLE__)
  EXPECT_TRUE(f.is_mmap());
#endif
}

TEST(MappedFile, ReportsErrorsInsteadOfThrowing) {
  MappedFile f;
  const char* err = nullptr;
  EXPECT_FALSE(f.open("/nonexistent/path/file.csv", &err));
  EXPECT_NE(err, nullptr);
  err = nullptr;
  EXPECT_FALSE(f.open(LOBKIT_DATA_DIR, &err));  // a directory is not a regular file
  EXPECT_NE(err, nullptr);
  EXPECT_EQ(f.size(), 0u);
}

TEST(MappedFile, EmptyFileIsAValidEmptyMapping) {
  const std::string path = std::string(::testing::TempDir()) + "lobkit_empty_test.bin";
  { std::ofstream(path, std::ios::binary); }
  MappedFile f;
  const char* err = nullptr;
  ASSERT_TRUE(f.open(path.c_str(), &err)) << err;
  EXPECT_EQ(f.size(), 0u);
  EXPECT_TRUE(f.text().empty());
  std::remove(path.c_str());
}

TEST(MappedFile, MoveTransfersOwnership) {
  MappedFile a;
  const char* err = nullptr;
  ASSERT_TRUE(a.open(LOBKIT_DATA_DIR "/itch_fixture.bin", &err));
  MappedFile b(std::move(a));
  EXPECT_EQ(b.size(), 479u);
  EXPECT_EQ(a.size(), 0u);  // NOLINT(bugprone-use-after-move): moved-from state is specified
  MappedFile c;
  c = std::move(b);
  EXPECT_EQ(c.size(), 479u);
}
