// Regnum — хранилище составных тайлов и их фоновая отрисовка в пуле потоков (jobs).
//
// Очередь своя (а не задачи пула на каждый тайл): приоритет меняется каждый кадр, ненужные запросы снимаются.
// Задача пула рисует один тайл и, если очередь не пуста, ставит себя снова — так пул остаётся доступен другим.
#include "base/jobs.h"
#include "map/map_internal.h"

namespace rg::map::detail {

size_t TileKeyHash::operator()(const TileKey& k) const {
  u64 d = 0;
  std::memcpy(&d, &k.ds, 8);
  u64 h = hashMix(k.style, d);
  h = hashMix(h, (u64(u32(k.tx)) << 32) | u32(k.ty));
  return size_t(h);
}

namespace {
struct Entry {
  std::shared_ptr<const gfx::Image> img;
  u64 have = 0, want = 0;   // поколение мира содержимого и с какого поколения устарел
  bool queued = false, running = false;
  double prio = 0;
  u64 used = 0;
  Box2 box;                 // габарит на карте
};

Box2 tileBox(const TileKey& k) {
  return Box2(k.tx * double(kTile) / k.ds, k.ty * double(kTile) / k.ds, (k.tx + 1) * double(kTile) / k.ds, (k.ty + 1) * double(kTile) / k.ds);
}
}  // namespace

struct TileStore::Shared {
  explicit Shared(const Basemap* b, size_t rasterBudget) : bm(b), raster(b, rasterBudget) {}
  mutable std::mutex mu;
  std::condition_variable cv;
  const Basemap* bm;
  RasterCache raster;
  std::unordered_map<TileKey, Entry, TileKeyHash> tiles;
  std::shared_ptr<const TileScene> scene;
  int active = 0, maxActive = 1;
  bool dead = false;
  std::atomic<bool> arrived{false};
  std::function<void()> wake;
  size_t bytes = 0, budget = 0;

