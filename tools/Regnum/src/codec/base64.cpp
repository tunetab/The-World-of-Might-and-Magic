// Regnum — Base64.
#include "codec/base64.h"

namespace rg::codec::base64 {

namespace {

constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct DecodeTable {
  i8 v[256];
  constexpr DecodeTable() : v() {
    for (int i = 0; i < 256; i++) v[i] = -1;
    for (int i = 0; i < 64; i++) v[u8(kAlphabet[i])] = i8(i);
  }
};
constexpr DecodeTable kDecode;

inline bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\v'; }

template <class Out>
bool decodeInto(std::string_view text, Out& out) {
  out.reserve(text.size() / 4 * 3 + 3);
  u32 acc = 0;
  int n = 0, pad = 0;
  for (char c : text) {
    if (isSpace(c)) continue;
    if (c == '=') {
      pad++;
      if (pad > 2) return false;
      continue;
    }
    if (pad) return false;  // данные после «=»
    i8 v = kDecode.v[u8(c)];
    if (v < 0) return false;
    acc = (acc << 6) | u32(v);
    if (++n == 4) {
      out.push_back(typename Out::value_type(acc >> 16));
      out.push_back(typename Out::value_type(acc >> 8));
      out.push_back(typename Out::value_type(acc));
      acc = 0;
      n = 0;
    }
  }
  // Хвост: 2 символа — 1 байт, 3 символа — 2 байта; дополнение должно соответствовать.
  if (n == 1) return false;
  if (pad && (n == 0 || n + pad != 4)) return false;
  if (n == 2) out.push_back(typename Out::value_type(acc >> 4));
  if (n == 3) {
    out.push_back(typename Out::value_type(acc >> 10));
    out.push_back(typename Out::value_type(acc >> 2));
  }
  return true;
}

}  // namespace

std::string encode(std::span<const u8> data) {
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  size_t i = 0, n = data.size();
  for (; i + 3 <= n; i += 3) {
    u32 v = (u32(data[i]) << 16) | (u32(data[i + 1]) << 8) | data[i + 2];
    out.push_back(kAlphabet[v >> 18]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(kAlphabet[(v >> 6) & 63]);
    out.push_back(kAlphabet[v & 63]);
  }
  if (n - i == 1) {
    u32 v = u32(data[i]) << 16;
    out.push_back(kAlphabet[v >> 18]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out += "==";
  } else if (n - i == 2) {
    u32 v = (u32(data[i]) << 16) | (u32(data[i + 1]) << 8);
    out.push_back(kAlphabet[v >> 18]);
    out.push_back(kAlphabet[(v >> 12) & 63]);
    out.push_back(kAlphabet[(v >> 6) & 63]);
    out.push_back('=');
  }
  return out;
}

std::optional<std::vector<u8>> decode(std::string_view text) {
  std::vector<u8> out;
  if (!decodeInto(text, out)) return std::nullopt;
  return out;
}

std::optional<std::string> decodeString(std::string_view text) {
  std::string out;
  if (!decodeInto(text, out)) return std::nullopt;
  return out;
}

}  // namespace rg::codec::base64
