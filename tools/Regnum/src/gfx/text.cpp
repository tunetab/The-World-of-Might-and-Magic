// Regnum — текст: реестр шрифтов, разбивка на глифы, раскладка строк, отрисовка, каретка.
#include "gfx/text.h"

#include <atomic>
#include <deque>
#include <mutex>

namespace rg::gfx {

namespace {

// ---------------------------------------------------------------- реестр шрифтов
// Ячейка шрифта: основные загружены при поиске, запасные — при первой нужде.
struct Slot {
  FontFace face;
  std::once_flag once;
  std::atomic<bool> done{false};
  std::shared_ptr<const Font> font;

  const Font* get() {
    if (done.load(std::memory_order_acquire)) return font.get();
    std::call_once(once, [&] {
      if (face.font) {
        font = face.font;
      } else {
        std::string err;
        font = Font::loadFile(face.path, face.index, &err);
        if (font) logInfo("Запасной шрифт: %s", font->fullName().c_str());
        else logWarn("Запасной шрифт %s не загружен: %s", face.path.c_str(), err.c_str());
      }
      done.store(true, std::memory_order_release);
    });
    return font.get();
  }
};

struct ChainEntry {
  Slot* slot;
  Synth synth;
};
struct Chain {
  std::vector<ChainEntry> e;  // [0] — основной шрифт стиля
};

struct Registry {
  bool ok = false;
  std::string error;
  SystemFonts sys;
  std::deque<Slot> slots;
  Chain chains[3][3];
};

Registry& reg() {
  static Registry r;
  return r;
}
std::once_flag gInitOnce;

Slot* slotFor(Registry& r, const FontFace& f) {
  for (Slot& s : r.slots)
    if (s.face.path == f.path && s.face.index == f.index) {
      if (!s.face.font && f.font) s.face.font = f.font;
      return &s;
    }
  Slot& s = r.slots.emplace_back();
  s.face = f;
  s.face.synth = Synth::None;
  return &s;
}

void initRegistry() {
  Registry& r = reg();
  if (!findSystemFonts(r.sys, &r.error)) {
    logError("Шрифты: %s", r.error.c_str());
    return;
  }
  for (int fam = 0; fam < 3; fam++) {
    for (int w = 0; w < 3; w++) {
      Chain& ch = r.chains[fam][w];
      const FontFace& prim = r.sys.faces[fam][w];
      ch.e.push_back({slotFor(r, prim), prim.synth});
      // Для засечек и моноширинного — запасной основной интерфейсный шрифт того же начертания.
      if (fam != 0) {
        const FontFace& ui = r.sys.faces[0][w];
        Slot* s = slotFor(r, ui);
        if (s != ch.e[0].slot) ch.e.push_back({s, ui.synth});
      }
      Synth fs = w == 0 ? Synth::None : (w == 1 ? Synth::Semibold : Synth::Bold);
      for (const FontFace& fb : r.sys.fallbacks) {
        if (ch.e.size() >= 250) break;
        Slot* s = slotFor(r, fb);
        bool dup = false;
        for (auto& e : ch.e) dup |= e.slot == s;
        if (!dup) ch.e.push_back({s, fs});
      }
    }
  }
  // Основные шрифты уже загружены — отметить ячейки готовыми.
  for (Slot& s : r.slots) if (s.face.font) s.get();
  r.ok = true;
  for (int fam = 0; fam < 3; fam++) {
    const Font* f = r.chains[fam][0].e[0].slot->get();
    logInfo("Шрифт %s: %s", fam == 0 ? "интерфейса" : fam == 1 ? "заголовков" : "моноширинный", f ? f->fullName().c_str() : "?");
  }
}

bool ensureFonts() {
  std::call_once(gInitOnce, initRegistry);
  return reg().ok;
}

const Chain& chainOf(const TextStyle& s) {
  int fam = std::clamp(int(s.family), 0, 2), w = std::clamp(int(s.weight), 0, 2);
  return reg().chains[fam][w];
}

float quantSize(float s) {
  if (!(s > 0) || !std::isfinite(s)) s = 13;
  return std::clamp(std::round(s * 4) / 4, 0.25f, 4096.f);
}

// ---------------------------------------------------------------- разбивка на глифы
enum class IK : u8 { Normal, Space, NoBreak, Tab, Invisible, SoftHyphen, Newline };

struct SG {
  float adv = 0;
  u32 byte = 0;
  u16 glyph = 0;
  u8 face = 0;
  u8 len = 1;      // длина кодовой точки в байтах (CRLF — 2)
  IK kind = IK::Normal;
  bool brk = false;  // после символа допустим перенос
};

bool isLetter(u32 c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7) ||
         (c >= 0x370 && c <= 0x3FF) || (c >= 0x400 && c <= 0x52F);
}

bool isInvisible(u32 c) {
  return c < 0x20 || (c >= 0x7F && c <= 0x9F) || (c >= 0x200B && c <= 0x200F) || (c >= 0x202A && c <= 0x202E) ||
         (c >= 0x2060 && c <= 0x206F) || c == 0xFEFF || (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0xE0000 && c <= 0xE0FFF);
}

// Ширина пробела, если в шрифтах нет глифа (доли кегля; < 0 — особые случаи).
float fallbackSpace(u32 c, float size, float space, const Font& f) {
  float k = size / float(f.unitsPerEm());
  switch (c) {
    case 0x00A0: return space;
    case 0x2000: case 0x2002: return size * 0.5f;
    case 0x2001: case 0x2003: case 0x3000: return size;
    case 0x2004: return size / 3;
    case 0x2005: return size / 4;
    case 0x2006: return size / 6;
    case 0x2007: { u16 g = f.glyphIndex('0'); return g ? float(f.advance(g)) * k : size * 0.55f; }
    case 0x2008: { u16 g = f.glyphIndex('.'); return g ? float(f.advance(g)) * k : size * 0.25f; }
    case 0x2009: case 0x202F: return std::max(size * 0.2f, space * 0.6f);
    case 0x200A: return size * 0.1f;
    default: return space;
  }
}

struct Ctx {
  const Chain* chain = nullptr;
  const Font* f0 = nullptr;
  float size = 13, k0 = 0, ls = 0;
  float space = 0;  // ширина обычного пробела основного шрифта

