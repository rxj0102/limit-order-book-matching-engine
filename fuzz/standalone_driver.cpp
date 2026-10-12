// Runs LLVMFuzzerTestOneInput over files and directories given on the command
// line. Lets GCC builds and CI replay the committed corpus and any saved crash
// reproducers as ordinary regression tests, without libFuzzer.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size);

int main(int argc, char** argv) {
  namespace fs = std::filesystem;
  std::size_t files = 0;
  for (int i = 1; i < argc; ++i) {
    std::vector<fs::path> paths;
    if (fs::is_directory(argv[i])) {
      for (const auto& e : fs::directory_iterator(argv[i]))
        if (e.is_regular_file()) paths.push_back(e.path());
    } else {
      paths.emplace_back(argv[i]);
    }
    for (const auto& p : paths) {
      std::ifstream f(p, std::ios::binary);
      const std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                                    std::istreambuf_iterator<char>());
      LLVMFuzzerTestOneInput(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
      ++files;
    }
  }
  std::printf("replayed %zu inputs\n", files);
  return files == 0 ? 1 : 0;
}
