// Regnum — панель битвы (ТЗ 1.c.iv): данные обеих сторон (флаги, полководцы, герои, отряды по фракциям),
// потери по каждому отряду (не больше численности), выбор победителя, «Отступить» — нападавший остаётся на
// исходной позиции, «Применить итог» — rules::resolveBattle: потери вычитаются из отрядов и общей численности,
// проигравший смещается от места боя, объект без отрядов исчезает.
#include "app/app_internal.h"
#include "app/panels/military.h"

namespace rg::app::mil {

namespace {

using Losses = std::map<Id, std::map<std::pair<Id, Id>, i64>>;

i64 lossOf(const Losses& L, Id army, Id faction, Id row) {
  auto a = L.find(army);
  if (a == L.end()) return 0;
  auto r = a->second.find({faction, row});
  return r == a->second.end() ? 0 : r->second;
}

i64 lossTotal(const Losses& L, Id army) {
  i64 n = 0;
  if (auto a = L.find(army); a != L.end())
    for (auto& [k, v] : a->second) n += v;
  return n;
}

struct BattleDialog final : Dialog {
  Id attacker = 0, defender = 0;
  Vec2 origin;
  std::function<void(App&, bool)> done;
  bool decided = false;
  int winner = 0;   // 0 — нападающий, 1 — защитник
  Losses losses;
  float cardsH = 0;
  std::shared_ptr<bool> retreatOk = std::make_shared<bool>(false);   // подтверждено «Отступить без потерь»

  const char* id() const override { return "battle"; }
  Style style(App& a) override {
    // Место боя — провинция под защитником.
    const World& w = frameWorld(a);
    const Army* d = w.army(defender);
    Id pid = d ? provinceUnder(w, d->pos) : 0;
    return {pid ? "Битва · " + w.provinceName(pid) : std::string("Битва"), "battle", ui::Tone::Danger, 880};
  }

  void finish(App& a, bool applied) {
    if (decided) return;
    decided = true;
    if (done) {
      auto fn = done;
      detail::later(a, [fn, applied](App& x) { fn(x, applied); });
    }
  }

  void retreat(App& a) {
    const Army* ar = a.world().army(attacker);
    if (ar) a.toast("«" + objectName(*ar) + "» отступает на исходную позицию", ToastKind::Info, "retreat");
    finish(a, false);
  }

  void dismissed(App& a) override { retreat(a); }

