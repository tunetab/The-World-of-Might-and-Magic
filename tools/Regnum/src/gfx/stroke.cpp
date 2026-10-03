// Regnum — обводка: контур из левой стороны вперёд, конца, правой стороны назад и начала.
//
// Внешняя сторона излома получает соединение (miter/round/bevel). Внутренняя — точку пересечения смещённых
// отрезков, если она лежит в пределах половин соседних отрезков, иначе обход через вершину ломаной.
// Такой контур равен сумме одинаково ориентированных четырёхугольников отрезков и клиньев соединений,
// поэтому заливка NonZero даёт ровно объединение без «дыр».
#include "gfx/stroke.h"

#include "gfx/flatten.h"

namespace rg::gfx {

namespace {

// Предел числа штрихов на вызов (по видимой длине): дальше линия рисуется сплошной — защита от зависания
// при очень частом шаблоне или огромной длине (штрихи мельче пикселя всё равно неразличимы).
constexpr double kMaxDashes = 5.0e4;

inline Pt normalOf(Pt d) { return {-d.y, d.x}; }
inline float cross(Pt a, Pt b) { return a.x * b.y - a.y * b.x; }
inline float dot(Pt a, Pt b) { return a.x * b.x + a.y * b.y; }
inline float dist2(Pt a, Pt b) { float dx = a.x - b.x, dy = a.y - b.y; return dx * dx + dy * dy; }

class Stroker {
 public:
  Stroker(Path& out, const Stroke& s, float tol) : out_(out) {
    hw_ = s.width * 0.5f;
    join_ = s.join;
    cap_ = s.cap;
    miterLimit_ = std::isfinite(s.miterLimit) ? std::max(1.0f, s.miterLimit) : 4.0f;
    tol_ = tol > 0 ? tol : 0.1f;
    // Допуск скруглений относителен радиусу: мелкие концы и соединения остаются круглыми.
    const float arcTol = std::min(tol_, std::max(hw_ * (1.0f / 256), tol_ * 0.05f));
    float c = 1 - arcTol / hw_;
    arcStep_ = c <= 0 ? float(kPi) * 0.5f : std::clamp(2 * std::acos(c), 0.02f, float(kPi) * 0.5f);
    eps2_ = std::max(1e-12f, (tol_ * 1e-3f) * (tol_ * 1e-3f));
  }

  // Ломаная без повторяющихся точек; для замкнутой последняя точка не совпадает с первой.
  void clean(const Pt* p, size_t n, bool closed, std::vector<Pt>& outPts) const {
    outPts.clear();
    for (size_t i = 0; i < n; i++)
      if (outPts.empty() || dist2(p[i], outPts.back()) > eps2_) outPts.push_back(p[i]);
    if (closed)
      while (outPts.size() > 1 && dist2(outPts.back(), outPts.front()) <= eps2_) outPts.pop_back();
  }

  void polyline(const Pt* p, size_t n, bool closed) {
    clean(p, n, closed, pts_);
    if (pts_.empty()) return;
    if (pts_.size() == 1) { drawDot(pts_[0]); return; }
    if (closed && pts_.size() >= 2) strokeClosed();
    else strokeOpen();
  }

  void dashed(const Pt* p, size_t n, bool closed, const std::vector<float>& pattern, float offset, const RectF* cull) {
    std::vector<Pt> src;
    clean(p, n, closed, src);
    if (src.empty()) return;
    bool ok = dashPolyline(src.data(), src.size(), closed, pattern, offset,
                           [&](const Pt* d, size_t m) { polyline(d, m, false); }, cull);
    if (!ok) polyline(src.data(), src.size(), closed);
  }

 private:
  void emit(Pt p) {
    if (!started_) { out_.moveTo(p.x, p.y); started_ = true; }
    else out_.lineTo(p.x, p.y);
  }
  void finish() {
    if (started_) out_.close();
    started_ = false;
  }

  static void segments(const std::vector<Pt>& p, bool closed, std::vector<Pt>& dirs, std::vector<float>& lens) {
    size_t n = p.size(), m = closed ? n : n - 1;
    dirs.resize(m);
    lens.resize(m);
    for (size_t i = 0; i < m; i++) {
      Pt d = p[(i + 1) % n] - p[i];
      float l = std::sqrt(d.x * d.x + d.y * d.y);
      lens[i] = l;
      dirs[i] = l > 0 ? Pt{d.x / l, d.y / l} : Pt{1, 0};
    }
  }

