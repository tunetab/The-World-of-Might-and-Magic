// Regnum — диалог «Флаг» (ТЗ 1.b.iv: флаг государства и гильдии): живой предпросмотр (крупно и в размерах списков),
// сетка узоров (все FlagPattern мини-флагами), три цвета и цвет эмблемы (геральдическая палитра), сетка эмблем с
// русскими названиями и «без эмблемы», загрузка изображения PNG/JPEG (системный диалог, без него — ввод пути;
// хранится как PNG в Flag::png), сброс к узору. Применение — одним действием (Ctrl+Z отменяет).
#include "app/panels/faction_common.h"
#include "base/fs.h"
#include "codec/jpeg.h"
#include "codec/png.h"
#include "gfx/emblems.h"
#include "gfx/flag.h"

namespace rg::app {
namespace {

using namespace fac;

constexpr int kMaxImageSide = 640;   // больше не нужно: флаг рисуется не крупнее предпросмотра

struct Shared {
  Flag flag;
  std::string fileName;
};

// Прочитать изображение PNG/JPEG и записать во флаг как PNG (уменьшив при необходимости).
bool loadImageInto(App& a, Shared& st, const std::string& path) {
  auto bytes = fs::readFile(path);
  if (!bytes) {
    a.toast("Не удалось прочитать файл «" + fs::filename(path) + "»", ToastKind::Warning, "image");
    return false;
  }
  auto rgba = codec::decodePng(*bytes);
  if (!rgba) rgba = codec::decodeJpeg(*bytes);
  if (!rgba || rgba->empty()) {
    a.toast("Файл не похож на изображение PNG или JPEG", ToastKind::Warning, "image");
    return false;
  }
  codec::RgbaImage out;
  if (std::max(rgba->w, rgba->h) > kMaxImageSide) {
    gfx::Image img = gfx::Image::fromRgba(rgba->rgba.data(), rgba->w, rgba->h);
    double k = double(kMaxImageSide) / std::max(img.w, img.h);
    img = img.scaled(std::max(1, int(std::lround(img.w * k))), std::max(1, int(std::lround(img.h * k))));
    out.w = img.w;
    out.h = img.h;
    out.rgba = img.toRgba();
  } else {
    out = std::move(*rgba);
  }
  std::vector<u8> png = codec::encodePng(out, 6);
  if (png.empty() || !codec::decodePng(png.data(), png.size())) {
    a.toast("Не удалось подготовить изображение флага", ToastKind::Warning, "image");
    return false;
  }
  st.flag.image = true;
  st.flag.png.assign(png.begin(), png.end());
  st.fileName = fs::filename(path);
  return true;
}

// Цвет поля под эмблемой (gfx/flag.h: эмблема — по центру или по месту узора): фон ячеек сетки эмблем.
Color emblemGround(const Flag& f) {
  const Color c0 = f.colors[0], c1 = f.colors[1], c2 = f.colors[2];
  switch (f.pattern) {
    case FlagPattern::H3:
    case FlagPattern::V3:
    case FlagPattern::Bend:
    case FlagPattern::Canton:
    case FlagPattern::Pale: return c1;
    case FlagPattern::Cross:
    case FlagPattern::Saltire: return c2 == c1 ? c1 : c2;
    case FlagPattern::Chevron: return c2;
    default: return c0;
  }
}

// Ячейка сетки: весь прямоугольник — цель щелчка и фокуса, подсказка — название.
struct CellHit {
  bool clicked = false, hovered = false, focused = false;
};
CellHit cell(std::string_view key, RectF r, std::string_view tip, bool selected, bool disabled) {
  const ui::Theme& t = ui::theme();
  ui::WidgetId wid = ui::id(key);
  ui::Interaction it = disabled ? ui::Interaction{} : ui::interact(wid, r, ui::IfFocusable);
  float hv = ui::animate(wid ^ 0xce11ull, it.hovered ? 1.f : 0.f, 0.1f);
  if (selected) {
    ui::draw::rect(r, t.accent.alpha(t.dark ? 0.16f : 0.13f), 8);
    ui::draw::rectStroke(r, t.accent, 8, 2);
  } else {
    ui::draw::rect(r, t.dark ? Color::mix(t.stripe, t.hover, hv) : Color::mix(t.surface3.alpha(0.5f), t.surface3, hv), 8);
    ui::draw::rectStroke(r, hv > 0.01f ? t.borderStrong : t.border, 8, 1);
  }
  if (it.focused) focusRing(r, 8);
  if (it.hovered) ui::setCursor(platform::Cursor::Hand);
  if (!disabled) tipOver(r, std::string(key) + "#tip", tip);
  return {it.clicked, it.hovered, it.focused};
}

class FlagEditor final : public Dialog {
 public:
  FlagEditor(Id faction, const Faction& f) : id_(faction), name_(displayName(f)), orig_(f.flag), st_(std::make_shared<Shared>()) {
    st_->flag = f.flag;
  }
  const char* id() const override { return "flag"; }
  Style style(App&) override { return {"Флаг · " + name_, "flag", ui::Tone::Accent, 660}; }

