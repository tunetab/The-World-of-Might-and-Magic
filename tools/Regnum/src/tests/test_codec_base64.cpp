// Тесты Base64: векторы RFC 4648, пробелы, ошибки, круговое преобразование.
#include "codec/base64.h"
#include "tests/test.h"

using namespace rg;
using namespace rg::codec;

TEST(codec_base64_rfc4648) {
  const std::pair<const char*, const char*> v[] = {{"", ""},         {"f", "Zg=="},         {"fo", "Zm8="},        {"foo", "Zm9v"},
                                                   {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
  for (auto& [plain, enc] : v) {
    CHECK_EQ(base64::encode(std::string_view(plain)), std::string(enc));
    auto d = base64::decodeString(enc);
    CHECK(d && *d == plain);
  }
  CHECK_EQ(base64::encode(std::string_view("Арден")), std::string("0JDRgNC00LXQvQ=="));
}

TEST(codec_base64_whitespace_and_errors) {
  auto d = base64::decodeString(" Zm9v\r\nYmFy \t");
  CHECK(d && *d == "foobar");
  auto np = base64::decodeString("Zm9vYg");  // без дополнения
  CHECK(np && *np == "foob");
  CHECK(!base64::decode("Zm9v!"));
  CHECK(!base64::decode("Z"));
  CHECK(!base64::decode("Zm9vY"));
  CHECK(!base64::decode("Zg==Zg=="));
  CHECK(!base64::decode("Zg==="));
  CHECK(!base64::decode("=Zg"));
  CHECK(!base64::decode("Zm-_"));
  CHECK(base64::decode("")->empty());
  // все значения байтов
  std::vector<u8> all;
  for (int i = 0; i < 256; i++) all.push_back(u8(i));
  for (size_t n = 0; n <= all.size(); n += 17) {
    std::span<const u8> s(all.data(), n);
    std::string e = base64::encode(s);
    CHECK_EQ(e.size(), (n + 2) / 3 * 4);
    auto back = base64::decode(e);
    CHECK(back && *back == std::vector<u8>(all.begin(), all.begin() + long(n)));
  }
}
