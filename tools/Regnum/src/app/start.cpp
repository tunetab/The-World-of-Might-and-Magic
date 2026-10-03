// Regnum — экран запуска: размытая базовая карта, знак и название, большие действия, предложение
// восстановить автосохранение, недавние миры с миниатюрами политической карты.
#include "app/app_internal.h"
#include "app/logo.h"
#include "base/fs.h"
#include "gfx/text.h"

namespace rg::app::detail {

namespace {

App::Impl& D(App& a) { return a.impl(); }

// Перекраска базовой карты (белая суша, синее море) в цвета темы: доля «суши» — по красному и зелёному каналам.
gfx::Image recolor(const gfx::Image& src, Color sea, Color land, Color ink) {
  gfx::Image out(src.w, src.h);
  for (size_t i = 0; i < src.px.size(); i++) {
    u32 p = src.px[i];
    float r = float((p >> 16) & 255) / 255.f, g = float((p >> 8) & 255) / 255.f, b = float(p & 255) / 255.f;
    float landK = clamp((r + g) * 0.5f, 0.f, 1.f);
    // Тёмные штрихи (реки, горы, подписи на суше) — чернилами поверх суши.
    float lum = 0.3f * r + 0.59f * g + 0.11f * b;
    float inkK = landK > 0.25f && b > 0.2f ? clamp((0.9f - lum) * 0.8f, 0.f, 0.3f) : 0.f;
    Color c = Color::mix(sea, land, landK);
    c = Color::mix(c, ink, inkK);
    out.px[i] = gfx::premul(c);
  }
  return out;
}

std::string shortPath(const std::string& p) {
  std::string home = fs::homeDir();
  if (!home.empty() && startsWith(p, home)) return "~" + p.substr(home.size());
  return p;
}

// Плитка большого действия.
bool actionTile(App& a, const char* id, RectF r, const char* icon, std::string_view title, std::string_view hint, bool primary) {
  const ui::Theme& th = ui::theme();
  ui::WidgetId wid = ui::id(id);
  ui::Interaction it = ui::interact(wid, r, ui::IfFocusable);
  float hv = ui::animate(wid ^ 1, it.hovered ? 1.f : 0.f, 0.14f);
  float dn = ui::animate(wid ^ 2, it.held ? 1.f : 0.f, 0.08f);
  RectF rr{r.x, r.y - 2 * hv + dn, r.w, r.h};
  ui::draw::shadow(rr, 14, 26 + 10 * hv, th.shadow.alpha(0.85f), 8 + 4 * hv);
  Color bg = th.dark ? Color::mix(th.surface1, th.surface2, 0.35f + 0.5f * hv).alpha(0.94f) : Color::mix(th.surface2, th.surface1, 0.2f * hv).alpha(0.96f);
  ui::draw::rect(rr, bg, 14);
  if (primary) ui::draw::gradient(RectF{rr.x, rr.y, rr.w, rr.h * 0.6f}, th.accent.alpha(0.10f + 0.06f * hv), th.accent.alpha(0.f), 14);
  Color bc = primary ? th.accent.alpha(0.45f + 0.35f * hv) : Color::mix(th.dark ? th.border : th.border, th.borderStrong, hv);
  ui::draw::rectStroke(rr, bc, 14, 1);
  RectF ic{rr.x + 20, rr.y + 20, 44, 44};
  if (primary) ui::draw::gradient(ic, th.accentHover, th.accent.darken(0.08f), 12);
  else ui::draw::rect(ic, th.dark ? th.surface3 : th.surface3, 12);
  ui::draw::icon(icon, ic.inset(11), primary ? th.onAccent : th.accent);
  ui::draw::icon("arrow-right", RectF{rr.right() - 34 + 4 * hv, rr.y + 30, 18, 18}, th.textMuted.alpha(0.4f + 0.6f * hv));
  ui::draw::text(title, RectF{rr.x + 20, rr.y + 76, rr.w - 40, 22}, ui::Font::Subtitle, th.text);
  ui::draw::text(hint, RectF{rr.x + 20, rr.y + 98, rr.w - 40, 18}, ui::Font::Small, th.textMuted);
  if (it.focused) ui::draw::rectStroke(rr.expand(3), th.accent, 17, 2);
  a.markUi(id, r);
  return it.clicked;
}

void recentRow(App& a, const io::RecentProject& rp, RectF r, int index) {
  App::Impl& d = D(a);
  const ui::Theme& th = ui::theme();
  ui::IdScope s(index);
  ui::WidgetId wid = ui::id("row");
  ui::Interaction it = ui::interact(wid, r, ui::IfFocusable | ui::IfAllowOverlap);
  const ui::Mouse& mo = ui::mouse();
  bool over = it.hovered || (r.contains(mo.x, mo.y) && !ui::anyModalOpen());
  float hv = ui::animate(wid ^ 1, over ? 1.f : 0.f, 0.12f);
  if (hv > 0.01f) ui::draw::rect(r, th.hover.alpha(hv), 10);
  // Миниатюра
  RectF tr{r.x + 8, r.y + 7, 88, r.h - 14};
  auto& slot = d.thumbs[rp.path];
  if (!slot) {
    auto img = loadImage(thumbPath(d.dataDir, rp.path), 360);
    slot = std::make_shared<gfx::Image>(img ? std::move(*img) : gfx::Image());
  }
  if (!slot->empty()) {
    ui::draw::image(*slot, tr, 6, rp.exists ? 1.f : 0.45f);
    ui::draw::rectStroke(tr, Color(0, 0, 0, 50), 6, 1);
  } else {
    ui::draw::rect(tr, th.surface3, 6);
    ui::draw::icon(rp.bundle ? "archive" : "map", RectF{tr.cx() - 11, tr.cy() - 11, 22, 22}, th.textMuted);
  }
  float tx = tr.right() + 14;
  float tw = r.right() - tx - 150;
  ui::draw::text(rp.name.empty() ? fs::stem(rp.path) : rp.name, RectF{tx, r.y + 12, tw, 20}, ui::Font::Strong, rp.exists ? th.text : th.textMuted);
  {
    RectF pr{tx, r.y + 34, tw, 18};
    ui::draw::icon(rp.bundle ? "archive" : "folder", RectF{pr.x, pr.y + 2, 14, 14}, th.textMuted);
    ui::draw::text(shortPath(rp.path), RectF{pr.x + 20, pr.y, pr.w - 20, pr.h}, ui::Font::Small, th.textMuted);
  }
  std::string when = rp.exists ? localTime(rp.at) : std::string("не найден");
  ui::draw::text(when, RectF{r.right() - 150, r.y, 104, r.h}, ui::Font::Small, rp.exists ? th.textDim : th.warning, ui::Align::Right);
  // Убрать из списка (кнопка при наведении на строку).
  if (over) {
    ui::at(RectF{r.right() - 36, r.cy() - 12, 24, 24});
    if (ui::iconButton("close", "Убрать из списка", {.size = ui::Size::Small})) {
      io::removeRecent(rp.path, d.dataDir);
      d.recentDirty = true;
      return;
    }
  }
  if (it.clicked) {
    std::string p = rp.path;
    later(a, [p](App& x) { x.openPath(p); });
  }
}

}  // namespace

// ================================================================ фон
void renderStartBackdrop(App& a, gfx::Canvas& c) {
  App::Impl& d = D(a);
  const ui::Theme& th = ui::theme();
  gfx::Image& out = c.target();
  if (d.backdropW != out.w || d.backdropH != out.h || d.backdropDark != th.dark) {
    if (!d.previewTried) {
      d.previewTried = true;
      if (d.basemap) {
        if (auto img = loadImage(d.basemap->previewPath(), 1000)) d.preview = std::move(*img);
      }
    }
    d.backdrop.resize(out.w, out.h);
    gfx::Canvas bc(d.backdrop);
    bc.clear(th.bg);
    int W = out.w, H = out.h;
    if (!d.preview.empty() && (d.backdropBase.empty() || d.backdropBaseDark != th.dark)) {
      // Основа один раз на тему: перекраска и размытие в фиксированном размере (смена размера окна дешёвая).
      Color sea = th.dark ? Color::hex(0x0b1320) : Color::hex(0xcfd9e2);
      Color land = th.dark ? Color::hex(0x2e3847) : Color::hex(0xf3eee3);
      Color ink = th.dark ? Color::hex(0x0e131b) : Color::hex(0xb9ae98);
      d.backdropBase = recolor(d.preview, sea, land, ink);
      gfx::Canvas pc(d.backdropBase);
      pc.blurRegion(gfx::RectI{0, 0, d.backdropBase.w, d.backdropBase.h}, 5.f);
      d.backdropBaseDark = th.dark;
    }
    if (!d.backdropBase.empty()) {
      // Кадр «cover» с лёгким приближением.
      const gfx::Image& base = d.backdropBase;
      float k = std::max(float(W) / float(base.w), float(H) / float(base.h)) * 1.18f;
      float dw = base.w * k, dh = base.h * k;
      bc.drawImage(base, RectF{(W - dw) * 0.42f, (H - dh) * 0.55f, dw, dh}, 1.f, true);
    }
    // Затемнение сверху и снизу (под заголовком и списком) и виньетка.
    gfx::Gradient g;
    g.kind = gfx::Gradient::Linear;
    g.p0 = {0, 0};
    g.p1 = {0, float(H)};
    if (th.dark) g.stops = {{0.f, Color(10, 13, 18, 150)}, {0.45f, Color(10, 13, 18, 95)}, {1.f, Color(10, 13, 18, 215)}};
    else g.stops = {{0.f, Color(244, 241, 234, 120)}, {0.45f, Color(244, 241, 234, 70)}, {1.f, Color(244, 241, 234, 200)}};
    gfx::Paint gp;
    gp.gradient = &g;
    bc.fillRect(RectF{0, 0, float(W), float(H)}, gp);
    gfx::Gradient v;
    v.kind = gfx::Gradient::Radial;
    v.p0 = {W * 0.5f, H * 0.45f};
    v.r1 = std::max(W, H) * 0.75f;
    v.stops = {{0.55f, Color(0, 0, 0, 0)}, {1.f, th.dark ? Color(0, 0, 0, 140) : Color(120, 100, 70, 60)}};
    gfx::Paint vp;
    vp.gradient = &v;
    bc.fillRect(RectF{0, 0, float(W), float(H)}, vp);
    d.backdropW = out.w;
    d.backdropH = out.h;
    d.backdropDark = th.dark;
  }
  std::memcpy(out.px.data(), d.backdrop.px.data(), out.px.size() * sizeof(u32));
}

// ================================================================ экран
void drawStartScreen(App& a) {
  App::Impl& d = D(a);
  const ui::Theme& th = ui::theme();
  if (d.recentDirty || d.recoveryDirty) refreshStartData(a);
  RectF V = ui::viewport();
  float colW = std::min(1000.f, V.w - 96);
  float x0 = std::round((V.w - colW) * 0.5f);
  float y = std::round(std::max(40.f, std::min(V.h * 0.12f, 120.f)));

  // Угол: тема, настройки, справка.
  {
    ui::Area corner(RectF{V.w - 24 - 3 * 30 - 2 * 6, 20, 3 * 30 + 2 * 6, 30}, 0);
    ui::HStack hs(30, ui::Align::Left, 6);
    if (ui::iconButton(th.dark ? "sun" : "moon", th.dark ? "Светлая тема" : "Тёмная тема")) a.setTheme(!a.ui.darkTheme);
    a.markUi("start.theme");
    if (ui::iconButton("settings", "Настройки")) a.showSettings();
    ui::tooltip("Настройки", parseShortcut("Ctrl+,"));
    if (ui::iconButton("keyboard", "Сочетания клавиш")) a.showHelp();
    ui::tooltip("Сочетания клавиш", {platform::Key::F1, 0});
  }

  // Знак и название.
  {
    RectF lr{x0, y, 76, 76};
    ui::custom(lr, [](gfx::Canvas& c, RectF dev, float) { drawLogoTile(c, dev); });
    bool dark = th.dark;
    Color tc = th.text;
    RectF tr{x0 + 96, y + 2, colW - 96, 52};
    ui::custom(tr, [tc, dark](gfx::Canvas& c, RectF dev, float scale) {
      gfx::TextStyle st = ui::textStyle(ui::Font::Display);
      st.size = 46 * scale;
      st.weight = gfx::FontWeight::Bold;
      st.letterSpacing = 0.5f * scale;
      (void)dark;
      gfx::drawText(c, "Regnum", st, dev.x, dev.y - 4 * scale, gfx::Paint(tc));
    });
    ui::draw::text("Редактор мира Меча и Магии", RectF{x0 + 98, y + 54, colW - 98, 22}, ui::Font::Subtitle, th.textDim);
    y += 76 + 36;
  }

  // Действия.
  {
    float gap = 16, tw = std::floor((colW - 2 * gap) / 3), th2 = 124;
    RectF r0{x0, y, tw, th2}, r1{x0 + tw + gap, y, tw, th2}, r2{x0 + 2 * (tw + gap), y, tw, th2};
    if (actionTile(a, "start.new", r0, "plus", "Новый мир", "Береговая линия уже готова", true)) later(a, [](App& x) { x.newWorldDialog(); });
    if (actionTile(a, "start.open", r1, "folder-open", "Открыть папку мира", "Папка с world.json", false)) later(a, [](App& x) { x.openWorldDialog(); });
    if (actionTile(a, "start.bundle", r2, "archive", "Открыть файл .regnum", "Мир одним файлом", false)) later(a, [](App& x) { x.openBundleDialog(); });
    y += th2 + 24;
  }

  // Восстановление после сбоя.
  for (size_t i = 0; i < d.recovery.size() && i < 2; i++) {
    const io::AutosaveInfo info = d.recovery[i];
    RectF r{x0, y, colW, 58};
    ui::draw::shadow(r, 12, 18, th.shadow.alpha(0.7f), 6);
    ui::draw::rect(r, th.dark ? th.surface2.alpha(0.96f) : th.surface2, 12);
    ui::draw::rectStroke(r, th.warning.alpha(0.55f), 12, 1);
    ui::draw::rect(RectF{r.x, r.y + 10, 3, r.h - 20}, th.warning, 2);
    ui::Area ar(r.inset(16, 13), 0);
    ui::IdScope s{int(i)};
    ui::HStack hs(32, ui::Align::Left, 10);
    ui::icon("history", ui::Ink::Warning, 20);
    {
      ui::Group g(std::max(120.f, r.w - 32 - 30 - 300), 0);
      std::string title = "Несохранённая работа: «" + (info.name.empty() ? std::string("Мир") : info.name) + "»";
      ui::label(title, {.font = ui::Font::Strong});
      std::string sub = localTime(info.at) + " · ход " + std::to_string(info.turn) + (info.project.empty() ? " · новый мир" : " · " + shortPath(info.project));
      ui::label(sub, {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    }
    ui::flex();
    if (ui::button("Отклонить", {.variant = ui::Variant::Ghost, .size = ui::Size::Small})) {
      io::clearAutosave(info.project, d.dataDir);
      d.recoveryDirty = true;
    }
    a.markUi("start.recovery.discard");
    if (ui::button("Восстановить", {.variant = ui::Variant::Primary, .icon = "history", .size = ui::Size::Small})) later(a, [info](App& x) { restoreAutosave(x, info); });
    a.markUi("start.recovery.restore");
    y += 58 + 12;
  }
  if (!d.recovery.empty()) y += 12;

  // Недавние миры.
  float listTop = y;
  float footer = 44;
  float avail = V.h - footer - listTop;
  if (avail > 90) {
    ui::draw::text("НЕДАВНИЕ МИРЫ", RectF{x0 + 4, listTop, 300, 16}, ui::Font::Caption, th.textMuted);
    listTop += 24;
    avail -= 24;
    float rowH = 64, rowGap = 4;
    float h = std::min(avail - 8, std::max(rowH + 16, float(std::max<size_t>(1, d.recent.size())) * (rowH + rowGap) - rowGap + 16));
    RectF pr{x0, listTop, colW, std::round(h)};
    ui::Panel p("recent", pr, {.pad = 8, .radius = 14, .glass = true});
    a.markUi("start.recent", pr);
    if (d.recent.empty()) {
      ui::draw::icon("map", RectF{pr.x + 24, pr.cy() - 11, 22, 22}, th.textMuted);
      ui::draw::text("Здесь появятся миры, которые вы откроете или создадите.", RectF{pr.x + 58, pr.y, pr.w - 80, pr.h}, ui::Font::Body, th.textMuted);
    } else {
      ui::Scroll sc("list");
      ui::gap(rowGap);
      for (size_t i = 0; i < d.recent.size(); i++) {
        RectF r = ui::next(rowH);
        a.markUi("start.recent." + std::to_string(i), r);
        recentRow(a, d.recent[i], r, int(i));
        if (d.recentDirty) break;
      }
    }
  }
  ui::draw::text("Перетащите папку мира или файл .regnum в окно · F1 — сочетания клавиш", RectF{x0, V.h - footer + 6, colW, 20}, ui::Font::Small,
                 th.textMuted, ui::Align::Center);
  drawToasts(a, RectF{V.w - 380, 60, 356, V.h - 84});
}

}  // namespace rg::app::detail
