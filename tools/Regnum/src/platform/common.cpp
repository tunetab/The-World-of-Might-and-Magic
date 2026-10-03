// Regnum — общая часть платформы: названия клавиш, щелчки, UTF-16, положение окна.
#include "platform/common.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace rg::platform {

u32 primaryMod() {
#ifdef __APPLE__
  return ModSuper;
#else
  return ModCtrl;
#endif
}

std::string keyName(Key k) {
  int v = int(k);
  if (k >= Key::A && k <= Key::Z) return std::string(1, char('A' + (v - int(Key::A))));
  if (k >= Key::D0 && k <= Key::D9) return std::string(1, char('0' + (v - int(Key::D0))));
  if (k >= Key::F1 && k <= Key::F12) return "F" + std::to_string(v - int(Key::F1) + 1);
  if (k >= Key::NumPad0 && k <= Key::NumPad9) return "Num " + std::to_string(v - int(Key::NumPad0));
#ifdef __APPLE__
  switch (k) {
    case Key::Escape: return "⎋";
    case Key::Enter: return "↩";
    case Key::Tab: return "⇥";
    case Key::Backspace: return "⌫";
    case Key::Delete: return "⌦";
    case Key::Home: return "↖";
    case Key::End: return "↘";
    case Key::PageUp: return "⇞";
    case Key::PageDown: return "⇟";
    case Key::NumEnter: return "⌤";
    case Key::Shift: return "⇧";
    case Key::Ctrl: return "⌃";
    case Key::Alt: return "⌥";
    case Key::Super: return "⌘";
    case Key::CapsLock: return "⇪";
    default: break;
  }
#endif
  switch (k) {
    case Key::Escape: return "Esc";
    case Key::Enter: return "Enter";
    case Key::Tab: return "Tab";
    case Key::Backspace: return "Backspace";
    case Key::Delete: return "Del";
    case Key::Insert: return "Ins";
    case Key::Home: return "Home";
    case Key::End: return "End";
    case Key::PageUp: return "PgUp";
    case Key::PageDown: return "PgDn";
    case Key::Left: return "←";
    case Key::Right: return "→";
    case Key::Up: return "↑";
    case Key::Down: return "↓";
    case Key::Space: return "Пробел";
    case Key::Minus: return "-";
    case Key::Equal: return "=";
    case Key::LBracket: return "[";
    case Key::RBracket: return "]";
    case Key::Semicolon: return ";";
    case Key::Quote: return "'";
    case Key::Comma: return ",";
    case Key::Period: return ".";
    case Key::Slash: return "/";
    case Key::Backslash: return "\\";
    case Key::Grave: return "`";
    case Key::NumAdd: return "Num +";
    case Key::NumSub: return "Num -";
    case Key::NumMul: return "Num *";
    case Key::NumDiv: return "Num /";
    case Key::NumDecimal: return "Num .";
    case Key::NumEnter: return "Num Enter";
    case Key::Shift: return "Shift";
    case Key::Ctrl: return "Ctrl";
    case Key::Alt: return "Alt";
#ifdef _WIN32
    case Key::Super: return "Win";
#else
    case Key::Super: return "Super";
#endif
    case Key::CapsLock: return "Caps Lock";
    case Key::Menu: return "Menu";
    default: return {};
  }
}

std::string shortcutText(Key k, u32 mods) {
  std::string s;
#ifdef __APPLE__
  // Порядок Apple: ⌃ ⌥ ⇧ ⌘, без разделителей.
  if (mods & ModCtrl) s += "⌃";
  if (mods & ModAlt) s += "⌥";
  if (mods & ModShift) s += "⇧";
  if (mods & ModSuper) s += "⌘";
  if (k != Key::Unknown) s += keyName(k);
#else
  auto add = [&s](const std::string& part) {
    if (!s.empty()) s += "+";
    s += part;
  };
  if ((mods & ModCtrl) && k != Key::Ctrl) add("Ctrl");
  if ((mods & ModShift) && k != Key::Shift) add("Shift");
  if ((mods & ModAlt) && k != Key::Alt) add("Alt");
  if ((mods & ModSuper) && k != Key::Super) add(keyName(Key::Super));
  if (k != Key::Unknown) add(keyName(k));
#endif
  return s;
}

