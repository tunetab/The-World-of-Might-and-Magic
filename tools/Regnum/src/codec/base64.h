// Regnum — Base64 (RFC 4648, стандартный алфавит, дополнение «=»).
#pragma once
#include <span>

#include "base/base.h"

namespace rg::codec::base64 {

std::string encode(std::span<const u8> data);
inline std::string encode(std::string_view data) {
  return encode(std::span<const u8>(reinterpret_cast<const u8*>(data.data()), data.size()));
}
// Пробельные символы пропускаются; дополнение «=» необязательно. Ошибка — nullopt.
std::optional<std::vector<u8>> decode(std::string_view text);
std::optional<std::string> decodeString(std::string_view text);

}  // namespace rg::codec::base64