  // Дуга вокруг c от вектора v на угол ang (без концевых точек).
  void arc(Pt c, Pt v, float ang) {
    const float turns = std::fabs(ang) / arcStep_;
    if (!(turns >= 1) || !(turns < 1e4f)) return;  // мелкий или нечисловой угол — без промежуточных точек
    const int n = int(std::ceil(turns));
    if (n < 2) return;
    float s = ang / float(n), cs = std::cos(s), sn = std::sin(s);
    for (int k = 1; k < n; k++) {
      v = {v.x * cs - v.y * sn, v.x * sn + v.y * cs};
      emit(c + v);
    }
  }

  // Переход левой стороны с отрезка d1 на d2 в вершине P.
  void joinAt(Pt P, Pt d1, Pt d2, float len1, float len2, bool forward) {
    const Pt n1 = normalOf(d1), n2 = normalOf(d2);
    const Pt A = P + n1 * hw_, B = P + n2 * hw_;
    const float cr = cross(d1, d2), dt = dot(d1, d2);
    if (std::fabs(cr) < 1e-6f && dt > 0) {
      emit(A);
      if (dist2(A, B) > eps2_) emit(B);
      return;
    }
    const bool inner = (std::fabs(cr) < 1e-6f && dt < 0) ? !forward : cr > 0;
    if (inner) {
      const float den = 1 + dt;
      if (den > 1e-6f) {
        const float t = hw_ * std::fabs(cr) / den;
        if (t <= 0.5f * len1 && t <= 0.5f * len2) { emit(A - d1 * t); return; }
      }
      emit(A);
      emit(P);
      emit(B);
      return;
    }
    emit(A);
    switch (join_) {
      case Join::Miter: {
        const float den = 1 + dt;
        const float cosHalf = std::sqrt(std::max(0.f, den * 0.5f));
        if (den > 1e-6f && cosHalf * miterLimit_ >= 1) emit(A + d1 * (hw_ * std::fabs(cr) / den));
        break;
      }
      case Join::Round: arc(P, n1 * hw_, std::atan2(cr, dt)); break;
      case Join::Bevel: break;
    }
    emit(B);
  }

  // Конец: от P + n·hw к P − n·hw.
  void capEnd(Pt P, Pt d) {
    const Pt n = normalOf(d);
    switch (cap_) {
      case Cap::Butt: emit(P - n * hw_); break;
      case Cap::Square:
        emit(P + n * hw_ + d * hw_);
        emit(P - n * hw_ + d * hw_);
        emit(P - n * hw_);
        break;
      case Cap::Round:
        arc(P, n * hw_, -float(kPi));
        emit(P - n * hw_);
        break;
    }
  }

  void drawDot(Pt P) {
    if (cap_ == Cap::Butt) return;
    const Pt d{1, 0};
    emit(P + normalOf(d) * hw_);
    capEnd(P, d);
    capEnd(P, Pt{-1, 0});
    finish();
  }

  void strokeOpen() {
    const size_t n = pts_.size();
    segments(pts_, false, dirs_, lens_);
    emit(pts_[0] + normalOf(dirs_[0]) * hw_);
    for (size_t i = 1; i + 1 < n; i++) joinAt(pts_[i], dirs_[i - 1], dirs_[i], lens_[i - 1], lens_[i], true);
    emit(pts_[n - 1] + normalOf(dirs_[n - 2]) * hw_);
    capEnd(pts_[n - 1], dirs_[n - 2]);
    rev_.assign(pts_.rbegin(), pts_.rend());
    segments(rev_, false, rdirs_, rlens_);
    for (size_t i = 1; i + 1 < n; i++) joinAt(rev_[i], rdirs_[i - 1], rdirs_[i], rlens_[i - 1], rlens_[i], false);
    emit(rev_[n - 1] + normalOf(rdirs_[n - 2]) * hw_);
    capEnd(rev_[n - 1], rdirs_[n - 2]);
    finish();
  }

  void strokeClosed() {
    const size_t n = pts_.size();
    segments(pts_, true, dirs_, lens_);
    for (size_t i = 0; i < n; i++) {
      size_t pv = (i + n - 1) % n;
      joinAt(pts_[i], dirs_[pv], dirs_[i], lens_[pv], lens_[i], true);
    }
    finish();
    rev_.assign(pts_.rbegin(), pts_.rend());
    segments(rev_, true, rdirs_, rlens_);
    for (size_t i = 0; i < n; i++) {
      size_t pv = (i + n - 1) % n;
      joinAt(rev_[i], rdirs_[pv], rdirs_[i], rlens_[pv], rlens_[i], false);
    }
    finish();
  }

