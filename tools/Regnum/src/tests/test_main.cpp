// Regnum — точка входа тестов.
#include <cstdio>
#include <filesystem>

#include "tests/test.h"

namespace rg::test {

std::vector<Case>& registry() { static std::vector<Case> r; return r; }

void fail(const char* file, int line, const std::string& msg) {
  std::string f = file;
  size_t p = f.find_last_of("/\\");
  if (p != std::string::npos) f = f.substr(p + 1);
  throw Failure{f + ":" + std::to_string(line) + ": " + msg};
}

static std::string gOut;
std::string outDir() {
  if (gOut.empty()) {
    const char* env = std::getenv("REGNUM_TEST_OUT");
    gOut = env && *env ? env : ".wmma/regnum-tests";
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(reinterpret_cast<const char8_t*>(gOut.c_str())), ec);
  return gOut;
}

}  // namespace rg::test

int main(int argc, char** argv) {
  using namespace rg::test;
  std::vector<std::string> filters;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("--out=", 0) == 0) { rg::test::gOut = a.substr(6); continue; }
    filters.push_back(a);
  }
  int pass = 0, failn = 0;
  double t0 = rg::nowSeconds();
  for (auto& c : registry()) {
    std::string n = c.name;
    if (!filters.empty()) {
      bool any = false;
      for (auto& f : filters) if (n.find(f) != std::string::npos) any = true;
      if (!any) continue;
    }
    double s = rg::nowSeconds();
    try {
      c.fn();
      pass++;
      double ms = (rg::nowSeconds() - s) * 1000;
      if (ms > 300) std::printf("  ok   %s (%.0f ms)\n", c.name, ms);
    } catch (const Failure& f) {
      failn++;
      std::printf("  FAIL %s\n       %s\n", c.name, f.msg.c_str());
    } catch (const std::exception& e) {
      failn++;
      std::printf("  FAIL %s\n       exception: %s\n", c.name, e.what());
    } catch (...) {
      failn++;
      std::printf("  FAIL %s\n       unknown exception\n", c.name);
    }
    std::fflush(stdout);
  }
  std::printf("%s tests: %d, passed: %d, failed: %d (%.2f s)\n", failn ? "FAILED" : "OK", pass + failn, pass, failn, rg::nowSeconds() - t0);
  return failn ? 1 : 0;
}
