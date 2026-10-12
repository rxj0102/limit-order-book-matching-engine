#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace lobkit::md {

// Read-only view of a whole file. On POSIX it is an mmap, so parsing reads the
// page cache directly and copies nothing. Elsewhere it falls back to reading the
// file into memory (one allocation at open time, never during parsing).
class MappedFile {
 public:
  MappedFile() = default;
  ~MappedFile();
  MappedFile(MappedFile&& o) noexcept;
  MappedFile& operator=(MappedFile&& o) noexcept;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  // Returns false and fills error (a static string) on failure. An empty file is
  // a valid, empty mapping.
  bool open(const char* path, const char** error) noexcept;
  void close() noexcept;

  std::span<const std::uint8_t> bytes() const noexcept { return {data_, size_}; }
  std::string_view text() const noexcept { return {reinterpret_cast<const char*>(data_), size_}; }
  std::size_t size() const noexcept { return size_; }
  bool is_mmap() const noexcept { return mapped_; }

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  bool mapped_ = false;
  std::vector<std::uint8_t> fallback_;  // used only when mmap is unavailable
};

}  // namespace lobkit::md
