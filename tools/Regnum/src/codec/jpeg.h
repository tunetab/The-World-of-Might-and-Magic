// Regnum — JPEG: baseline и progressive (Хаффман), 1/3/4 компоненты, любая целая субдискретизация
// (4:4:4, 4:2:2, 4:2:0, 4:4:0, 4:1:1), рестарт-маркеры, JFIF/Adobe (YCbCr, RGB, CMYK, YCCK), ориентация EXIF.
// Арифметическое кодирование, lossless, иерархический режим и 12 бит не поддерживаются — понятная ошибка.
// Обрезанный файл декодируется до места обрыва (остаток — серый), как в libjpeg.
#pragma once
#include "codec/png.h"

namespace rg::codec {

struct JpegInfo {
  int w = 0, h = 0;          // размер в файле (до поворота EXIF)
  int components = 0;
  bool progressive = false;
  int orientation = 1;       // EXIF 1..8
};
std::optional<JpegInfo> jpegInfo(const u8* data, size_t size, std::string* error = nullptr);

struct JpegDecodeOptions {
  bool applyOrientation = true;
  i64 maxPixels = i64(1) << 28;
};
std::optional<RgbaImage> decodeJpeg(const u8* data, size_t size, const JpegDecodeOptions& opt, std::string* error = nullptr);
inline std::optional<RgbaImage> decodeJpeg(const u8* data, size_t size, std::string* error = nullptr) {
  return decodeJpeg(data, size, JpegDecodeOptions{}, error);
}
inline std::optional<RgbaImage> decodeJpeg(const std::string& bytes, std::string* error = nullptr) {
  return decodeJpeg(reinterpret_cast<const u8*>(bytes.data()), bytes.size(), JpegDecodeOptions{}, error);
}

// Формат по сигнатуре: PNG или JPEG. Иначе — ошибка «неизвестный формат».
std::optional<RgbaImage> decodeImage(const u8* data, size_t size, std::string* error = nullptr);
inline std::optional<RgbaImage> decodeImage(const std::string& bytes, std::string* error = nullptr) {
  return decodeImage(reinterpret_cast<const u8*>(bytes.data()), bytes.size(), error);
}
// Прочитать и декодировать файл изображения (UTF-8 путь).
std::optional<RgbaImage> readImageFile(const std::string& path, std::string* error = nullptr);

}  // namespace rg::codec