  bool hasQueuedLocked() const {
    for (const auto& kv : tiles)
      if (kv.second.queued) return true;
    return false;
  }
};

namespace {

void runJob(const std::shared_ptr<TileStore::Shared>& s);

// Задача пула. Если пул отбросит задачу, не выполнив (остановка программы), счётчик активных всё равно уменьшится.
struct Job {
  std::shared_ptr<TileStore::Shared> s;
  explicit Job(std::shared_ptr<TileStore::Shared> sh) : s(std::move(sh)) {}
  Job(Job&& o) noexcept : s(std::move(o.s)) {}
  Job(const Job&) = delete;
  Job& operator=(const Job&) = delete;
  ~Job() {
    if (!s) return;
    std::lock_guard<std::mutex> lk(s->mu);
    s->active--;
    s->cv.notify_all();
  }
  void operator()() {
    auto sh = std::move(s);  // дальше счётчик ведёт runJob
    runJob(sh);
  }
};

void submitJob(const std::shared_ptr<TileStore::Shared>& s) { jobs::submit(Job(s)); }

void runJob(const std::shared_ptr<TileStore::Shared>& s) {
  TileKey key;
  std::shared_ptr<const TileScene> sc;
  {
    std::lock_guard<std::mutex> lk(s->mu);
    Entry* best = nullptr;
    if (!s->dead && s->scene)
      for (auto& [k, e] : s->tiles) {
        if (!e.queued) continue;
        if (k.style != s->scene->styleKey) { e.queued = false; continue; }
        if (!best || e.prio < best->prio) { best = &e; key = k; }
      }
    if (!best) {
      s->active--;
      s->cv.notify_all();
      return;
    }
    best->queued = false;
    best->running = true;
    sc = s->scene;
  }
  std::shared_ptr<gfx::Image> img;
  try {
    auto g = geoIndex(sc->world);
    img = std::make_shared<gfx::Image>(renderTile(*sc, *g, s->bm, &s->raster, key.ds, key.tx, key.ty));
  } catch (const std::exception& ex) {
    // Ошибка не роняет карту: тайл — цветом суши, повтор — после следующего изменения мира.
    logError("Тайл карты (%d, %d) не нарисован: %s", key.tx, key.ty, ex.what());
    img = std::make_shared<gfx::Image>(kTile, kTile, gfx::premul(s->bm ? s->bm->landColor() : Color(255, 255, 255)));
  }
  std::function<void()> wake;
  {
    std::lock_guard<std::mutex> lk(s->mu);
    auto it = s->tiles.find(key);
    if (it != s->tiles.end()) {
      Entry& e = it->second;
      e.running = false;
      if (e.img) s->bytes -= e.img->px.size() * 4;
      s->bytes += img->px.size() * 4;
      e.img = std::move(img);
      e.have = sc->gen;
    }
    s->arrived = true;
    wake = s->wake;
  }
  // Будить приложение, пока задача числится активной: деструктор хранилища ждёт её завершения.
  if (wake) wake();
  bool again = false;
  {
    std::lock_guard<std::mutex> lk(s->mu);
    again = !s->dead && s->hasQueuedLocked();
    if (!again) s->active--;
    s->cv.notify_all();
  }
  if (again) submitJob(s);
}

}  // namespace

TileStore::TileStore(const Basemap* bm, size_t rasterBudget, size_t tileBudget) : s_(std::make_shared<Shared>(bm, rasterBudget)) {
  s_->budget = tileBudget;
  // Часть пула остаётся свободной для полос кадра (jobs::parallelFor в render).
  s_->maxActive = std::max(1, jobs::workers() * 2 / 3);
}

TileStore::~TileStore() {
  std::unique_lock<std::mutex> lk(s_->mu);
  s_->dead = true;
  for (auto& kv : s_->tiles) kv.second.queued = false;
  s_->cv.wait(lk, [&] { return s_->active == 0; });
}

void TileStore::setScene(std::shared_ptr<const TileScene> sc) {
  std::lock_guard<std::mutex> lk(s_->mu);
  s_->scene = std::move(sc);
}

void TileStore::invalidate(const std::vector<Box2>& dirty, u64 gen, u64 styleKey, bool all) {
  std::lock_guard<std::mutex> lk(s_->mu);
  for (auto& [k, e] : s_->tiles) {
    bool hit = all || k.style != styleKey;
    if (!hit) {
      const Box2 b = e.box.inflated(16.0 / k.ds);
      for (const Box2& d : dirty)
        if (d.intersects(b)) { hit = true; break; }
    }
    if (hit) e.want = std::max(e.want, gen);
  }
}

void TileStore::request(const std::vector<TileRequest>& reqs, u64 used) {
  std::lock_guard<std::mutex> lk(s_->mu);
  if (s_->dead) return;
  for (auto& kv : s_->tiles) kv.second.queued = false;
  for (const TileRequest& r : reqs) {
    auto [it, fresh] = s_->tiles.try_emplace(r.key);
    Entry& e = it->second;
    if (fresh) e.box = tileBox(r.key);
    e.used = std::max(e.used, used);
    if (e.running) continue;
    if (!e.img || e.have < e.want) {
      e.queued = true;
      e.prio = r.prio;
    }
  }
  int queued = 0;
  for (auto& kv : s_->tiles) queued += kv.second.queued;
  while (s_->active < s_->maxActive && s_->active < queued) {
    s_->active++;
    submitJob(s_);
  }
}

void TileStore::available(u64 style, const Box2& box, std::vector<TileView>& out) const {
  out.clear();
  std::lock_guard<std::mutex> lk(s_->mu);
  for (const auto& [k, e] : s_->tiles)
    if (e.img && (style == 0 || k.style == style) && e.box.intersects(box)) out.push_back({k, e.img, e.have < e.want});
}

bool TileStore::get(const TileKey& k, TileView& out) const {
  std::lock_guard<std::mutex> lk(s_->mu);
  auto it = s_->tiles.find(k);
  if (it == s_->tiles.end() || !it->second.img) return false;
  out = {k, it->second.img, it->second.have < it->second.want};
  return true;
}

void TileStore::touch(const std::vector<TileKey>& keys, u64 used) {
  std::lock_guard<std::mutex> lk(s_->mu);
  for (const TileKey& k : keys) {
    auto it = s_->tiles.find(k);
    if (it != s_->tiles.end()) it->second.used = std::max(it->second.used, used);
  }
}

void TileStore::trim(u64 frame, u64 keepStyle) {
  std::lock_guard<std::mutex> lk(s_->mu);
  // Пустые записи без работы — сразу; изображения — по давности использования, пока не уложимся в бюджет.
  for (auto it = s_->tiles.begin(); it != s_->tiles.end();)
    if (!it->second.img && !it->second.queued && !it->second.running) it = s_->tiles.erase(it);
    else ++it;
  if (s_->bytes <= s_->budget) return;
  std::vector<std::pair<u64, TileKey>> order;
  for (const auto& [k, e] : s_->tiles)
    if (e.img && !e.running && e.used < frame && !(k.style == keepStyle && k.ds == kBaseScale)) order.push_back({e.used, k});
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [u, k] : order) {
    if (s_->bytes <= s_->budget) break;
    auto it = s_->tiles.find(k);
    s_->bytes -= it->second.img->px.size() * 4;
    s_->tiles.erase(it);
  }
}

bool TileStore::takeArrived() { return s_->arrived.exchange(false); }
bool TileStore::arrived() const { return s_->arrived.load(); }

bool TileStore::busy() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->active > 0 || s_->hasQueuedLocked();
}

int TileStore::pending() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  int n = 0;
  for (const auto& kv : s_->tiles) n += kv.second.queued || kv.second.running;
  return n;
}

bool TileStore::wait(double timeoutSec) {
  std::unique_lock<std::mutex> lk(s_->mu);
  return s_->cv.wait_for(lk, std::chrono::duration<double>(timeoutSec), [&] { return s_->active == 0 && !s_->hasQueuedLocked(); });
}

void TileStore::setWake(std::function<void()> fn) {
  std::lock_guard<std::mutex> lk(s_->mu);
  s_->wake = std::move(fn);
}

size_t TileStore::bytes() const {
  std::lock_guard<std::mutex> lk(s_->mu);
  return s_->bytes;
}

RasterCache& TileStore::raster() { return s_->raster; }

}  // namespace rg::map::detail