namespace detail {

double monotonicSeconds() {
  using namespace std::chrono;
  static const auto t0 = steady_clock::now();
  return duration<double>(steady_clock::now() - t0).count();
}

int ClickCounter::press(int button, float x, float y, double t, double interval, float slop) {
  bool chain = count_ > 0 && button == button_ && t - t_ <= interval && t >= t_ && std::fabs(x - x_) <= slop &&
               std::fabs(y - y_) <= slop;
  count_ = chain ? count_ + 1 : 1;
  button_ = button;
  x_ = x;
  y_ = y;
  t_ = t;
  return count_;
}

bool isTextCodepoint(u32 cp) {
  if (cp < 0x20 || cp == 0x7F) return false;
  if (cp >= 0x80 && cp < 0xA0) return false;
  if (cp >= 0xD800 && cp <= 0xDFFF) return false;
  return cp <= 0x10FFFF;
}

std::string Utf16Input::push(u16 unit) {
  u32 cp = unit;
  if (unit >= 0xD800 && unit <= 0xDBFF) {
    high_ = unit;
    return {};
  }
  if (unit >= 0xDC00 && unit <= 0xDFFF) {
    if (!high_) return {};
    cp = 0x10000 + ((u32(high_) - 0xD800) << 10) + (u32(unit) - 0xDC00);
  }
  high_ = 0;
  if (!isTextCodepoint(cp)) return {};
  return utf8::encode(cp);
}

std::string filterText(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    u32 cp = utf8::decode(s, i);
    if (isTextCodepoint(cp)) utf8::append(out, cp);
  }
  return out;
}

std::string toCrlf(std::string_view s) {
  std::string out;
  out.reserve(s.size() + s.size() / 16);
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '\r') {
      out += "\r\n";
      if (i + 1 < s.size() && s[i + 1] == '\n') i++;
    } else if (c == '\n') {
      out += "\r\n";
    } else {
      out += c;
    }
  }
  return out;
}

std::string fromCrlf(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '\r') {
      out += '\n';
      if (i + 1 < s.size() && s[i + 1] == '\n') i++;
    } else {
      out += c;
    }
  }
  return out;
}

bool isSafeUrl(std::string_view url) {
  std::string l = utf8::lower(url.substr(0, std::min<size_t>(url.size(), 16)));
  size_t rest = startsWith(l, "http://") ? 7 : startsWith(l, "https://") ? 8 : startsWith(l, "mailto:") ? 7 : 0;
  if (rest == 0 || url.size() <= rest) return false;  // схема не из списка или пустой адрес
  for (char c : url)
    if (u8(c) < 0x20 || c == 0x7F) return false;  // управляющие символы недопустимы
  return url[rest] != '/' && url[rest] != ' ';
}

static std::filesystem::path fsPath(const std::string& s) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::optional<Placement> loadPlacement(const std::string& path) {
  if (path.empty()) return std::nullopt;
  std::ifstream in(fsPath(path), std::ios::binary);
  if (!in) return std::nullopt;
  Placement p;
  bool hasRect[4] = {};
  std::string line;
  while (std::getline(in, line)) {
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    long long v = 0;
    std::istringstream vs(val);
    if (!(vs >> v)) continue;
    if (v < -100000 || v > 100000) return std::nullopt;
    int iv = int(v);
    if (key == "x") { p.x = iv; hasRect[0] = true; }
    else if (key == "y") { p.y = iv; hasRect[1] = true; }
    else if (key == "w") { p.w = iv; hasRect[2] = true; }
    else if (key == "h") { p.h = iv; hasRect[3] = true; }
    else if (key == "maximized") p.maximized = iv != 0;
    else if (key == "dpi") p.dpi = iv;
  }
  for (bool b : hasRect)
    if (!b) return std::nullopt;
  if (p.w < 100 || p.h < 60 || p.dpi < 48 || p.dpi > 960) return std::nullopt;
  return p;
}

bool savePlacement(const std::string& path, const Placement& p) {
  if (path.empty()) return false;
  std::error_code ec;
  std::filesystem::path target = fsPath(path);
  if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
  std::filesystem::path tmp = target;
  tmp += ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "# Regnum: положение главного окна\n"
        << "x=" << p.x << "\ny=" << p.y << "\nw=" << p.w << "\nh=" << p.h << "\nmaximized=" << (p.maximized ? 1 : 0)
        << "\ndpi=" << p.dpi << "\n";
    if (!out.flush()) return false;
  }
  std::filesystem::rename(tmp, target, ec);
  if (ec) {
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

std::string placementPath(const WindowConfig& cfg, const std::string& dataDir) {
  if (cfg.placementFile.empty()) return {};
  if (fsPath(cfg.placementFile).is_absolute()) return cfg.placementFile;
  if (dataDir.empty()) return {};
  std::string d = dataDir;
  if (d.back() != '/' && d.back() != '\\') d += '/';
  return d + cfg.placementFile;
}

}  // namespace detail
}  // namespace rg::platform
