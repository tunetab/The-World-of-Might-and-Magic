// Помощники тестов кодеков: путь к эталонным данным src/tests/data, сравнение изображений.
#pragma once
#include <cstdlib>
#include <filesystem>
#include <string>

#include "base/base.h"

namespace rg::test {

// Путь к файлу эталонных данных (tests/data/<rel>): от корня репозитория или рядом с этим заголовком.
inline std::string dataPath(const std::string& rel) {
  static const std::string base = [] {
    std::error_code ec;
    const char* fromRepo = "tools/Regnum/src/tests/data";
    if (std::filesystem::is_directory(std::filesystem::path(fromRepo), ec)) return std::string(fromRepo);
    std::string here = __FILE__;
    size_t p = here.find_last_of("/\\");
    return (p == std::string::npos ? std::string(".") : here.substr(0, p)) + "/data";
  }();
  return base + "/" + rel;
}

// Средняя абсолютная ошибка по каналам (n каналов на пиксель, сравниваются первые k каналов).
inline double meanAbsError(const u8* a, const u8* b, size_t pixels, int strideA, int strideB, int k) {
  if (!pixels) return 0;
  u64 sum = 0;
  for (size_t i = 0; i < pixels; i++)
    for (int c = 0; c < k; c++) sum += u64(std::abs(int(a[i * size_t(strideA) + size_t(c)]) - int(b[i * size_t(strideB) + size_t(c)])));
  return double(sum) / double(pixels * size_t(k));
}

}  // namespace rg::test
