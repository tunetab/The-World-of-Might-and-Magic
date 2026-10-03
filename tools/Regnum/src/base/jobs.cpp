// Regnum — пул потоков (std::thread).
#include "base/jobs.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace rg::jobs {

namespace {

class Pool {
 public:
  explicit Pool(int n) {
    threads_.reserve(size_t(n));
    for (int i = 0; i < n; i++) threads_.emplace_back([this] { loop(); });
  }

  int size() const { return int(threads_.size()); }

  void push(std::unique_ptr<detail::Task> t) {
    {
      std::lock_guard<std::mutex> lk(m_);
      q_.push_back(std::move(t));
    }
    cv_.notify_one();
  }

  // Остановить: текущие задачи завершаются, очередь отбрасывается.
  void stop() {
    std::deque<std::unique_ptr<detail::Task>> dropped;
    {
      std::lock_guard<std::mutex> lk(m_);
      stop_ = true;
      dropped.swap(q_);
    }
    cv_.notify_all();
    for (auto& t : threads_)
      if (t.joinable()) t.join();
    threads_.clear();
    dropped.clear();  // future невыполненных задач получат broken_promise
  }

 private:
  void loop() {
    for (;;) {
      std::unique_ptr<detail::Task> t;
      {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return stop_ || !q_.empty(); });
        if (stop_) return;
        t = std::move(q_.front());
        q_.pop_front();
      }
      try {
        t->run();
      } catch (...) {
        // задачи сами передают исключения (future / parallelFor); здесь только защита потока
      }
    }
  }

  std::vector<std::thread> threads_;
  std::deque<std::unique_ptr<detail::Task>> q_;
  std::mutex m_;
  std::condition_variable cv_;
  bool stop_ = false;
};

// Мьютекс не разрушается при выходе: к пулу могут обратиться деструкторы статических объектов других единиц.
std::mutex& gMuRef() {
  static std::mutex* m = new std::mutex();
  return *m;
}
Pool* gPool = nullptr;
bool gDown = false;

int defaultWorkers() {
  unsigned hc = std::thread::hardware_concurrency();
  return hc > 1 ? int(hc - 1) : 1;
}

// Под gMuRef(): пул создаётся лениво; после shutdown — nullptr.
Pool* poolLocked() {
  if (gDown) return nullptr;
  if (!gPool) gPool = new Pool(defaultWorkers());
  return gPool;
}

// Поставить задачу; false — пул остановлен (задача остаётся у вызывающего).
bool tryPush(std::unique_ptr<detail::Task>& t) {
  std::lock_guard<std::mutex> lk(gMuRef());
  Pool* p = poolLocked();
  if (!p) return false;
  p->push(std::move(t));
  return true;
}

// Остановка пула при завершении программы.
struct ExitGuard {
  ~ExitGuard() { shutdown(); }
} gExitGuard;

// Общее состояние одного parallelRanges.
struct ForState {
  const std::function<void(size_t, size_t)>* fn = nullptr;
  size_t n = 0, grain = 1, chunks = 0;
  std::atomic<size_t> next{0};
  std::atomic<size_t> done{0};
  std::atomic<bool> cancel{false};
  std::mutex m;
  std::condition_variable cv;
  std::exception_ptr err;

  void work() {
    for (;;) {
      size_t c = next.fetch_add(1, std::memory_order_relaxed);
      if (c >= chunks) return;  // fn больше не трогаем: вызывающий мог уже вернуться
      if (!cancel.load(std::memory_order_relaxed)) {
        size_t b = c * grain, e = std::min(n, b + grain);
        try {
          (*fn)(b, e);
        } catch (...) {
          std::lock_guard<std::mutex> lk(m);
          if (!err) err = std::current_exception();
          cancel.store(true, std::memory_order_relaxed);
        }
      }
      if (done.fetch_add(1, std::memory_order_acq_rel) + 1 == chunks) {
        std::lock_guard<std::mutex> lk(m);
        cv.notify_all();
      }
    }
  }
};

}  // namespace

namespace detail {
void enqueue(std::unique_ptr<Task> t) {
  if (!tryPush(t)) t->run();
}
}  // namespace detail

int workers() {
  std::lock_guard<std::mutex> lk(gMuRef());
  Pool* p = poolLocked();
  return p ? p->size() : 0;
}

void shutdown() {
  Pool* p;
  {
    std::lock_guard<std::mutex> lk(gMuRef());
    gDown = true;
    p = gPool;
    gPool = nullptr;
  }
  if (p) {
    p->stop();
    delete p;
  }
}

void parallelRanges(size_t n, const std::function<void(size_t, size_t)>& fn, size_t grain) {
  if (n == 0) return;
  int w = workers();
  if (grain == 0) {
    size_t parts = size_t(w + 1) * 4;
    grain = std::max<size_t>(1, (n + parts - 1) / parts);
  }
  size_t chunks = (n + grain - 1) / grain;
  if (w == 0 || chunks <= 1) {
    fn(0, n);
    return;
  }
  auto st = std::make_shared<ForState>();
  st->fn = &fn;
  st->n = n;
  st->grain = grain;
  st->chunks = chunks;
  size_t helpers = std::min<size_t>(size_t(w), chunks - 1);
  for (size_t i = 0; i < helpers; i++) {
    auto job = [st] { st->work(); };
    std::unique_ptr<detail::Task> t = std::make_unique<detail::FnTask<decltype(job)>>(std::move(job));
    if (!tryPush(t)) break;  // пул остановлен — всё сделает вызывающий поток
  }
  st->work();
  {
    std::unique_lock<std::mutex> lk(st->m);
    st->cv.wait(lk, [&] { return st->done.load(std::memory_order_acquire) == st->chunks; });
  }
  if (st->err) std::rethrow_exception(st->err);
}

void parallelFor(size_t n, const std::function<void(size_t)>& fn, size_t grain) {
  parallelRanges(n, [&fn](size_t b, size_t e) {
    for (size_t i = b; i < e; i++) fn(i);
  }, grain);
}

}  // namespace rg::jobs
