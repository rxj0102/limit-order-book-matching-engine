#include "lobkit/md/mapped_file.hpp"

#include <cstdio>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define LOBKIT_HAVE_MMAP 1
#endif

namespace lobkit::md {

MappedFile::~MappedFile() {
  close();
}

MappedFile::MappedFile(MappedFile&& o) noexcept {
  *this = std::move(o);
}

MappedFile& MappedFile::operator=(MappedFile&& o) noexcept {
  if (this != &o) {
    close();
    fallback_ = std::move(o.fallback_);
    mapped_ = o.mapped_;
    size_ = o.size_;
    data_ = mapped_ ? o.data_ : fallback_.data();
    o.data_ = nullptr;
    o.size_ = 0;
    o.mapped_ = false;
  }
  return *this;
}

void MappedFile::close() noexcept {
#ifdef LOBKIT_HAVE_MMAP
  if (mapped_ && data_) munmap(const_cast<std::uint8_t*>(data_), size_);
#endif
  data_ = nullptr;
  size_ = 0;
  mapped_ = false;
  fallback_.clear();
}

bool MappedFile::open(const char* path, const char** error) noexcept {
  close();
#ifdef LOBKIT_HAVE_MMAP
  const int fd = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    *error = "cannot open file";
    return false;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    ::close(fd);
    *error = "not a regular file";
    return false;
  }
  if (st.st_size == 0) {
    ::close(fd);
    return true;
  }
  void* p = mmap(nullptr, static_cast<std::size_t>(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);  // the mapping stays valid after the descriptor is closed
  if (p == MAP_FAILED) {
    *error = "mmap failed";
    return false;
  }
  madvise(p, static_cast<std::size_t>(st.st_size), MADV_SEQUENTIAL);
  data_ = static_cast<const std::uint8_t*>(p);
  size_ = static_cast<std::size_t>(st.st_size);
  mapped_ = true;
  return true;
#else
  std::FILE* f = std::fopen(path, "rb");
  if (!f) {
    *error = "cannot open file";
    return false;
  }
  try {
    std::uint8_t buf[1 << 16];
    for (std::size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;)
      fallback_.insert(fallback_.end(), buf, buf + n);
  } catch (...) {
    std::fclose(f);
    *error = "out of memory";
    return false;
  }
  std::fclose(f);
  data_ = fallback_.data();
  size_ = fallback_.size();
  return true;
#endif
}

}  // namespace lobkit::md
