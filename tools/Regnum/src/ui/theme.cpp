// Regnum — тема интерфейса: токены цветов (ARCHITECTURE.md §6), роли шрифтов, тоны, сочетания клавиш.
#include "ui/ui_internal.h"

namespace rg::ui {

using namespace in;

Theme darkTheme() {
  Theme t;
  t.dark = true;
  t.bg = Color::hex(0x0e1117);
  t.surface1 = Color::hex(0x151a22);
  t.surface2 = Color::hex(0x1b2129);
  t.surface3 = Color::hex(0x242b36);
  t.border = Color::hex(0x2b3340);
  t.borderStrong = Color::hex(0x3a4454);
  t.text = Color::hex(0xece8df);
  t.textDim = Color::hex(0xa7afbb);
  t.textMuted = Color::hex(0x6f7886);
  t.accent = Color::hex(0xd9a441);
  t.accentHover = Color::hex(0xe8b75c);
  t.onAccent = Color::hex(0x1a1408);
  t.success = Color::hex(0x4fae6d);
  t.warning = Color::hex(0xe0a33a);
  t.danger = Color::hex(0xe05a4f);
  t.info = Color::hex(0x4a9fe0);
  t.shadow = Color(0, 0, 0, 115);
  t.scrim = Color(5, 7, 10, 140);
  t.hover = Color(236, 232, 223, 15);
  t.pressed = Color(236, 232, 223, 26);
  t.selection = Color(217, 164, 65, 82);
  t.stripe = Color(236, 232, 223, 6);
  t.track = Color::hex(0x2c3440);
  return t;
}

Theme lightTheme() {
  Theme t;
  t.dark = false;
  t.bg = Color::hex(0xf4f1ea);
  t.surface1 = Color::hex(0xfbf9f4);
  t.surface2 = Color::hex(0xffffff);
  t.surface3 = Color::hex(0xefeae0);
  t.border = Color::hex(0xddd5c6);
  t.borderStrong = Color::hex(0xc9bea9);
  t.text = Color::hex(0x1d1a14);
  t.textDim = Color::hex(0x5d574c);
  t.textMuted = Color::hex(0x8f877a);
  t.accent = Color::hex(0xb07d1f);
  t.accentHover = Color::hex(0xc48f2c);
  t.onAccent = Color::hex(0xffffff);
  t.success = Color::hex(0x2f8a4c);
  t.warning = Color::hex(0xb7791f);
  t.danger = Color::hex(0xc2392f);
  t.info = Color::hex(0x2f7fc0);
  t.shadow = Color(70, 52, 24, 46);
  t.scrim = Color(40, 32, 18, 77);
  t.hover = Color(29, 26, 20, 12);
  t.pressed = Color(29, 26, 20, 23);
  t.selection = Color(176, 125, 31, 64);
  t.stripe = Color(29, 26, 20, 7);
  t.track = Color::hex(0xe4ddcf);
  return t;
}

const Theme& theme() { return C().th; }

void setTheme(bool dark) {
  C().th = dark ? darkTheme() : lightTheme();
  C().redraw = true;
}

void setUiScale(float s) {
  if (!std::isfinite(s)) return;
  C().uiScale = clamp(s, 0.9f, 1.5f);
  C().redraw = true;
}

float uiScale() { return C().uiScale; }

namespace in {

float easeOut(float t) {
  t = clamp(t, 0.f, 1.f);
  float u = 1 - t;
  return 1 - u * u * u;
}

const gfx::TextStyle& styleOf(Font f) {
  using gfx::FontFamily;
  using gfx::FontWeight;
  static const gfx::TextStyle styles[] = {
      {FontFamily::UI, FontWeight::Regular, 11, 0.15f, 0},      // Caption
      {FontFamily::UI, FontWeight::Regular, 12, 0, 0},          // Small
      {FontFamily::UI, FontWeight::Regular, 13, 0, 0},          // Body
      {FontFamily::UI, FontWeight::Semibold, 13, 0, 0},         // Strong
      {FontFamily::UI, FontWeight::Semibold, 15, 0, 0},         // Subtitle
      {FontFamily::UI, FontWeight::Semibold, 18, 0, 0},         // Title
      {FontFamily::Display, FontWeight::Bold, 22, 0, 0},        // Heading
      {FontFamily::Display, FontWeight::Regular, 28, 0, 0},     // Display
      {FontFamily::Mono, FontWeight::Regular, 12, 0, 0},        // Mono
      {FontFamily::UI, FontWeight::Semibold, 20, -0.2f, 0},     // Number
  };
  size_t i = size_t(f);
  return styles[i < std::size(styles) ? i : 2];
}

gfx::TextStyle styleWith(Font f, gfx::FontWeight w) {
  gfx::TextStyle s = styleOf(f);
  s.weight = w;
  return s;
}

}  // namespace in

gfx::TextStyle textStyle(Font f) { return styleOf(f); }

float measure(std::string_view text, Font f) { return textWidth(text, styleOf(f)); }

float lineHeight(Font f) {
  static float cache[16] = {};
  size_t i = size_t(f);
  if (i >= 16) return 16;
  if (cache[i] <= 0) {
    float lh = gfx::metrics(styleOf(f)).lineHeight;
    cache[i] = lh > 0 ? lh : styleOf(f).size * 1.3f;
  }
  return cache[i];
}

Color inkColor(Ink k) {
  const Theme& t = C().th;
  switch (k) {
    case Ink::Normal: return t.text;
    case Ink::Dim: return t.textDim;
    case Ink::Muted: return t.textMuted;
    case Ink::Accent: return t.accent;
    case Ink::Success: return t.success;
    case Ink::Warning: return t.warning;
    case Ink::Danger: return t.danger;
    case Ink::Info: return t.info;
    case Ink::OnAccent: return t.onAccent;
  }
  return t.text;
}

Color toneColor(Tone tone) {
  const Theme& t = C().th;
  switch (tone) {
    case Tone::Neutral: return t.textDim;
    case Tone::Accent: return t.accent;
    case Tone::Success: return t.success;
    case Tone::Warning: return t.warning;
    case Tone::Danger: return t.danger;
    case Tone::Info: return t.info;
  }
  return t.textDim;
}

std::string shortcutText(Shortcut s) {
  if (!s) return {};
  u32 mods = s.mods & ~ModPrimary;
  if (s.mods & ModPrimary) mods |= platform::primaryMod();
  return platform::shortcutText(s.key, mods);
}

std::span<const Color> heraldicPalette() {
  // Геральдические эмали и металлы с приглушёнными вариантами.
  static const Color pal[] = {
      Color::hex(0xa4262c), Color::hex(0xc8553d), Color::hex(0xd9a441), Color::hex(0xe8d8a8), Color::hex(0x2e7d4f), Color::hex(0x6b8f3a),
      Color::hex(0x1f4e9c), Color::hex(0x4a86c8), Color::hex(0x5b3a8c), Color::hex(0x8c3a6b), Color::hex(0x7a1f2b), Color::hex(0xb46a2a),
      Color::hex(0x1c1d21), Color::hex(0x4b4f58), Color::hex(0x9aa0a8), Color::hex(0xefece4),
  };
  return pal;
}

}  // namespace rg::ui

namespace rg::ui::in {

// «ход|хода|ходов» — склонение по числу; дробные — вторая форма («2,5 хода»).
std::string unitFor(const char* unit, double v, int digits) {
  if (!unit) return {};
  std::string_view u(unit);
  size_t p1 = u.find('|');
  if (p1 == std::string_view::npos) return std::string(u);
  size_t p2 = u.find('|', p1 + 1);
  std::string one(u.substr(0, p1));
  std::string few(p2 == std::string_view::npos ? u.substr(p1 + 1) : u.substr(p1 + 1, p2 - p1 - 1));
  std::string many(p2 == std::string_view::npos ? few : std::string(u.substr(p2 + 1)));
  double p = std::pow(10.0, std::max(0, std::min(digits, 9)));
  double r = std::round(v * p) / p;
  if (std::fabs(r - std::round(r)) > 1e-9) return few;
  i64 n = i64(std::llround(r));
  return plural(n < 0 ? -n : n, one.c_str(), few.c_str(), many.c_str());
}

}  // namespace rg::ui::in