  // Выбор победителя: две половины с флагами (←/→ с клавиатуры). can[i] = false — у стороны после потерь не остаётся
  // отрядов, а у другой остаются: победителем её выбрать нельзя (rules::resolveBattle тоже откажет).
  void winnerPicker(App& a, const World& w, const Army& A, const Army& D, const bool can[2]) {
    const ui::Theme& th = ui::theme();
    RectF r = ui::next(58);
    float half = std::floor((r.w - 10) * 0.5f);
    for (int i = 0; i < 2; i++) {
      const Army& ar = i == 0 ? A : D;
      RectF h{r.x + float(i) * (half + 10), r.y, half, r.h};
      ui::WidgetId wid = ui::id(i == 0 ? "##win-attacker" : "##win-defender");
      ui::Interaction it = ui::interact(wid, h, can[i] ? ui::IfFocusable : ui::IfNone);
      if (it.clicked && can[i]) winner = i;
      if (it.focused) {
        if (ui::keyPressed(platform::Key::Left)) {
          ui::consumeKey(platform::Key::Left);
          if (can[0]) winner = 0;
        }
        if (ui::keyPressed(platform::Key::Right)) {
          ui::consumeKey(platform::Key::Right);
          if (can[1]) winner = 1;
        }
      }
      bool sel = winner == i;
      float hv = ui::animate(wid ^ 1, it.hovered ? 1.f : 0.f);
      float sk = ui::animate(wid ^ 2, sel ? 1.f : 0.f, 0.14f);
      Color bg = Color::mix(th.surface3, th.accent.alpha(th.dark ? 0.16f : 0.14f), sk);
      ui::draw::rect(h, bg, 10);
      if (hv > 0.01f && !sel) ui::draw::rect(h, th.hover.alpha(hv), 10);
      ui::draw::rectStroke(h, Color::mix(th.border, th.accent, sk), 10, 1 + sk * 0.75f);
      if (it.focused) ui::draw::rectStroke(h.expand(3), th.accent.alpha(0.5f), 12, 2);
      ui::at(RectF{h.x + 12, h.cy() - 15, 45, 30});
      factionFlag(w, ar.leader(), 45, 30);
      float x = h.x + 12 + 45 + 12;
      float right = h.right() - 12;
      if (!can[i]) {
        // Сторона без отрядов: вместо «Победа» — причина, почему её не выбрать.
        const char* why = "Нет отрядов";
        float bw = ui::measure(why, ui::Font::Small) + 30;
        RectF b{right - bw, h.cy() - 12, bw, 24};
        ui::draw::rect(b, th.danger.alpha(th.dark ? 0.18f : 0.12f), 12);
        ui::draw::icon("skull", RectF{b.x + 7, b.cy() - 7, 14, 14}, th.danger);
        ui::draw::text(why, RectF{b.x + 24, b.y, bw - 28, b.h}, ui::Font::Small, th.danger);
        right = b.x - 10;
      }
      if (sel) {
        float bw = ui::measure("Победа", ui::Font::Strong) + 30;
        RectF b{right - bw, h.cy() - 12, bw, 24};
        ui::draw::rect(b, th.accent, 12);
        ui::draw::icon("crown", RectF{b.x + 7, b.cy() - 7, 14, 14}, th.onAccent);
        ui::draw::text("Победа", RectF{b.x + 24, b.y, bw - 28, b.h}, ui::Font::Strong, th.onAccent);
        right = b.x - 10;
      }
      float lh = ui::lineHeight(ui::Font::Caption), nh = ui::lineHeight(ui::Font::Strong);
      float y0 = h.cy() - (lh + nh) * 0.5f;
      // Сторона — по фракции (названия объектов часто совпадают: «Войско №1»); объект — в колонке ниже.
      std::string side = w.factionName(ar.leader()) + (ar.allied() ? " и союзники" : "");
      ui::draw::text(i == 0 ? "НАПАДАЮЩИЙ" : "ЗАЩИТНИК", RectF{x, y0, right - x, lh}, ui::Font::Caption, th.textMuted);
      ui::draw::text(side, RectF{x, y0 + lh, right - x, nh}, ui::Font::Strong, sel ? th.text : th.textDim);
      a.markUi(i == 0 ? "battle.winner.attacker" : "battle.winner.defender", h);
      if (!can[i] && it.hovered) ui::tooltip("После потерь у этой стороны не остаётся отрядов — победителем она быть не может");
    }
  }