  explicit Ctx(const TextStyle& st) {
    chain = &chainOf(st);
    f0 = chain->e[0].slot->get();
    size = quantSize(st.size);
    ls = std::isfinite(st.letterSpacing) ? st.letterSpacing : 0;
    k0 = size / float(f0->unitsPerEm());
    u16 sg = f0->glyphIndex(' ');
    space = sg ? float(f0->advance(sg)) * k0 : size * 0.25f;
  }

  const Font* font(u8 face) const { return chain->e[face].slot->get(); }
  Synth synth(u8 face) const { return chain->e[face].synth; }

  bool map(u32 cp, u8& face, u16& glyph) const {
    if (u16 g = f0->glyphIndex(cp)) { face = 0; glyph = g; return true; }
    for (size_t k = 1; k < chain->e.size(); k++) {
      const Font* f = chain->e[k].slot->get();
      if (!f) continue;
      if (u16 g = f->glyphIndex(cp)) { face = u8(k); glyph = g; return true; }
    }
    return false;
  }

  float advanceOf(u8 face, u16 glyph) const {
    const Font* f = font(face);
    return float(f->advance(glyph)) * size / float(f->unitsPerEm()) + synthAdvance(*f, synth(face), size);
  }
};

void shape(std::string_view s, const Ctx& cx, std::vector<SG>& out) {
  out.clear();
  out.reserve(s.size());
  u32 prevCp = 0;
  size_t i = 0;
  while (i < s.size()) {
    size_t b = i;
    u32 cp = utf8::decode(s, i);
    SG g;
    g.byte = u32(b);
    g.len = u8(i - b);
    if (cp == '\n' || cp == 0x2028 || cp == 0x2029 || cp == 0x0B || cp == 0x0C || cp == 0x85) {
      g.kind = IK::Newline;
    } else if (cp == '\r') {
      g.kind = IK::Newline;
      if (i < s.size() && s[i] == '\n') { i++; g.len = 2; }
    } else if (cp == '\t') {
      g.kind = IK::Tab;
      g.brk = true;
    } else if (cp == 0xAD) {
      g.kind = IK::SoftHyphen;
    } else if (cp == 0x200B) {
      g.kind = IK::Invisible;
      g.brk = true;
    } else if (isInvisible(cp)) {
      g.kind = IK::Invisible;
    } else {
      bool space = cp == ' ' || cp == 0x3000 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A && cp != 0x2007);
      bool nbsp = cp == 0xA0 || cp == 0x2007 || cp == 0x202F;
      g.kind = space ? IK::Space : nbsp ? IK::NoBreak : IK::Normal;
      g.brk = space;
      if (cx.map(cp, g.face, g.glyph)) {
        g.adv = cx.advanceOf(g.face, g.glyph);
        if (space || nbsp) g.adv -= synthAdvance(*cx.font(g.face), cx.synth(g.face), cx.size);
      } else if (space || nbsp) {
        g.adv = fallbackSpace(cp, cx.size, cx.space, *cx.f0);
      } else {
        g.face = 0;
        g.glyph = 0;  // .notdef основного шрифта
        g.adv = cx.advanceOf(0, 0);
      }
      g.adv += cx.ls;
      // Перенос после дефиса внутри слова и после тире.
      if ((cp == '-' || cp == 0x2010) && isLetter(prevCp)) g.brk = true;
      if ((cp == 0x2013 || cp == 0x2014) && prevCp != ' ' && prevCp != 0xA0 && prevCp != 0) g.brk = true;
    }
    out.push_back(g);
    prevCp = cp;
  }
  // Кернинг соседних глифов одного шрифта.
  for (size_t j = 0; j + 1 < out.size(); j++) {
    SG& a = out[j];
    const SG& c = out[j + 1];
    if (a.face != c.face || (a.kind != IK::Normal && a.kind != IK::Space && a.kind != IK::NoBreak) ||
        (c.kind != IK::Normal && c.kind != IK::Space && c.kind != IK::NoBreak) || !a.glyph || !c.glyph)
      continue;
    const Font* f = cx.font(a.face);
    if (!f->hasKerning()) continue;
    if (int kv = f->kerning(a.glyph, c.glyph)) a.adv += float(kv) * cx.size / float(f->unitsPerEm());
  }
}

