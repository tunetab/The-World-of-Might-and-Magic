// Regnum — сжатие DEFLATE (RFC 1951) в обёртках zlib (RFC 1950) и gzip (RFC 1952), CRC-32, Adler-32.
//
//   auto packed = codec::deflate(bytes, 6, codec::ZFormat::Gzip);
//   std::string err;
//   auto plain = codec::inflate(packed, codec::ZFormat::Gzip, &err);   // nullopt + сообщение при ошибке
//
// Распаковка устойчива к повреждённым данным: проверка границ, контрольных сумм и предела размера.
#pragma once
#include <span>

#include "base/base.h"

namespace rg::codec {

enum class ZFormat : u8 { Raw, Zlib, Gzip };

u32 crc32(const void* data, size_t n, u32 crc = 0);      // продолжение: crc32(b, nb, crc32(a, na))
u32 adler32(const void* data, size_t n, u32 adler = 1);
inline u32 crc32(std::string_view s, u32 crc = 0) { return crc32(s.data(), s.size(), crc); }
inline u32 adler32(std::string_view s, u32 adler = 1) { return adler32(s.data(), s.size(), adler); }

struct InflateOptions {
  size_t maxOutput = size_t(1) << 31;  // предел распакованного размера (защита от «бомб»)
  size_t sizeHint = 0;                 // ожидаемый размер (0 — неизвестен): память выделяется сразу
  bool stopAtMax = false;              // по достижении maxOutput прекратить без ошибки (лишнее отбрасывается)
  bool verifyChecksum = true;
};

std::optional<std::vector<u8>> inflate(std::span<const u8> data, ZFormat fmt = ZFormat::Zlib, std::string* error = nullptr,
                                       const InflateOptions& opt = {});
inline std::optional<std::vector<u8>> inflate(std::string_view data, ZFormat fmt = ZFormat::Zlib, std::string* error = nullptr,
                                              const InflateOptions& opt = {}) {
  return inflate(std::span<const u8>(reinterpret_cast<const u8*>(data.data()), data.size()), fmt, error, opt);
}
// Распаковка в готовый буфер известного размера (out.size() — ожидаемый объём).
// Возвращает число записанных байт или nullopt; данных больше буфера — ошибка (или остановка при stopAtMax).
std::optional<size_t> inflateInto(std::span<const u8> data, ZFormat fmt, std::span<u8> out, std::string* error = nullptr,
                                  bool stopAtMax = false, bool verifyChecksum = true);

// Уровень 0 — без сжатия, 1 — быстрее всего, 9 — сильнее всего (6 — по умолчанию).
std::vector<u8> deflate(std::span<const u8> data, int level = 6, ZFormat fmt = ZFormat::Zlib);
inline std::vector<u8> deflate(std::string_view data, int level = 6, ZFormat fmt = ZFormat::Zlib) {
  return deflate(std::span<const u8>(reinterpret_cast<const u8*>(data.data()), data.size()), level, fmt);
}

}  // namespace rg::codec