  // Колонка стороны: фигурка, название, флаги, полководец, герои, отряды с потерями, итог.
  void sideCard(App& a, const World& w, const Army& ar, int role) {
    bool fleet = ar.isFleet();
    bool ro = a.readOnly();
    ui::IdScope s(role == 0 ? "attacker" : "defender");
    {
      ui::Card card({.pad = 12, .tone = winner == role ? ui::Tone::Accent : ui::Tone::Neutral});
      {
        ui::Row hr({ui::px(46), ui::fr(1)}, ui::kAuto, 10);
        figure(w, ar, 46, winner == role);
        ui::Group g(0, 2);
        ui::caption(objectCaption(ar));
        ui::label(objectName(ar), {.font = ui::Font::Title});
        ui::HStack hs(18, ui::Align::Left, 5);
        for (Id f : factionsIn(ar)) {
          ui::IdScope fs{i64(f)};
          factionFlag(w, f, 22, 15);
        }
        ui::label(w.factionName(ar.leader()) + (ar.allied() ? " и союзники" : ""), {.font = ui::Font::Small, .ink = ui::Ink::Dim});
      }
      // Полководец и герои.
      {
        ui::HStack hs(26, ui::Align::Left, 6);
        ui::icon("commander", ui::Ink::Accent, 16, fleet ? "Главный флотоводец" : "Главный полководец");
        if (ar.commander) w::characterChip(ar.commander);
        else ui::label(fleet ? "Без флотоводца" : "Без полководца", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
        int others = 0;
        for (const ArmyGroup& g : ar.groups)
          for (Id h : g.heroes)
            if (h != ar.commander) {
              if (others < 2) w::characterChip(h);
              others++;
            }
        if (others > 2) ui::badge("+" + std::to_string(others - 2), ui::Tone::Neutral);
      }
      // Отряды по фракциям.
      for (const ArmyGroup& g : ar.groups) {
        ui::IdScope gs(i64(g.faction) + 0x73000000LL);
        if (ar.allied()) {
          ui::HStack hs(20, ui::Align::Left, 6);
          factionFlag(w, g.faction, 20, 13);
          ui::label(w.factionName(g.faction), {.font = ui::Font::Small, .ink = ui::Ink::Dim});
        }
        struct L {
          UnitRow row;
          i64 count;
        };
        std::vector<L> lines;
        for (const ArmyUnit& u : g.units)
          if (auto r = unitRow(w, g.faction, u.row, fleet)) lines.push_back(L{*r, u.count});
        ui::Column cols[] = {{fleet ? "Судно" : "Отряд", nullptr, ui::fr(1, 90)},
                             {"Было", nullptr, ui::px(64), ui::Align::Right},
                             {"Потери", "skull", ui::px(108), ui::Align::Left, false, "Не больше численности отряда"},
                             {"Станет", nullptr, ui::px(64), ui::Align::Right}};
        ui::Table t("units", cols, int(lines.size()), {.rowHeight = 38, .selectable = false, .emptyIcon = fleet ? "fleet" : "army",
                                                       .emptyText = fleet ? "Кораблей нет" : "Отрядов нет"});
        for (int i : t) {
          const L& l = lines[size_t(i)];
          RectF cr = t.cell();
          unitCell(cr, l.row);
          t.text(fmtCount(l.count));
          t.cell();
          i64 loss = lossOf(losses, ar.id, g.faction, l.row.id);
          ui::Disabled dis(ro);
          if (ui::numberField("loss", loss, {.min = 0, .max = double(l.count), .tooltip = "Потери: от 0 до " + fmtCount(l.count)})) {
            loss = clamp<i64>(loss, 0, l.count);
            if (loss > 0) losses[ar.id][{g.faction, l.row.id}] = loss;
            else if (auto it = losses.find(ar.id); it != losses.end()) it->second.erase({g.faction, l.row.id});
          }
          a.markUi("battle.loss." + std::to_string(ar.id) + "." + std::to_string(l.row.id));
          i64 after = l.count - loss;
          t.text(fmtCount(after), after == 0 ? ui::Ink::Danger : loss > 0 ? ui::Ink::Warning : ui::Ink::Normal);
        }
      }
      // Итог стороны.
      i64 before = unitCount(ar), lost = lossTotal(losses, ar.id), after = before - lost;
      {
        ui::HStack hs(26, ui::Align::Left, 6);
        ui::label("Итого", {.font = ui::Font::Strong});
        ui::flex();
        ui::label(fmtCount(before), {.ink = ui::Ink::Dim});
        ui::icon("arrow-right", ui::Ink::Muted, 14);
        ui::label(fmtCount(after), {.font = ui::Font::Strong, .ink = after == 0 ? ui::Ink::Danger : ui::Ink::Normal});
        if (lost > 0) ui::tag(fmtSigned(double(-lost)), ui::Tone::Danger);
      }
      if (after == 0) ui::tag(fleet ? "Флот будет потоплен" : "Войско будет уничтожено", ui::Tone::Danger, "skull");
    }
    cardsH = std::max(cardsH, ui::lastItem().rect.h);
  }

  void apply(App& a) {
    const World& w = frameWorld(a);
    const Army* A = w.army(attacker);
    const Army* D = w.army(defender);
    if (!A || !D) return;
    rules::BattleResult r;
    r.attacker = attacker;
    r.defender = defender;
    r.attackerWins = winner == 0;
    r.attackerOrigin = origin;
    for (auto& [aid, rows] : losses)
      for (auto& [k, v] : rows)
        if (v > 0) r.losses[aid][k] = v;
    std::string an = objectName(*A), dn = objectName(*D);
    std::string winName = r.attackerWins ? an : dn;
    if (!a.act("Битва: " + an + " против " + dn, [&](Tx& tx) { rules::resolveBattle(tx, r); })) return;
    Id win = r.attackerWins ? attacker : defender;
    if (a.world().army(win)) a.select(SelType::Army, win);
    else a.clearSelection();
    a.toast("Победа: " + winName, ToastKind::Success, "battle");
    finish(a, true);
  }

  bool draw(App& a) override {
    const World& w = frameWorld(a);
    const Army* A = w.army(attacker);
    const Army* D = w.army(defender);
    if (!A || !D) {
      finish(a, false);
      return false;
    }
    if (*retreatOk) {   // «Отступить без потерь» подтверждено в окне вопроса
      retreat(a);
      return false;
    }
    // Кто может победить: сторона, у которой после потерь остаются отряды (если не остаётся ни у кого — любая,
    // исчезнут обе).
    const i64 leftA = unitCount(*A) - lossTotal(losses, attacker), leftD = unitCount(*D) - lossTotal(losses, defender);
    const bool can[2] = {leftA > 0 || leftD <= 0, leftD > 0 || leftA <= 0};
    if (!can[winner]) winner = 1 - winner;
    winnerPicker(a, w, *A, *D, can);
    ui::spacer(2);
    float maxH = std::max(160.f, ui::viewport().h - 330);   // окно 700 точек: карточки сторон целиком
    float h = std::min(cardsH > 0 ? cardsH + 2 : 360.f, maxH);
    {
      ui::Scroll sc("sides", h);
      ui::Row cols({ui::fr(1), ui::fr(1)}, ui::kAuto, 14);
      float keep = cardsH;
      cardsH = 0;
      {
        ui::Group g(0, 0);
        sideCard(a, w, *A, 0);
      }
      {
        ui::Group g(0, 0);
        sideCard(a, w, *D, 1);
      }
      if (cardsH <= 0) cardsH = keep;
    }
    {
      const Army& loser = winner == 0 ? *D : *A;
      ui::label("Победитель встанет на месте боя, «" + objectName(loser) + "» отступит. Объект без отрядов исчезнет, его герои останутся у фракции.",
                {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "info", .wrap = true});
    }
    // Введённые потери «Отступить» не учитывает — предупреждаем заранее и переспрашиваем.
    const i64 entered = lossTotal(losses, attacker) + lossTotal(losses, defender);
    if (entered > 0) {
      ui::label("Введены потери (" + fmtCount(entered) + "): «Отступить» их не учтёт — нападающий вернётся на исходную позицию без потерь. "
                "Чтобы списать потери, выберите победителя и нажмите «Применить итог».",
                {.font = ui::Font::Small, .ink = ui::Ink::Warning, .icon = "warning", .wrap = true});
      a.markUi("battle.retreatNote");
    }
    ui::ModalFooter f;
    if (ui::button("Отступить", {.icon = "retreat", .tooltip = entered > 0 ? "Нападающий вернётся на исходную позицию; введённые потери не будут учтены"
                                                                          : "Нападающий вернётся на исходную позицию без потерь"})) {
      if (entered == 0) {
        retreat(a);
        return false;
      }
      auto ok = retreatOk;
      a.confirm("Отступить без потерь?",
                "Введённые потери (" + fmtCount(entered) + ") не будут учтены: нападающий вернётся на исходную позицию, численность обеих сторон "
                "не изменится. Чтобы списать потери, выберите победителя и нажмите «Применить итог».",
                "Отступить без потерь", false, [ok](App&) { *ok = true; });
    }
    a.markUi("battle.retreat");
    {
      ui::Disabled dis(a.readOnly());
      if (ui::button("Применить итог", {.variant = ui::Variant::Primary, .icon = "check"})) {
        apply(a);
        if (decided) return false;
      }
      a.markUi("battle.apply");
    }
    return true;
  }
};

// Битва из реестра диалогов: arg — нападающий, защитник — вражеский объект вплотную к нему.
std::unique_ptr<Dialog> makeRegistered(App& a, Id arg) {
  const World& w = frameWorld(a);
  const Army* ar = w.army(arg);
  if (!ar) fail("Войско не найдено");
  rules::Encounter e = rules::encounter(w, arg, ar->pos);
  if (e.type != rules::EncounterType::Battle || !e.target) fail("Рядом нет войска противника, с которым идёт война");
  auto d = std::make_unique<BattleDialog>();
  d->attacker = arg;
  d->defender = e.target;
  d->origin = ar->pos;
  return d;
}

DialogReg reg({"battle", makeRegistered});

}  // namespace

void openBattle(App& a, Id attacker, Id defender, Vec2 origin, std::function<void(App&, bool)> done) {
  auto d = std::make_unique<BattleDialog>();
  d->attacker = attacker;
  d->defender = defender;
  d->origin = origin;
  d->done = std::move(done);
  a.openDialog(std::move(d));
}

}  // namespace rg::app::mil