// ---------------------------------------------------------------- раскладка строк
struct Special {
  u8 face = 0;
  u16 glyph = 0;
  float adv = 0;
  int count = 1;
};

Special specialGlyph(const Ctx& cx, u32 cp, u32 alt, int altCount) {
  Special s;
  if (cx.map(cp, s.face, s.glyph)) { s.adv = cx.advanceOf(s.face, s.glyph); return s; }
  if (cx.map(alt, s.face, s.glyph)) { s.adv = cx.advanceOf(s.face, s.glyph); s.count = altCount; return s; }
  s.adv = 0;
  s.count = 0;
  return s;
}

GlyphKind publicKind(IK k) {
  switch (k) {
    case IK::Normal: return GlyphKind::Normal;
    case IK::Space: return GlyphKind::Space;
    case IK::NoBreak: return GlyphKind::NoBreak;
    case IK::Tab: return GlyphKind::Tab;
    default: return GlyphKind::Invisible;
  }
}

struct Builder {
  TextLayout& L;
  const std::vector<SG>& sg;
  const Ctx& cx;
  const LayoutOptions& opt;
  std::string_view text;
  float tabW = 0;
  bool wrapping = false;
  float maxW = 0;
  Special ell, hyph;
  bool haveEll = false, haveHyph = false;

  float tabAdvance(float x) const {
    float next = (std::floor(x / tabW + 1e-4f) + 1) * tabW;
    return next - x;
  }

  void push(const SG& g, float x, float adv) {
    TextGlyph t;
    t.x = x;
    t.advance = adv;
    t.byte = g.byte;
    t.glyph = g.glyph;
    t.face = g.face;
    t.kind = publicKind(g.kind);
    L.glyphs.push_back(t);
  }

  static bool hangs(GlyphKind k) { return k == GlyphKind::Space || k == GlyphKind::Invisible; }

  float contentWidth(size_t gb, size_t ge) const {
    float w = 0;
    for (size_t k = gb; k < ge; k++) {
      const TextGlyph& t = L.glyphs[k];
      if (!hangs(t.kind)) w = std::max(w, t.x + t.advance);
    }
    return w;
  }

  // Строка без переноса от sg[i] до конца абзаца; возвращает индекс перевода строки (или конца).
  size_t placeUnwrapped(size_t i) {
    float x = 0;
    size_t j = i;
    for (; j < sg.size() && sg[j].kind != IK::Newline; j++) {
      float adv = sg[j].kind == IK::Tab ? tabAdvance(x) : sg[j].adv;
      push(sg[j], x, adv);
      x += adv;
    }
    return j;
  }

  void pushEllipsis(float x, u32 byte) {
    if (!haveEll || ell.count == 0) return;
    for (int k = 0; k < ell.count; k++) {
      TextGlyph t;
      t.x = x;
      t.advance = ell.adv;
      t.byte = byte;
      t.glyph = ell.glyph;
      t.face = ell.face;
      t.kind = GlyphKind::Ellipsis;
      L.glyphs.push_back(t);
      x += ell.adv;
    }
  }