  Path& out_;
  float hw_ = 0.5f, miterLimit_ = 4, tol_ = 0.1f, arcStep_ = 0.5f, eps2_ = 1e-12f;
  Join join_ = Join::Miter;
  Cap cap_ = Cap::Butt;
  bool started_ = false;
  std::vector<Pt> pts_, rev_, dirs_, rdirs_;
  std::vector<float> lens_, rlens_;
};

}  // namespace

double polylineLength(const Pt* p, size_t n, bool closed) {
  if (!p || n < 2) return 0;
  double s = 0;
  size_t m = closed ? n : n - 1;
  for (size_t i = 0; i < m; i++) {
    double dx = double(p[(i + 1) % n].x) - p[i].x, dy = double(p[(i + 1) % n].y) - p[i].y;
    s += std::sqrt(dx * dx + dy * dy);
  }
  return s;
}

bool dashPolyline(const Pt* p, size_t n, bool closed, const std::vector<float>& pattern, float offset,
                  const std::function<void(const Pt*, size_t)>& emit, const RectF* cull) {
  if (!p || n == 0 || pattern.empty()) return false;
  std::vector<double> pat;
  double sum = 0;
  for (float v : pattern) {
    if (!std::isfinite(v) || v < 0) return false;
    pat.push_back(v);
    sum += v;
  }
  if (pat.size() % 2) pat.insert(pat.end(), pat.begin(), pat.end());
  if (pattern.size() % 2) sum *= 2;
  if (!(sum > 0)) return false;
  const size_t M = pat.size();
  const size_t segs = n < 2 ? 0 : closed ? n : n - 1;
  // Видимая часть отрезка [t0, t1] (отсечение Лианга — Барски по области видимости).
  auto visiblePart = [cull](Pt a, Pt b, double& t0, double& t1) {
    t0 = 0;
    t1 = 1;
    if (!cull) return true;
    const double dx = double(b.x) - a.x, dy = double(b.y) - a.y;
    const double ps[4] = {-dx, dx, -dy, dy};
    const double qs[4] = {double(a.x) - cull->x, double(cull->right()) - a.x, double(a.y) - cull->y, double(cull->bottom()) - a.y};
    for (int k = 0; k < 4; k++) {
      if (ps[k] == 0) {
        if (qs[k] < 0) return false;
      } else {
        const double r = qs[k] / ps[k];
        if (ps[k] < 0) t0 = std::max(t0, r);
        else t1 = std::min(t1, r);
      }
    }
    return t0 < t1;
  };
  // Ограничение числа штрихов — по видимой длине.
  double total = 0, shown = 0;
  for (size_t i = 0; i < segs; i++) {
    const Pt a = p[i], b = p[(i + 1) % n];
    const double l = std::hypot(double(b.x) - a.x, double(b.y) - a.y);
    total += l;
    double t0, t1;
    if (visiblePart(a, b, t0, t1)) shown += l * (t1 - t0);
  }
  if (!(shown / sum * double(M / 2) <= kMaxDashes)) return false;

  double off = std::isfinite(offset) ? std::fmod(double(offset), sum) : 0.0;
  if (off < 0) off += sum;
  size_t idx = 0;
  for (size_t guard = 0; guard < 2 * M && off >= pat[idx]; guard++) {
    off -= pat[idx];
    idx = (idx + 1) % M;
  }
  double rem = std::max(0.0, pat[idx] - off);
  bool on = idx % 2 == 0;

  std::vector<Pt> cur, first;
  const bool holdFirst = closed && on;  // первый штрих замкнутой линии может слиться с последним
  bool firstDone = false;
  auto finishDash = [&](std::vector<Pt>& d) {
    if (d.size() < 2) d.push_back(d.back());
    if (holdFirst && !firstDone) { first = d; firstDone = true; }
    else emit(d.data(), d.size());
  };
  // Невидимый участок длины len, заканчивающийся в точке e: текущий штрих обрывается (конец вне области),
  // фаза шаблона сдвигается арифметически.
  auto skip = [&](double len, Pt e) {
    if (on && !cur.empty()) finishDash(cur);
    cur.clear();
    if (len < rem) {
      rem -= len;
    } else {
      double l = std::fmod(len - rem, sum);
      idx = (idx + 1) % M;
      for (size_t guard = 0; guard < 2 * M && l >= pat[idx]; guard++) {
        l -= pat[idx];
        idx = (idx + 1) % M;
      }
      rem = std::max(0.0, pat[idx] - l);
      on = idx % 2 == 0;
    }
    if (on) cur.push_back(e);
  };
  // Видимый участок от a до b: штрихи по шаблону.
  auto walk = [&](Pt a, Pt b) {
    const double dx = double(b.x) - a.x, dy = double(b.y) - a.y;
    const double L = std::sqrt(dx * dx + dy * dy);
    if (!(L > 0)) return;
    double pos = 0;
    while (L - pos > rem) {
      pos += rem;
      const double t = pos / L;
      const Pt q{float(a.x + dx * t), float(a.y + dy * t)};
      if (on) {
        cur.push_back(q);
        finishDash(cur);
        cur.clear();
      } else {
        cur.clear();
        cur.push_back(q);
      }
      on = !on;
      idx = (idx + 1) % M;
      rem = pat[idx];
    }
    rem -= L - pos;
    if (on) cur.push_back(b);
  };
  if (on) cur.push_back(p[0]);
  if (!(total > 0)) {
    if (on) { cur.push_back(p[0]); emit(cur.data(), cur.size()); }
    return true;
  }
  for (size_t i = 0; i < segs; i++) {
    const Pt a = p[i], b = p[(i + 1) % n];
    const double dx = double(b.x) - a.x, dy = double(b.y) - a.y;
    const double L = std::sqrt(dx * dx + dy * dy);
    if (!(L > 0)) continue;
    double t0, t1;
    if (!visiblePart(a, b, t0, t1)) { skip(L, b); continue; }
    if (t0 <= 0 && t1 >= 1) { walk(a, b); continue; }
    const Pt pa = t0 > 0 ? Pt{float(a.x + dx * t0), float(a.y + dy * t0)} : a;
    const Pt pb = t1 < 1 ? Pt{float(a.x + dx * t1), float(a.y + dy * t1)} : b;
    if (t0 > 0) skip(L * t0, pa);
    walk(pa, pb);
    if (t1 < 1) skip(L * (1 - t1), b);
  }
  if (on && !cur.empty()) {
    if (holdFirst && firstDone) {
      cur.insert(cur.end(), first.begin() + 1, first.end());
      firstDone = false;
      first.clear();
    }
    if (cur.size() < 2) cur.push_back(cur.back());
    emit(cur.data(), cur.size());
  }
  if (holdFirst && firstDone) emit(first.data(), first.size());
  return true;
}

namespace {

// Область видимости, расширенная на наибольший вынос обводки (половина толщины × предел miter).
RectF expandCull(const RectF& c, const Stroke& s, float tol) {
  const float m = s.width * 0.5f * std::max(1.0f, std::isfinite(s.miterLimit) ? s.miterLimit : 4.0f) + tol;
  return {c.x - m, c.y - m, c.w + 2 * m, c.h + 2 * m};
}

}  // namespace

void strokePolyline(const Pt* pts, size_t n, bool closed, const Stroke& s, float tol, Path& out, const RectF* cull) {
  if (!pts || n == 0 || !(s.width > 0) || !std::isfinite(s.width)) return;
  Stroker st(out, s, tol);
  const RectF box = cull ? expandCull(*cull, s, tol) : RectF{};
  if (!s.dash.empty()) st.dashed(pts, n, closed, s.dash, s.dashOffset, cull ? &box : nullptr);
  else st.polyline(pts, n, closed);
}

void strokeToPath(const Path& src, const Stroke& s, float tol, Path& out, const RectF* cull) {
  out.clear();
  if (!(s.width > 0) || !std::isfinite(s.width)) return;
  if (!(tol > 0)) tol = 0.1f;
  Stroker st(out, s, tol);
  detail::CullBox box;
  const RectF ext = cull ? expandCull(*cull, s, tol) : RectF{};
  if (cull) box = {ext.x, ext.y, ext.right(), ext.bottom()};
  struct Sink {
    Stroker& st;
    const Stroke& s;
    const RectF* cull;
    std::vector<Pt> pts;
    void move(Pt p) { pts.clear(); pts.push_back(p); }
    void line(Pt p) { pts.push_back(p); }
    void end(bool closed) {
      if (pts.size() >= 2 || (closed && !pts.empty())) {
        if (!s.dash.empty()) st.dashed(pts.data(), pts.size(), closed, s.dash, s.dashOffset, cull);
        else st.polyline(pts.data(), pts.size(), closed);
      }
      pts.clear();
    }
  } sink{st, s, cull ? &ext : nullptr, {}};
  detail::flattenPath(src, Affine{}, tol, sink, box);
}

Path strokeToPath(const Path& src, const Stroke& s, float tol) {
  Path out;
  strokeToPath(src, s, tol, out);
  return out;
}

}  // namespace rg::gfx
