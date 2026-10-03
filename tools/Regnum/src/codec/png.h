// Regnum — PNG: чтение всех стандартных вариантов и запись RGBA8.
#pragma once
#include "base/base.h"

namespace rg::codec {

// Обычный (не premultiplied) RGBA8, строки сверху вниз.
struct RgbaImage {
  int w = 0, h = 0;
  std::vector<u8> rgba;
  bool empty() const { return w <= 0 || h <= 0; }
};

// Декодирование: типы цвета 0/2/3/4/6, глубина 1–16 бит, палитра и tRNS, чересстрочный Adam7.
// 16 бит приводятся к 8 с округлением. Проверяются CRC блоков и Adler-32 данных.
std::optional<RgbaImage> decodePng(const u8* data, size_t size, std::string* error = nullptr);
inline std::optional<RgbaImage> decodePng(const std::string& bytes, std::string* error = nullptr) {
  return decodePng(reinterpret_cast<const u8*>(bytes.data()), bytes.size(), error);
}
// Кодирование RGBA8 (level 0 — без сжатия, 1–9 — deflate).
std::vector<u8> encodePng(const RgbaImage& img, int level = 6);
// Сохранить в файл атомарно (UTF-8 путь). false при ошибке записи.
bool writePngFile(const std::string& path, const RgbaImage& img, int level = 6);

// ---------------------------------------------------------------- дополнительно
struct PngInfo {
  int w = 0, h = 0;
  int bitDepth = 0;   // 1, 2, 4, 8, 16
  int colorType = 0;  // 0 серый, 2 RGB, 3 палитра, 4 серый+альфа, 6 RGBA
  bool interlaced = false;
};
// Заголовок без распаковки (размер для планирования памяти).
std::optional<PngInfo> pngInfo(const u8* data, size_t size, std::string* error = nullptr);

struct PngDecodeOptions {
  i64 maxPixels = i64(1) << 28;  // защита от огромных размеров
};
std::optional<RgbaImage> decodePng(const u8* data, size_t size, const PngDecodeOptions& opt, std::string* error = nullptr);

struct PngEncodeOptions {
  int level = 6;
  bool reduce = true;  // без потерь уменьшить формат: RGB без альфы, серый, палитра (≤ 256 цветов)
};
std::vector<u8> encodePng(const RgbaImage& img, const PngEncodeOptions& opt);

}  // namespace rg::codec