  // Обрезка строки с многоточием до ширины limit (force — многоточие даже если всё помещается;
  // last — строка последняя, текст дальше не показывается).
  void fitEllipsis(TextLine& line, float limit, bool force, bool last) {
    size_t gb = line.glyphBegin;
    size_t ge = L.glyphs.size();
    float full = contentWidth(gb, ge);
    if (!force && full <= limit + 0.01f) return;
    float ellW = haveEll ? ell.adv * float(ell.count) : 0;
    size_t k = ge;
    if (!force || full + ellW > limit + 0.01f) {
      while (k > gb) {
        const TextGlyph& t = L.glyphs[k - 1];
        if (t.x + t.advance + ellW <= limit + 0.01f) break;
        k--;
      }
    }
    // Не разрывать основу и диакритику, не оставлять пробел перед многоточием.
    while (k > gb && k < ge && L.glyphs[k].advance == 0 && L.glyphs[k].kind == GlyphKind::Normal) k--;
    // Перед многоточием не остаются пробелы и знаки препинания («налог 15 %…», а не «15 %.…»).
    auto trailing = [&](const TextGlyph& t) {
      if (hangs(t.kind) || t.kind == GlyphKind::Tab || t.kind == GlyphKind::NoBreak) return true;
      if (t.byte >= text.size()) return false;
      char ch = text[t.byte];
      return ch == '.' || ch == ',' || ch == ';' || ch == ':';
    };
    while (k > gb && trailing(L.glyphs[k - 1])) k--;
    u32 cut = k < ge ? L.glyphs[k].byte : line.end;
    float x = k > gb ? L.glyphs[k - 1].x + L.glyphs[k - 1].advance : 0;
    L.glyphs.resize(k);
    pushEllipsis(x, cut);
    line.end = std::min(line.end, cut);
    if (last) {
      line.next = line.end;
      line.hardBreak = false;
    }
    L.truncated = true;
  }

  void finishLine(TextLine& line) {
    line.glyphEnd = u32(L.glyphs.size());
    line.width = contentWidth(line.glyphBegin, line.glyphEnd);
    L.lines.push_back(line);
  }

  void run() {
    size_t n = sg.size();
    u32 textEnd = L.textBytes;
    size_t i = 0;
    while (true) {
      TextLine line;
      line.begin = i < n ? sg[i].byte : textEnd;
      line.glyphBegin = u32(L.glyphs.size());
      float x = 0;
      size_t brkIdx = 0, brkG = 0;
      bool brkSoft = false;
      size_t j = i;
      size_t nextI = n;
      bool hard = false, overflow = false;
      for (; j < n; j++) {
        const SG& g = sg[j];
        if (g.kind == IK::Newline) { hard = true; break; }
        float adv = g.kind == IK::Tab ? tabAdvance(x) : g.adv;
        bool visible = g.kind == IK::Normal || g.kind == IK::NoBreak || g.kind == IK::Tab;
        if (wrapping && visible && adv > 0 && j > i && x + adv > maxW + 0.01f) { overflow = true; break; }
        push(g, x, adv);
        x += adv;
        if (g.kind == IK::SoftHyphen && wrapping && haveHyph && x + hyph.adv <= maxW + 0.01f) {
          brkIdx = j + 1; brkG = L.glyphs.size(); brkSoft = true;
        } else if (g.brk) {
          brkIdx = j + 1; brkG = L.glyphs.size(); brkSoft = false;
        }
      }
      if (overflow) {
        if (brkIdx > i) {
          L.glyphs.resize(brkG);
          nextI = brkIdx;
          if (brkSoft) {
            const TextGlyph& last = L.glyphs.back();
            TextGlyph t;
            t.x = last.x + last.advance;
            t.advance = hyph.adv;
            t.byte = last.byte;
            t.glyph = hyph.glyph;
            t.face = hyph.face;
            t.kind = GlyphKind::Hyphen;
            L.glyphs.push_back(t);
          }
        } else {
          // Слово длиннее строки: разрыв по символам (не отрывая диакритику от основы).
          size_t k = j;
          while (k > i + 1 && sg[k].adv == 0 && sg[k].kind == IK::Normal) k--;
          L.glyphs.resize(line.glyphBegin + (k - i));
          nextI = k;
        }
        line.end = nextI < n ? sg[nextI].byte : textEnd;
        line.next = line.end;
      } else if (hard) {
        line.end = sg[j].byte;
        line.next = sg[j].byte + sg[j].len;
        line.hardBreak = true;
        nextI = j + 1;
      } else {
        line.end = textEnd;
        line.next = textEnd;
        nextI = n;
      }
      // Строка шире предела без переноса: многоточие.
      if (!wrapping && opt.ellipsis && opt.maxWidth > 0) fitEllipsis(line, opt.maxWidth, false, false);
      bool hidden = nextI < n;  // дальше есть текст
      if (opt.maxLines > 0 && int(L.lines.size()) + 1 >= opt.maxLines && (hidden || hard)) {
        if (hidden) {
          L.truncated = true;
          if (opt.ellipsis) {
            // Последняя строка: остаток абзаца без переноса, обрезанный с многоточием.
            L.glyphs.resize(line.glyphBegin);
            size_t pe = placeUnwrapped(i);
            line.end = pe < n ? sg[pe].byte : textEnd;
            float limit = opt.maxWidth > 0 ? opt.maxWidth : std::numeric_limits<float>::infinity();
            fitEllipsis(line, limit, true, true);
          }
        }
        line.next = line.end;
        line.hardBreak = false;
        finishLine(line);
        break;
      }
      finishLine(line);
      if (nextI >= n) {
        if (hard) {
          // Текст кончается переводом строки — пустая последняя строка (для каретки).
          if (opt.maxLines > 0 && int(L.lines.size()) >= opt.maxLines) break;
          TextLine last;
          last.begin = last.end = last.next = textEnd;
          last.glyphBegin = last.glyphEnd = u32(L.glyphs.size());
          L.lines.push_back(last);
        }
        break;
      }
      i = nextI;
    }
  }
};

