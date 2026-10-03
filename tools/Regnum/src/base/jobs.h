// Regnum — пул потоков.
//
//   jobs::parallelFor(rows, [&](size_t y) { ... });          // блокирует, пока все i не обработаны
//   auto f = jobs::submit([=] { return decodePng(bytes); }); // std::future
//
// Пул создаётся при первом обращении: hardware_concurrency − 1 рабочих (не меньше 1).
// parallelFor можно вызывать из задач пула (вложенно): вызывающий поток сам берёт части работы.
// Исключение из fn прерывает оставшиеся части и пробрасывается вызывающему (первое по времени).
// Не ждите future внутри задачи пула: при занятых рабочих это взаимная блокировка.
#pragma once
#include <future>
#include <memory>
#include <type_traits>

#include "base/base.h"

namespace rg::jobs {

int workers();  // число рабочих потоков пула

// Выполнить fn(i) для i ∈ [0, n). grain — размер части (0 — автоматически).
void parallelFor(size_t n, const std::function<void(size_t)>& fn, size_t grain = 0);
// То же по диапазонам: fn(begin, end) для непересекающихся частей [0, n).
void parallelRanges(size_t n, const std::function<void(size_t, size_t)>& fn, size_t grain = 0);

namespace detail {
struct Task {
  virtual ~Task() = default;
  virtual void run() = 0;
};
void enqueue(std::unique_ptr<Task> t);  // после shutdown задача выполняется в вызывающем потоке
template <class F>
struct FnTask final : Task {
  F f;
  explicit FnTask(F&& fn) : f(std::move(fn)) {}
  void run() override { f(); }
};
}  // namespace detail

// Поставить задачу в очередь. Результат или исключение — через future.
template <class F>
auto submit(F&& fn) -> std::future<std::invoke_result_t<std::decay_t<F>>> {
  using R = std::invoke_result_t<std::decay_t<F>>;
  std::packaged_task<R()> task(std::forward<F>(fn));
  std::future<R> fut = task.get_future();
  auto wrap = [t = std::move(task)]() mutable { t(); };
  detail::enqueue(std::make_unique<detail::FnTask<decltype(wrap)>>(std::move(wrap)));
  return fut;
}

// Остановить пул: дождаться текущих задач, невыполненные из очереди отбросить (их future получат broken_promise).
// Вызывается автоматически при завершении программы; повторный вызов безопасен.
void shutdown();

}  // namespace rg::jobs
