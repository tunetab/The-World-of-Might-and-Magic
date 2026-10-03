// Regnum — редактор «Торговля» (EditorReg «trade»): составление сделки между любыми государствами и гильдиями —
// позиции каждой стороны (ресурс или золото, количество, «разово» или «каждый ход» на N ходов), подарок — позиции
// одной стороны; живая проверка rules::validateDeal со списком причин; заключение rules::concludeDeal. Справа —
// список сделок, дани и репараций (активные и завершённые) с остатком ходов и расторжением.
// ТЗ 1.b.vii (вкладка торговли, безвозмездно, доход за ход на срок), 1.d.iii (гильдии торгуют), 1.e.ii (дань).
#include "app/editors/trade.h"

#include "app/app_internal.h"
#include "app/dialogs/turn_ui.h"
#include "app/widgets.h"

namespace rg::app::trade {

// ================================================================ черновик
Draft& draft() {
  static Draft d;
  static u64 tag = 0;
  if (hasApp()) {
    u64 t = turnui::sessionTag(app());   // другой мир — чистый черновик
    if (t != tag) {
      tag = t;
      d = Draft{};
    }
  }
  return d;
}

void startDraft(Id a, Id b) {
  Draft& d = draft();
  d.a = a;
  d.b = b == a ? 0 : b;
  d.items.clear();
}

DraftItem& addItem(DealSide from) {
  Draft& d = draft();
  DraftItem it;
  it.key = d.nextKey++;
  it.from = from;
  d.items.push_back(it);
  return d.items.back();
}

Deal toDeal(const Draft& d) {
  Deal deal;
  deal.kind = DealKind::Trade;
  deal.a = d.a;
  deal.b = d.b;
  for (const DraftItem& it : d.items) {
    DealItem di;
    di.from = it.from;
    di.res = it.res;
    di.amount = it.amount;
    di.mode = it.mode;
    di.turns = it.mode == DealMode::PerTurn ? std::max(1, it.turns) : 1;
    di.left = di.mode == DealMode::PerTurn ? di.turns : 0;
    deal.items.push_back(di);
  }
  return deal;
}

// ================================================================ сделки
const char* dealIcon(DealKind k) {
  switch (k) {
    case DealKind::Tribute: return "tribute";
    case DealKind::Reparations: return "reparations";
    default: return "trade";
  }
}

ui::Tone dealTone(const Deal& d) {
  if (d.status == DealStatus::Cancelled) return ui::Tone::Danger;
  if (d.status == DealStatus::Done) return ui::Tone::Neutral;
  switch (d.kind) {
    case DealKind::Tribute: return ui::Tone::Warning;
    case DealKind::Reparations: return ui::Tone::Danger;
    default: return ui::Tone::Accent;
  }
}

int dealLeft(const Deal& d) {
  int left = 0;
  for (const DealItem& it : d.items)
    if (it.mode == DealMode::PerTurn) left = std::max(left, it.left);
  return left;
}

int dealTurns(const Deal& d) {
  int n = 0;
  for (const DealItem& it : d.items)
    if (it.mode == DealMode::PerTurn) n = std::max(n, it.turns);
  return n;
}

std::string resName(const World& w, Id res) {
  const CatalogItem* c = w.resource(res);
  return c ? (c->name.empty() ? std::string("Ресурс") : c->name) : std::string(res == kGold ? "Золото" : "Ресурс");
}

std::vector<const Deal*> dealsOf(const World& w, Id faction) {
  std::vector<const Deal*> v;
  w.deals.each([&](const Deal& d) {
    if (!faction || d.a == faction || d.b == faction) v.push_back(&d);
  });
  std::stable_sort(v.begin(), v.end(), [](const Deal* x, const Deal* y) {
    bool ax = x->status == DealStatus::Active, ay = y->status == DealStatus::Active;
    if (ax != ay) return ax;
    return x->id > y->id;
  });
  return v;
}

namespace {

std::string kindTitle(const Deal& d) {
  switch (d.kind) {
    case DealKind::Tribute: return "Дань";
    case DealKind::Reparations: return "Репарации";
    default: break;
  }
  bool a = false, b = false;
  for (const DealItem& it : d.items) (it.from == DealSide::A ? a : b) = true;
  return a != b ? "Подарок" : "Обмен";
}

void statusTag(const Deal& d) {
  switch (d.status) {
    case DealStatus::Active: {
      int left = dealLeft(d);
      ui::tag(left > 0 ? "ещё " + nTurns(left) : std::string("активна"), ui::Tone::Success, "repeat");
      break;
    }
    case DealStatus::Done: ui::tag("выполнена", ui::Tone::Neutral, "check"); break;
    case DealStatus::Cancelled: ui::tag("расторгнута", ui::Tone::Danger, "close"); break;
  }
}

}  // namespace

bool dealCard(App& a, const World& w, const Deal& d, const CardOpt& o) {
  const ui::Theme& th = ui::theme();
  ui::IdScope s{i64(d.id)};
  bool active = d.status == DealStatus::Active;
  bool cancel = false;
  {
    ui::Card c({.pad = 12, .tone = dealTone(d)});
    {
      ui::HStack hs(28, ui::Align::Left, 8);
      RectF tr = ui::next(26, 28);
      turnui::iconTile(RectF{tr.x, tr.cy() - 13, 26, 26}, dealIcon(d.kind), dealTone(d));
      ui::label(kindTitle(d), {.font = ui::Font::Strong});
      statusTag(d);
      ui::flex();
      if (active) {
        std::string tip = d.kind == DealKind::Trade ? "Расторгнуть сделку" : d.kind == DealKind::Tribute ? "Отменить дань" : "Отменить репарации";
        if (ui::iconButton("trash", tip, {.size = ui::Size::Small, .disabled = a.readOnly(), .tone = ui::Tone::Danger})) cancel = true;
        a.markUi("trade.cancel." + std::to_string(d.id));
      }
    }
    // Стороны: для дани a — получатель, b — плательщик. Узкая карточка — в две строки.
    if (o.perspective && (d.a == o.perspective || d.b == o.perspective)) {
      ui::HStack hs(26, ui::Align::Left, 6);
      Id other = d.a == o.perspective ? d.b : d.a;
      const char* rel = d.kind == DealKind::Trade ? "с" : (d.b == o.perspective ? "платим" : "платит");
      ui::label(rel, {.font = ui::Font::Small, .ink = ui::Ink::Muted});
      w::factionChip(other, true);
    } else {
      Id first = d.kind == DealKind::Trade ? d.a : d.b, second = d.kind == DealKind::Trade ? d.b : d.a;
      const char* mid = d.kind == DealKind::Trade ? "trade" : "arrow-right";
      auto chipW = [&](Id f) { return ui::measure(w.factionName(f), ui::Font::Small) + 60; };
      bool wide = chipW(first) + chipW(second) + 32 <= ui::avail().w;
      if (wide) {
        ui::HStack hs(26, ui::Align::Left, 6);
        w::factionChip(first, true);
        ui::icon(mid, ui::Ink::Muted, 14, d.kind == DealKind::Trade ? "обмен" : "платит");
        w::factionChip(second, true);
      } else {
        {
          ui::HStack hs(26, ui::Align::Left, 6);
          w::factionChip(first, true);
        }
        ui::HStack hs(26, ui::Align::Left, 6);
        ui::icon(mid, ui::Ink::Muted, 14, d.kind == DealKind::Trade ? "обмен" : "платит");
        w::factionChip(second, true);
      }
    }
    // Позиции
    for (size_t i = 0; i < d.items.size(); i++) {
      const DealItem& it = d.items[i];
      ui::IdScope is{int(i)};
      Id payer = it.from == DealSide::A ? d.a : d.b;
      ui::HStack hs(22, ui::Align::Left, 6);
      if (o.perspective && (d.a == o.perspective || d.b == o.perspective)) {
        bool give = payer == o.perspective;
        ui::icon(give ? "arrow-up" : "arrow-down", give ? ui::Ink::Danger : ui::Ink::Success, 14, give ? "Отдаём" : "Получаем");
      } else {
        RectF dr = ui::next(10, 22);
        ui::draw::circle(dr.cx(), dr.cy(), 4, w::factionColor(w, payer));
      }
      ui::iconColored(w::resourceIcon(w, it.res), w::resourceColor(w, it.res), 16);
      ui::label(turnui::money(it.amount), {.font = ui::Font::Strong});
      ui::label(resName(w, it.res), {.ink = ui::Ink::Dim});
      ui::flex();
      std::string mode;
      if (it.mode == DealMode::Once) mode = "разово";
      else if (active) mode = "за ход · осталось " + std::to_string(std::max(0, it.left)) + " из " + std::to_string(it.turns);
      else mode = "за ход · " + nTurns(it.turns);
      ui::label(mode, {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = it.mode == DealMode::Once ? "bolt" : "repeat"});
    }
    int total = dealTurns(d);
    if (active && total > 0) {
      double done = double(total - dealLeft(d)) / double(total);
      ui::progress(done, {.tone = ui::Tone::Accent, .height = 4});
    }
    std::string foot = "Заключена на ходу " + std::to_string(d.turn);
    if (!d.note.empty()) foot += " · " + d.note;
    ui::label(foot, {.font = ui::Font::Caption, .ink = ui::Ink::Muted});
  }
  RectF cr = ui::lastItem().rect;
  bool clicked = false;
  if (o.clickable || o.highlight) {
    ui::WidgetId wid = ui::id("##card");
    float hv = 0;
    if (o.clickable) {
      ui::Interaction it = ui::interact(wid, cr, ui::IfAllowOverlap);
      hv = ui::animate(wid ^ 0xd3a1ull, it.hovered ? 1.f : 0.f);
      if (it.hovered) ui::setCursor(platform::Cursor::Hand);
      clicked = it.clicked;
    }
    float k = o.highlight ? 1.f : hv;
    if (k > 0.01f) ui::draw::rectStroke(cr, th.accent.alpha(0.65f * k), th.radiusCard, 1.5f);
  }
  if (cancel) askCancel(a, d.id);
  return clicked;
}

void askCancel(App& a, Id deal) {
  const Deal* d = a.world().deal(deal);
  if (!d) return;
  std::string title = d->kind == DealKind::Trade ? "Расторгнуть сделку?" : d->kind == DealKind::Tribute ? "Отменить дань?" : "Отменить репарации?";
  std::string text = "Выплаты «каждый ход» прекратятся; уже переданное не вернётся. Отменить можно Ctrl+Z.";
  a.confirm(title, text, d->kind == DealKind::Trade ? "Расторгнуть" : "Отменить выплаты", true, [deal](App& x) {
    if (x.act("Расторгнуть сделку", [&](Tx& tx) { rules::cancelDeal(tx, deal); })) x.toast("Сделка расторгнута", ToastKind::Info, "trade");
  });
}

// ================================================================ редактор
namespace {

struct ListState {
  int status = 0;   // 0 — активные, 1 — завершённые, 2 — все
  int kind = 0;     // 0 — все, 1 — торговля, 2 — дань и репарации
  Id faction = 0;
};
ListState& listState() {
  static ListState s;
  static u64 tag = 0;
  if (hasApp()) {
    u64 t = turnui::sessionTag(app());
    if (t != tag) {
      tag = t;
      s = ListState{};
    }
  }
  return s;
}

// Выбор ресурса: точка цвета, запас плательщика справа, поиск.
bool resourceCombo(std::string_view id, Id& res, Id payer) {
  const World& w = app().world();
  const auto& list = w.catalogs->resources;
  const Faction* f = w.faction(payer);
  std::vector<std::string> hints;
  hints.reserve(list.size());
  for (auto& c : list) hints.push_back(f ? "есть " + turnui::money(f->stock(c.id)) : std::string());
  std::vector<ui::Option> opts;
  opts.reserve(list.size());
  int idx = -1;
  for (size_t i = 0; i < list.size(); i++) {
    ui::Option o;
    o.label = list[i].name;
    o.color = list[i].color;
    o.hint = hints[i];
    opts.push_back(o);
    if (list[i].id == res) idx = int(i);
  }
  ui::ComboOpt co;
  co.search = 1;
  co.icon = "resource";
  co.placeholder = "Ресурс";
  co.popupWidth = 240;
  if (!ui::combo(id, idx, std::span<const ui::Option>(opts), co)) return false;
  if (idx < 0 || idx >= int(list.size()) || list[size_t(idx)].id == res) return false;
  res = list[size_t(idx)].id;
  return true;
}

// Запасы стороны строкой: казна и до трёх ресурсов.
void stocks(const World& w, Id fid) {
  const Faction* f = w.faction(fid);
  ui::HStack hs(20, ui::Align::Left, 6);
  if (!f) {
    ui::label("Государство или гильдия", {.font = ui::Font::Small, .ink = ui::Ink::Muted});
    return;
  }
  std::vector<std::pair<Id, double>> rs;
  for (auto& [r, v] : f->res)
    if (r != kGold && v > 0) rs.push_back({r, v});
  std::sort(rs.begin(), rs.end(), [](auto& x, auto& y) { return x.second > y.second; });
  if (rs.size() > 3) rs.resize(3);
  rs.insert(rs.begin(), std::make_pair(Id(kGold), f->treasury()));
  for (size_t i = 0; i < rs.size(); i++) {
    ui::IdScope s{i64(rs[i].first)};
    if (i) ui::spacer(8);
    ui::iconColored(w::resourceIcon(w, rs[i].first), w::resourceColor(w, rs[i].first), 16, resName(w, rs[i].first));
    ui::label(turnui::money(rs[i].second), {.font = ui::Font::Small, .ink = rs[i].second < 0 ? ui::Ink::Danger : ui::Ink::Dim});
  }
}

// Позиция черновика. true — удалить.
bool itemEditor(App& a, const World& w, DraftItem& it, Id payer) {
  ui::IdScope s{i64(it.key)};
  std::string key = "trade.item." + std::to_string(it.key);
  bool remove = false;
  ui::Card c({.pad = 8});
  {
    ui::Row r({ui::fr(1), ui::px(112), ui::px(28)}, 30, 6);
    resourceCombo("res", it.res, payer);
    a.markUi(key + ".res");
    ui::numberField("amount", it.amount, {.min = 0, .max = 1e12, .step = 10, .digits = 1, .tooltip = "Количество"});
    a.markUi(key + ".amount");
    if (ui::iconButton("close", "Убрать позицию", {.size = ui::Size::Small})) remove = true;
    a.markUi(key + ".remove");
  }
  const Faction* f = w.faction(payer);
  double have = f ? f->stock(it.res) : 0;
  {
    ui::Row r({ui::fr(1), ui::px(112)}, 28, 6);
    int mode = int(it.mode);
    if (ui::segmented("mode", mode, {{"bolt", "Разово", "Передать сразу при заключении"}, {"repeat", "Каждый ход", "Передавать каждый ход указанный срок"}},
                      {.size = ui::Size::Small}))
      it.mode = DealMode(mode);
    a.markUi(key + ".mode");
    if (it.mode == DealMode::PerTurn) {
      ui::numberField("turns", it.turns, {.min = 1, .max = 999, .unit = "ход|хода|ходов", .tooltip = "Срок выплат"});
      a.markUi(key + ".turns");
    } else {
      bool short_ = f && have + 1e-9 < it.amount;
      ui::label(f ? (short_ ? "есть " + turnui::money(std::max(0.0, have)) : "есть " + turnui::money(have)) : std::string("—"),
                {.font = ui::Font::Small, .ink = short_ ? ui::Ink::Danger : ui::Ink::Muted, .align = ui::Align::Right});
    }
  }
  if (it.mode == DealMode::PerTurn)
    ui::label("Всего " + turnui::money(it.amount * it.turns) + " за " + nTurns(it.turns) + (f ? " · есть " + turnui::money(have) : std::string()),
              {.font = ui::Font::Small, .ink = ui::Ink::Muted, .icon = "repeat"});
  return remove;
}

void partyColumn(App& a, const World& w, Draft& d, DealSide side) {
  bool isA = side == DealSide::A;
  ui::IdScope scope(isA ? "side.a" : "side.b");
  Id& party = isA ? d.a : d.b;
  Id other = isA ? d.b : d.a;
  ui::Group g(0, 8);
  ui::caption(isA ? "Сторона А" : "Сторона Б");
  if (w::factionPicker(isA ? "party.a" : "party.b", party, w::FactionFilter::Any, "Выберите фракцию", other)) d.highlight = 0;
  a.markUi(isA ? "trade.party.a" : "trade.party.b");
  stocks(w, party);
  ui::spacer(2);
  int n = 0, otherN = 0;
  for (auto& it : d.items) (it.from == side ? n : otherN)++;
  {
    ui::HStack hs(24, ui::Align::Left, 8);
    const Faction* f = w.faction(party);
    if (f) {
      RectF dr = ui::next(10, 24);
      ui::draw::circle(dr.cx(), dr.cy(), 4.5f, f->color);
    }
    ui::label(isA ? "А передаёт" : "Б передаёт", {.font = ui::Font::Strong});
    ui::flex();
    if (n) ui::badge(std::to_string(n), ui::Tone::Neutral);
  }
  for (size_t i = 0; i < d.items.size();) {
    DraftItem& it = d.items[i];
    if (it.from != side) {
      i++;
      continue;
    }
    if (itemEditor(a, w, it, party)) d.items.erase(d.items.begin() + long(i));
    else i++;
  }
  if (!n) {
    const ui::Theme& th = ui::theme();
    RectF r = ui::next(46);
    ui::draw::rectStroke(r, th.border, th.radiusCard, 1);
    ui::draw::text(otherN ? "Ничего — это подарок" : "Нет позиций", r, ui::Font::Small, th.textMuted, ui::Align::Center);
  }
  if (ui::button("Позиция", {.variant = ui::Variant::Secondary, .icon = "plus", .size = ui::Size::Small, .fill = true,
                             .tooltip = isA ? "Что передаёт сторона А" : "Что передаёт сторона Б"})) {
    DraftItem& it = addItem(side);
    it.amount = 100;
  }
  a.markUi(isA ? "trade.add.a" : "trade.add.b");
}

void composer(App& a, const World& w) {
  Draft& d = draft();
  ui::Disabled dis(a.readOnly());
  {
    ui::HStack hs(30, ui::Align::Left, 8);
    ui::icon("handshake", ui::Ink::Accent, 20);
    ui::label("Новая сделка", {.font = ui::Font::Title});
    ui::flex();
    if (ui::button("Очистить", {.variant = ui::Variant::Ghost, .icon = "close", .size = ui::Size::Small, .disabled = d.items.empty() && !d.a && !d.b}))
      startDraft(0, 0);
    a.markUi("trade.clear");
  }
  ui::label("Обмен ресурсами и золотом между государствами и гильдиями: разово или каждый ход на срок. Подарок — позиции только одной стороны.",
            {.font = ui::Font::Small, .ink = ui::Ink::Muted, .wrap = true});
  {
    ui::Row r({ui::fr(1), ui::px(32), ui::fr(1)}, ui::kAuto, 12);
    partyColumn(a, w, d, DealSide::A);
    {
      ui::Group g(0, 0);
      ui::spacer(24);
      if (ui::iconButton("trade", "Поменять стороны местами")) {
        std::swap(d.a, d.b);
        for (auto& it : d.items) it.from = it.from == DealSide::A ? DealSide::B : DealSide::A;
      }
      a.markUi("trade.swap");
    }
    partyColumn(a, w, d, DealSide::B);
  }
  ui::spacer(4);
  Deal deal = toDeal(d);
  bool ready = d.a && d.b && !d.items.empty();
  rules::DealCheck chk = ready ? rules::validateDeal(w, deal) : rules::DealCheck{false, {}};
  bool hasA = false, hasB = false;
  for (auto& it : d.items) (it.from == DealSide::A ? hasA : hasB) = true;
  bool gift = hasA != hasB;
  if (!ready) {
    ui::Card c({.pad = 12, .icon = "info", .title = "Выберите стороны и добавьте позиции", .tone = ui::Tone::Info});
    ui::label("Сделка исполняется сразу (разовые позиции) или каждый ход до конца срока.", {.font = ui::Font::Small, .ink = ui::Ink::Dim, .wrap = true});
  } else if (chk.ok) {
    ui::Card c({.pad = 12, .icon = gift ? "star" : "check-circle", .title = gift ? "Подарок можно передать" : "Сделку можно заключить", .tone = ui::Tone::Success});
    for (int sd = 0; sd < 2; sd++) {
      DealSide side = DealSide(sd);
      std::vector<std::string> parts;
      for (auto& it : d.items)
        if (it.from == side)
          parts.push_back(resName(w, it.res) + " " + turnui::money(it.amount) + (it.mode == DealMode::PerTurn ? " за ход, " + nTurns(it.turns) : std::string(" разово")));
      if (parts.empty()) continue;
      Id to = side == DealSide::A ? d.b : d.a;
      ui::label(w.factionName(to) + " получает: " + join(parts, "; "), {.font = ui::Font::Small, .ink = ui::Ink::Dim, .icon = "arrow-down", .wrap = true});
    }
  } else {
    ui::Card c({.pad = 12, .icon = "warning", .title = "Нужно поправить", .tone = ui::Tone::Warning});
    for (size_t i = 0; i < chk.problems.size(); i++) {
      ui::IdScope s{int(i)};
      ui::label(chk.problems[i], {.font = ui::Font::Small, .ink = ui::Ink::Warning, .icon = "warning", .wrap = true});
    }
  }
  a.markUi("trade.check");
  {
    ui::HStack hs(32, ui::Align::Left, 8);
    if (ui::button("Дань или репарации", {.icon = "tribute", .tooltip = "Навязать выплаты золотом на срок"})) a.openDialog("tribute", d.a);
    a.markUi("trade.tribute");
    ui::flex();
    if (ui::button(gift ? "Подарить" : "Заключить сделку", {.variant = ui::Variant::Primary, .icon = gift ? "star" : "handshake", .disabled = !chk.ok})) {
      Id nid = 0;
      if (a.act(gift ? "Подарок" : "Торговая сделка", [&](Tx& tx) { nid = rules::concludeDeal(tx, deal); })) {
        const Deal* nd = a.store.world().deal(nid);
        bool perTurn = nd && nd->status == DealStatus::Active;
        a.toast(perTurn ? "Сделка заключена: выплаты каждый ход" : gift ? "Подарок передан" : "Обмен выполнен", ToastKind::Success, "handshake");
        d.items.clear();
        d.highlight = nid;
        listState().status = perTurn ? 0 : 1;
      }
    }
    a.markUi("trade.conclude");
  }
}

void dealsList(App& a, const World& w) {
  ListState& ls = listState();
  Draft& d = draft();
  int active = 0, tributes = 0;
  double goldPerTurn = 0;
  w.deals.each([&](const Deal& x) {
    if (x.status != DealStatus::Active) return;
    active++;
    if (x.kind != DealKind::Trade) tributes++;
    for (auto& it : x.items)
      if (it.mode == DealMode::PerTurn && it.res == kGold && it.left > 0) goldPerTurn += it.amount;
  });
  {
    ui::Row r({ui::fr(1), ui::fr(1), ui::fr(1)}, 64, 10);
    ui::stat(std::to_string(active), "Активных", {.icon = "handshake", .tone = ui::Tone::Accent});
    ui::stat(std::to_string(tributes), "Дань и репарации", {.icon = "tribute", .tone = ui::Tone::Warning});
    ui::stat(fmtNum(goldPerTurn), "Золота за ход", {.icon = "coins", .tone = ui::Tone::Success, .tooltip = "Сумма выплат золотом по активным сделкам"});
  }
  {
    ui::HStack hs(30, ui::Align::Left, 8);
    ui::segmented("status", ls.status, {{nullptr, "Активные"}, {nullptr, "Завершённые"}, {nullptr, "Все"}}, {.size = ui::Size::Small, .fill = false});
    a.markUi("trade.list.status");
    ui::segmented("kind", ls.kind, {{"list", {}, "Все виды"}, {"trade", {}, "Торговля"}, {"tribute", {}, "Дань и репарации"}},
                  {.size = ui::Size::Small, .fill = false});
    a.markUi("trade.list.kind");
    ui::flex();
    w::factionPicker("lfac", ls.faction, w::FactionFilter::Any, "Все фракции");
  }
  std::vector<const Deal*> list;
  for (const Deal* x : dealsOf(w, ls.faction)) {
    bool act = x->status == DealStatus::Active;
    if (ls.status == 0 && !act) continue;
    if (ls.status == 1 && act) continue;
    if (ls.kind == 1 && x->kind != DealKind::Trade) continue;
    if (ls.kind == 2 && x->kind == DealKind::Trade) continue;
    list.push_back(x);
  }
  ui::Scroll sc("deals");
  if (list.empty()) {
    ui::spacer(24);
    ui::emptyState("handshake", ls.status == 0 ? "Активных сделок нет." : "Сделок нет.");
    return;
  }
  for (const Deal* x : list)
    if (dealCard(a, w, *x, {.clickable = true, .highlight = d.highlight == x->id})) d.highlight = d.highlight == x->id ? 0 : x->id;
}

void drawEditor(App& a, Id arg) {
  // Открытие с фракцией (вкладка фракции, палитра): сторона А заполняется один раз за открытие.
  static u64 lastFrame = 0;
  static Id seenArg = 0;
  bool fresh = a.frameCount() > lastFrame + 2;
  lastFrame = a.frameCount();
  if (fresh || arg != seenArg) {
    seenArg = arg;
    Draft& d = draft();
    if (arg && d.a != arg && d.b != arg) startDraft(arg, 0);
  }
  const World& w = a.world();
  RectF all = ui::avail();
  const float gap = 24;
  float lw = std::round(clamp((all.w - gap) * 0.55f, std::min(520.f, all.w), all.w));
  bool stacked = all.w < 980;
  if (stacked) lw = all.w;
  RectF L{all.x, all.y, lw, stacked ? all.h * 0.6f : all.h};
  RectF R = stacked ? RectF{all.x, L.bottom() + gap, all.w, all.bottom() - L.bottom() - gap} : RectF{L.right() + gap, all.y, all.right() - L.right() - gap, all.h};
  {
    ui::Area ar(L, 0);
    ui::Scroll sc("compose");
    composer(a, w);
  }
  const ui::Theme& th = ui::theme();
  if (!stacked) ui::draw::line(L.right() + gap * 0.5f, all.y, L.right() + gap * 0.5f, all.bottom(), th.border, 1);
  {
    ui::Area ar(R, 0);
    ui::IdScope s("list");
    dealsList(a, w);
  }
}

EditorReg editor({"trade", "Торговля", drawEditor, "trade"});

bool editorScreen(App& a) { return a.ui.screen == Screen::Editor; }
CommandReg cmdTrade({"trade.open", "Торговля: сделки и подарки", "trade", nullptr, [](App& a) { a.openEditor("trade"); }, editorScreen, false, "Торговля"});
CommandReg cmdTribute({"trade.tribute", "Навязать дань или репарации…", "tribute", nullptr, [](App& a) { a.openDialog("tribute"); },
                       [](App& a) { return editorScreen(a) && !a.readOnly(); }, false, "Торговля"});

}  // namespace
}  // namespace rg::app::trade