void finalize(TextLayout& L, const LayoutOptions& opt, const FontMetrics& m) {
  L.lineHeight = m.lineHeight;
  L.ascent = m.ascent;
  L.descent = m.descent;
  float w = 0;
  for (auto& ln : L.lines) w = std::max(w, ln.width);
  L.width = w;
  L.boxWidth = opt.maxWidth > 0 ? opt.maxWidth : w;
  for (size_t li = 0; li < L.lines.size(); li++) {
    TextLine& ln = L.lines[li];
    float off = 0;
    if (opt.align == Align::Center) off = (L.boxWidth - ln.width) * 0.5f;
    else if (opt.align == Align::Right) off = L.boxWidth - ln.width;
    ln.x = off;
    ln.y = float(li) * m.lineHeight;
    ln.baseline = ln.y + m.baseline;
    if (off != 0)
      for (u32 g = ln.glyphBegin; g < ln.glyphEnd; g++) L.glyphs[g].x += off;
  }
  L.height = float(L.lines.size()) * m.lineHeight;
}

FontMetrics metricsFor(const Ctx& cx, const TextStyle& st) {
  FontMetrics m;
  const Font& f = *cx.f0;
  float k = cx.k0;
  m.ascent = float(f.ascender()) * k;
  m.descent = float(-f.descender()) * k;
  m.lineGap = float(f.lineGap()) * k;
  float natural = m.ascent + m.descent + m.lineGap;
  m.lineHeight = st.lineHeight > 0 && std::isfinite(st.lineHeight) ? st.lineHeight * cx.size : natural;
  m.baseline = (m.lineHeight - (m.ascent + m.descent)) * 0.5f + m.ascent;
  m.capHeight = float(f.capHeight()) * k;
  m.xHeight = float(f.xHeight()) * k;
  return m;
}

void emptyLayout(TextLayout& L, const TextStyle& st, std::string_view text) {
  L.glyphs.clear();
  L.lines.clear();
  L.style = st;
  L.size = quantSize(st.size);
  L.width = L.height = L.boxWidth = 0;
  L.lineHeight = L.ascent = L.descent = 0;
  L.truncated = false;
  L.textBytes = u32(std::min<size_t>(text.size(), 0xFFFFFFFFu));
}

}  // namespace

// ================================================================ публичные функции
bool initFonts(std::string* error) {
  bool ok = ensureFonts();
  if (!ok && error) *error = reg().error;
  return ok;
}

bool fontsReady() { return ensureFonts(); }

std::vector<std::string> fontReport() {
  std::vector<std::string> out;
  if (!ensureFonts()) {
    out.push_back(reg().error);
    return out;
  }
  static const char* fam[3] = {"UI", "Display", "Mono"};
  static const char* wt[3] = {"Regular", "Semibold", "Bold"};
  Registry& r = reg();
  for (int f = 0; f < 3; f++)
    for (int w = 0; w < 3; w++) {
      const FontFace& face = r.sys.faces[f][w];
      std::string s = std::string(fam[f]) + " " + wt[w] + ": " + (face.font ? face.font->fullName() : "?");
      if (face.synth != Synth::None) s += face.synth == Synth::Bold ? " (синтетический жирный)" : " (синтетический полужирный)";
      s += " — " + face.path;
      out.push_back(std::move(s));
    }
  for (const FontFace& fb : r.sys.fallbacks) out.push_back("Запасной: " + fb.path + (fb.index ? " #" + std::to_string(fb.index) : ""));
  return out;
}