  bool draw(App& a) override {
    if (!a.world().faction(id_)) return false;   // фракцию удалили (например, отменой)
    const ui::Theme& t = ui::theme();
    Flag& fl = st_->flag;
    const bool img = fl.image && !fl.png.empty();

    // Предпросмотр и цвета.
    {
      ui::Row row({ui::px(252), ui::fr(1)}, ui::kAuto, 22);
      {
        ui::Group g(252, 10);
        RectF pr = ui::next(252, 168);
        drawFlagCopy(fl, pr, 8);
        a.markUi("flag.preview", pr);
        ui::HStack hs(32, ui::Align::Left, 12);
        for (float w : {48.f, 36.f, 27.f, 21.f}) {
          RectF r = ui::next(w, 32);
          drawFlagCopy(fl, RectF{r.x, std::round(r.cy() - w / 3), w, std::round(w * 2 / 3)}, w > 30 ? 3.f : 2.f);
        }
        ui::label("в списках", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
      }
      {
        ui::Group g(0, 8);
        ui::caption("Цвета");
        {
          ui::Disabled d(img);
          const char* names[] = {"Поле", "Узор", "Третий"};
          for (int i = 0; i < 3; i++) {
            ui::IdScope s{i};
            ui::prop(names[i], nullptr, 0.38f);
            Color c = fl.colors[size_t(i)];
            if (ui::colorButton("c", c, {.tooltip = names[i]})) fl.colors[size_t(i)] = c;
            a.markUi("flag.color." + std::to_string(i));
          }
          ui::prop("Эмблема", nullptr, 0.38f);
          Color ec = fl.emblemColor;
          if (ui::colorButton("ec", ec, {.tooltip = "Цвет эмблемы"})) fl.emblemColor = ec;
        }
        ui::spacer(2);
        {
          ui::HStack hs(30, ui::Align::Left, 8);
          if (ui::button(img ? "Другое изображение…" : "Загрузить изображение…", {.icon = "upload", .size = ui::Size::Small})) pickImage(a);
          a.markUi("flag.upload");
          if (img) {
            if (ui::button("К узору", {.icon = "refresh", .size = ui::Size::Small, .tooltip = "Убрать изображение и вернуть узор с эмблемой"})) {
              fl.image = false;
              fl.png.clear();
              st_->fileName.clear();
            }
            a.markUi("flag.reset");
          }
        }
        if (img)
          ui::label(st_->fileName.empty() ? std::string("Изображение") : st_->fileName, {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "image"});
        else
          ui::label("PNG или JPEG; хранится в мире как PNG", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
      }
    }
    ui::spacer(4);

    // Узоры.
    {
      const int n = int(FlagPattern::Count);
      int hovered = -1;
      const float cw = 78, ch = 54, gap = 8;
      float W = ui::avail().w;
      int perRow = std::max(1, int((W + gap) / (cw + gap)));
      int rows = (n + perRow - 1) / perRow;
      std::string capRow = "Узор · " + std::string(schema::kFlagPatterns[int(fl.pattern)].name);
      RectF capR = ui::next(18);
      RectF area = ui::next(rows * ch + (rows - 1) * gap);
      for (int i = 0; i < n; i++) {
        ui::IdScope s{i};
        RectF r{area.x + float(i % perRow) * (cw + gap), area.y + float(i / perRow) * (ch + gap), cw, ch};
        CellHit h = cell("pat", r, schema::kFlagPatterns[i].name, !img && int(fl.pattern) == i, img);
        if (h.hovered) hovered = i;
        Flag mini = fl;
        mini.image = false;
        mini.png.clear();
        mini.pattern = FlagPattern(i);
        mini.emblem.clear();
        RectF fr{r.x + 9, r.y + 7, cw - 18, ch - 14};
        drawFlagCopy(mini, fr, 3);
        if (img) ui::draw::rect(r, t.surface2.alpha(0.55f), 8);
        if (h.clicked) fl.pattern = FlagPattern(i);
        a.markUi("flag.pattern." + std::to_string(i), r);
      }
      if (hovered >= 0) capRow = "Узор · " + std::string(schema::kFlagPatterns[hovered].name);
      ui::draw::text(utf8::upper(capRow), capR, ui::Font::Caption, t.textMuted);
    }
    ui::spacer(4);

    // Эмблемы.
    {
      const auto& names = gfx::emblemNames();
      const int n = int(names.size()) + 1;   // первая — «без эмблемы»
      const float cs = 44, gap = 6;
      float W = ui::avail().w;
      int perRow = std::max(1, int((W + gap) / (cs + gap)));
      int rows = (n + perRow - 1) / perRow;
      std::string cur = fl.emblem.empty() ? std::string("без эмблемы") : std::string(gfx::emblemTitle(fl.emblem));
      int hovered = -1;
      RectF capR = ui::next(18);
      RectF area = ui::next(rows * cs + (rows - 1) * gap);
      for (int i = 0; i < n; i++) {
        ui::IdScope s{i};
        RectF r{area.x + float(i % perRow) * (cs + gap), area.y + float(i / perRow) * (cs + gap), cs, cs};
        std::string nm = i == 0 ? std::string() : names[size_t(i - 1)];
        std::string title = i == 0 ? std::string("Без эмблемы") : std::string(gfx::emblemTitle(nm));
        bool sel = fl.emblem == nm;
        CellHit h = cell("emb", r, title, !img && sel, img);
        if (h.hovered) hovered = i;
        RectF in = r.inset(5);
        ui::draw::rect(in, emblemGround(fl), 5);
        if (i == 0) {
          ui::draw::line(in.x + 6, in.bottom() - 6, in.right() - 6, in.y + 6, fl.emblemColor.alpha(0.85f), 2);
        } else {
          Color ec = fl.emblemColor;
          ui::custom(in.inset(3), [nm, ec](gfx::Canvas& c, RectF d, float) { gfx::drawEmblem(c, nm, d, ec); });
        }
        if (img) ui::draw::rect(r, t.surface2.alpha(0.55f), 8);
        if (h.clicked) fl.emblem = nm;
        a.markUi("flag.emblem." + (i == 0 ? std::string("none") : nm), r);
      }
      std::string capRow = "Эмблема · " + (hovered < 0 ? cur : hovered == 0 ? std::string("без эмблемы") : std::string(gfx::emblemTitle(names[size_t(hovered - 1)])));
      ui::draw::text(utf8::upper(capRow), capR, ui::Font::Caption, t.textMuted);
    }

    ui::ModalFooter foot;
    if (ui::button("Отмена")) return false;
    a.markUi("flag.cancel");
    bool changed = !sameFlag(fl, orig_);
    if (ui::button("Применить", {.variant = ui::Variant::Primary, .icon = "check", .disabled = a.readOnly(), .isDefault = true})) {
      if (!changed) return false;
      Flag nf = fl;
      if (a.act("Флаг: " + name_, [&](Tx& tx) { tx.faction(id_).flag = nf; })) return false;
    }
    a.markUi("flag.ok");
    return true;
  }

 private:
  static bool sameFlag(const Flag& x, const Flag& y) {
    return x.image == y.image && x.pattern == y.pattern && x.colors == y.colors && x.emblem == y.emblem && x.emblemColor == y.emblemColor &&
           x.png == y.png;
  }

  void pickImage(App& a) {
    std::shared_ptr<Shared> st = st_;
    detail::later(a, [st](App& x) {
      if (platform::dialogsSupported()) {
        auto path = platform::openFileDialog("Изображение флага", {{"Изображения PNG и JPEG", {"png", "jpg", "jpeg"}}}, fs::documentsDir());
        if (path) loadImageInto(x, *st, *path);
        return;
      }
      // Системных диалогов нет (а свой проводник выбирает только миры) — ввод пути к файлу.
      x.prompt("Изображение флага", "Путь к файлу PNG или JPEG", "", [st](App& y, const std::string& p) {
        std::string path = trim(p);
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
        loadImageInto(y, *st, path);
      });
    });
  }

  Id id_;
  std::string name_;
  Flag orig_;
  std::shared_ptr<Shared> st_;
};

std::unique_ptr<Dialog> makeFlagEditor(App& a, Id arg) {
  const Faction* f = a.world().faction(arg);
  if (!f) fail("Фракция не найдена");
  return std::make_unique<FlagEditor>(arg, *f);
}

DialogReg reg({"flag", makeFlagEditor});

}  // namespace
}  // namespace rg::app