const Font* primaryFont(FontFamily family, FontWeight weight, Synth* synth) {
  if (!ensureFonts()) return nullptr;
  TextStyle st;
  st.family = family;
  st.weight = weight;
  const Chain& ch = chainOf(st);
  if (synth) *synth = ch.e[0].synth;
  return ch.e[0].slot->get();
}

FontMetrics metrics(const TextStyle& style) {
  if (!ensureFonts()) return {};
  Ctx cx(style);
  return metricsFor(cx, style);
}

float measureText(std::string_view text, const TextStyle& style) {
  if (!ensureFonts() || text.empty()) return 0;
  Ctx cx(style);
  thread_local std::vector<SG> sg;
  shape(text, cx, sg);
  float tabW = std::max(1.f, cx.space * 4);
  float best = 0, x = 0, w = 0;
  for (const SG& g : sg) {
    if (g.kind == IK::Newline) {
      best = std::max(best, w);
      x = w = 0;
      continue;
    }
    float adv = g.kind == IK::Tab ? (std::floor(x / tabW + 1e-4f) + 1) * tabW - x : g.adv;
    x += adv;
    if (g.kind != IK::Space && g.kind != IK::Invisible && g.kind != IK::SoftHyphen) w = x;
  }
  return std::max(best, w);
}

void layoutTextInto(TextLayout& L, std::string_view text, const TextStyle& style, const LayoutOptions& opt) {
  emptyLayout(L, style, text);
  if (!ensureFonts()) return;
  Ctx cx(style);
  L.size = cx.size;
  FontMetrics m = metricsFor(cx, style);
  thread_local std::vector<SG> sg;
  shape(text.substr(0, L.textBytes), cx, sg);
  Builder b{L, sg, cx, opt, text.substr(0, L.textBytes)};
  b.tabW = std::max(1.f, cx.space * 4);
  b.maxW = opt.maxWidth;
  b.wrapping = opt.wrap && opt.maxWidth > 0;
  if (opt.ellipsis) {
    b.ell = specialGlyph(cx, 0x2026, '.', 3);
    b.haveEll = b.ell.count > 0;
  }
  b.hyph = specialGlyph(cx, '-', 0x2010, 1);
  b.haveHyph = b.hyph.count > 0;
  b.run();
  finalize(L, opt, m);
}

TextLayout layoutText(std::string_view text, const TextStyle& style, const LayoutOptions& opt) {
  TextLayout L;
  layoutTextInto(L, text, style, opt);
  return L;
}

TextLayout layoutText(std::string_view text, const TextStyle& style, float maxWidth, int maxLines, bool ellipsis) {
  LayoutOptions o;
  o.maxWidth = maxWidth;
  o.maxLines = maxLines;
  o.wrap = maxWidth > 0;
  o.ellipsis = ellipsis;
  return layoutText(text, style, o);
}

// ---------------------------------------------------------------- отрисовка
static bool drawable(GlyphKind k) { return k == GlyphKind::Normal || k == GlyphKind::Hyphen || k == GlyphKind::Ellipsis; }

void rasterizeLayout(const TextLayout& L, float x, float y, float scale, TextTone tone, const GlyphSink& sink) {
  if (L.glyphs.empty() || !ensureFonts() || !(scale > 0)) return;
  const Chain& ch = chainOf(L.style);
  float rs = L.size * scale;
  u16 size4 = u16(std::clamp(std::lround(rs * 4), 1L, 65535L));
  thread_local std::vector<GlyphRequest> req;
  thread_local std::vector<std::shared_ptr<const GlyphBitmap>> bmp;
  thread_local std::vector<std::pair<int, int>> pos;
  req.clear();
  pos.clear();
  for (const TextLine& ln : L.lines) {
    int by = int(std::lround(y + ln.baseline * scale));
    for (u32 gi = ln.glyphBegin; gi < ln.glyphEnd; gi++) {
      const TextGlyph& g = L.glyphs[gi];
      if (!drawable(g.kind) || g.face >= ch.e.size()) continue;
      float px = x + g.x * scale;
      float fx = std::floor(px);
      int sub = int(std::lround((px - fx) * 4));
      int ix = int(fx);
      if (sub >= 4) { ix++; sub = 0; }
      GlyphRequest r;
      r.font = ch.e[g.face].slot->get();
      if (!r.font) continue;
      r.glyph = g.glyph;
      r.size4 = size4;
      r.subX = u8(sub);
      r.synth = ch.e[g.face].synth;
      r.tone = tone;
      req.push_back(r);
      pos.push_back({ix, by});
    }
  }
  if (req.empty()) return;
  bmp.resize(req.size());
  glyphCache().getMany(req.data(), req.size(), bmp.data());
  for (size_t k = 0; k < req.size(); k++) {
    const GlyphBitmap* b = bmp[k].get();
    if (b && !b->mask.empty()) sink(b->mask, pos[k].first + b->left, pos[k].second + b->top);
  }
  for (auto& p : bmp) p.reset();
}

void layoutPath(const TextLayout& L, float x, float y, Path& out) {
  if (L.glyphs.empty() || !ensureFonts()) return;
  const Chain& ch = chainOf(L.style);
  GlyphOutline o;
  for (const TextLine& ln : L.lines) {
    for (u32 gi = ln.glyphBegin; gi < ln.glyphEnd; gi++) {
      const TextGlyph& g = L.glyphs[gi];
      if (!drawable(g.kind) || g.face >= ch.e.size()) continue;
      const Font* f = ch.e[g.face].slot->get();
      if (!f || !f->outline(g.glyph, o)) continue;
      float k = L.size / float(f->unitsPerEm());
      float strength = synthStrength(ch.e[g.face].synth, L.size);
      o.transform(Affine{k, 0, 0, -k, synthAdvance(*f, ch.e[g.face].synth, L.size) * 0.5f, 0});
      if (strength > 0) o.embolden(strength);
      o.appendTo(out, Affine::translate(x + g.x, y + ln.baseline));
    }
  }
}

void drawLayout(Canvas& c, const TextLayout& L, float x, float y, const Paint& paint) {
  if (L.glyphs.empty() || !std::isfinite(x) || !std::isfinite(y)) return;
  const Affine t = c.transform();
  // Границы с запасом на выносные элементы и наклон глифов.
  float x0 = 0, x1 = L.boxWidth;
  for (const TextLine& ln : L.lines) {
    x0 = std::min(x0, ln.x);
    x1 = std::max(x1, ln.x + ln.width);
  }
  float over = L.size;
  if (c.quickReject(RectF{x + x0 - over, y - over * 0.5f, x1 - x0 + 2 * over, L.height + over})) return;
  bool axis = t.b == 0 && t.c == 0 && t.a > 0 && std::fabs(t.a - t.d) <= 1e-4f * t.a;
  float s = t.a;
  if (!axis || L.size * s > 180.f) {
    // Поворот, отражение, неравномерный масштаб или крупный кегль — контурами.
    thread_local Path path;
    path.clear();
    layoutPath(L, x, y, path);
    if (!path.empty()) c.fillPath(path, paint);
    return;
  }
  TextTone tone = (paint.gradient || paint.image) ? TextTone::Mid : toneOf(paint.color);
  float dx = t.a * x + t.e, dy = t.d * y + t.f;
  bool intTranslate = s == 1.f && t.e == std::floor(t.e) && t.f == std::floor(t.f);
  if (intTranslate) {
    rasterizeLayout(L, dx, dy, 1.f, tone, [&](const Mask& m, int mx, int my) { c.fillMask(m, float(mx) - t.e, float(my) - t.f, paint); });
    return;
  }
  c.save();
  c.setTransform(Affine{});
  rasterizeLayout(L, dx, dy, s, tone, [&](const Mask& m, int mx, int my) { c.fillMask(m, float(mx), float(my), paint); });
  c.restore();
}

void drawText(Canvas& c, std::string_view text, const TextStyle& style, float x, float y, const Paint& paint) {
  if (text.empty()) return;
  thread_local TextLayout L;
  layoutTextInto(L, text, style, LayoutOptions{});
  drawLayout(c, L, x, y, paint);
}

RectF drawTextBox(Canvas& c, std::string_view text, const TextStyle& style, RectF box, const Paint& paint, Align align, VAlign valign,
                  bool wrap, int maxLines, bool ellipsis) {
  thread_local TextLayout L;
  LayoutOptions o;
  o.wrap = wrap;
  o.ellipsis = ellipsis;
  o.align = align;
  o.maxLines = std::max(0, maxLines);
  o.maxWidth = (wrap || ellipsis || align != Align::Left) ? std::max(box.w, 1.f) : 0;
  if (wrap && ellipsis && o.maxLines == 0) {
    FontMetrics m = metrics(style);
    if (m.lineHeight > 0) o.maxLines = std::max(1, int((box.h + 0.5f) / m.lineHeight));
  }
  layoutTextInto(L, text, style, o);
  float ty = box.y;
  if (valign == VAlign::Middle) ty = box.y + (box.h - L.height) * 0.5f;
  else if (valign == VAlign::Bottom) ty = box.bottom() - L.height;
  drawLayout(c, L, box.x, ty, paint);
  float minX = L.boxWidth;
  for (auto& ln : L.lines) minX = std::min(minX, ln.x);
  if (L.lines.empty()) minX = 0;
  return {box.x + minX, ty, L.width, L.height};
}

// ---------------------------------------------------------------- каретка и выделение
size_t lineOf(const TextLayout& L, size_t byte) {
  if (L.lines.empty()) return 0;
  for (size_t i = 0; i + 1 < L.lines.size(); i++)
    if (byte < L.lines[i].next) return i;
  return L.lines.size() - 1;
}

static float caretX(const TextLayout& L, const TextLine& ln, size_t byte) {
  float endX = ln.x;
  for (u32 gi = ln.glyphBegin; gi < ln.glyphEnd; gi++) {
    const TextGlyph& g = L.glyphs[gi];
    if (g.kind == GlyphKind::Hyphen) continue;
    if (g.byte >= byte) return g.x;
    endX = g.x + g.advance;
  }
  return endX;
}

Pt caretPos(const TextLayout& L, size_t byte) {
  if (L.lines.empty()) return {0, 0};
  const TextLine& ln = L.lines[lineOf(L, byte)];
  return {caretX(L, ln, byte), ln.y};
}

size_t hitTest(const TextLayout& L, float x, float y) {
  if (L.lines.empty()) return 0;
  int li = L.lineHeight > 0 ? int(std::floor(y / L.lineHeight)) : 0;
  li = std::clamp(li, 0, int(L.lines.size()) - 1);
  const TextLine& ln = L.lines[size_t(li)];
  for (u32 gi = ln.glyphBegin; gi < ln.glyphEnd; gi++) {
    const TextGlyph& g = L.glyphs[gi];
    if (g.kind == GlyphKind::Hyphen) continue;
    if (g.kind == GlyphKind::Ellipsis) return g.byte;
    if (x < g.x + g.advance * 0.5f) return g.byte;
  }
  // Справа от строки: конец строки; при мягком переносе по пробелу — перед этим пробелом.
  if (size_t(li) + 1 < L.lines.size() && !ln.hardBreak && ln.glyphEnd > ln.glyphBegin) {
    const TextGlyph& last = L.glyphs[ln.glyphEnd - 1];
    if (last.kind == GlyphKind::Space) return last.byte;
    if (last.kind == GlyphKind::Hyphen) return last.byte;
  }
  return ln.end;
}

std::vector<RectF> selectionRects(const TextLayout& L, size_t a, size_t b) {
  std::vector<RectF> out;
  if (a > b) std::swap(a, b);
  if (a == b || L.lines.empty()) return out;
  for (size_t i = 0; i < L.lines.size(); i++) {
    const TextLine& ln = L.lines[i];
    size_t lb = ln.begin, le = ln.end;
    if (b <= lb) break;
    if (a >= ln.next && i + 1 < L.lines.size()) continue;
    size_t s0 = std::max(a, lb), s1 = std::min(b, le);
    if (s0 > le) continue;
    float x0 = caretX(L, ln, s0), x1 = caretX(L, ln, s1);
    // Выделенный перевод строки — небольшой хвост.
    if (b > le && ln.hardBreak) x1 += std::max(2.f, L.size * 0.3f);
    if (x1 > x0) out.push_back({x0, ln.y, x1 - x0, L.lineHeight});
  }
  return out;
}

std::string ellipsize(std::string_view text, const TextStyle& style, float maxWidth) {
  LayoutOptions o;
  o.maxWidth = std::max(maxWidth, 0.f);
  o.maxLines = 1;
  o.wrap = false;
  o.ellipsis = true;
  thread_local TextLayout L;
  layoutTextInto(L, text, style, o);
  if (!L.truncated) return std::string(text);
  u32 cut = L.lines.empty() ? 0 : L.lines[0].end;
  std::string s(text.substr(0, std::min<size_t>(cut, text.size())));
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
  s += "\xE2\x80\xA6";
  return s;
}

}  // namespace rg::gfx
