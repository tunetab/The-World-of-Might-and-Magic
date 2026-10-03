// Тесты пула потоков: покрытие индексов, части, исключения, вложенность, future, параллельность.
#include <atomic>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include "base/jobs.h"
#include "tests/test.h"

using namespace rg;

TEST(base_jobs_parallel_for_coverage) {
  CHECK(jobs::workers() >= 1);
  for (size_t n : {size_t(0), size_t(1), size_t(2), size_t(7), size_t(1000), size_t(1000003)}) {
    for (size_t grain : {size_t(0), size_t(1), size_t(7), size_t(100000), size_t(5000000)}) {
      if (n > 10000 && grain == 1) continue;
      std::vector<std::atomic<int>> hits(n);
      jobs::parallelFor(n, [&](size_t i) { hits[i].fetch_add(1, std::memory_order_relaxed); }, grain);
      for (size_t i = 0; i < n; i++) CHECK(hits[i].load() == 1);
    }
  }
  // диапазоны не пересекаются и покрывают всё
  std::mutex m;
  std::vector<std::pair<size_t, size_t>> ranges;
  jobs::parallelRanges(10007, [&](size_t b, size_t e) {
    std::lock_guard<std::mutex> lk(m);
    ranges.emplace_back(b, e);
  }, 100);
  std::sort(ranges.begin(), ranges.end());
  size_t at = 0;
  for (auto& [b, e] : ranges) {
    CHECK_EQ(b, at);
    CHECK(e > b && e - b <= 100);
    at = e;
  }
  CHECK_EQ(at, size_t(10007));
}

TEST(base_jobs_exceptions) {
  std::atomic<int> calls{0};
  bool caught = false;
  try {
    jobs::parallelFor(100000, [&](size_t i) {
      calls++;
      if (i == 5000) throw std::runtime_error("ошибка в задаче");
    }, 100);
  } catch (const std::runtime_error& e) {
    caught = std::string(e.what()) == "ошибка в задаче";
  }
  CHECK(caught);
  CHECK(calls.load() < 100000);  // оставшиеся части отменены
  // UserError проходит без изменений
  bool user = false;
  try {
    jobs::parallelFor(10, [](size_t i) { if (i == 3) fail("понятная ошибка"); });
  } catch (const UserError& e) {
    user = std::string(e.what()) == "понятная ошибка";
  }
  CHECK(user);
  // пул работает после исключений
  std::atomic<long> sum{0};
  jobs::parallelFor(1000, [&](size_t i) { sum += long(i); });
  CHECK_EQ(sum.load(), 499500L);
}

TEST(base_jobs_nested_and_concurrency) {
  std::atomic<long> total{0};
  jobs::parallelFor(16, [&](size_t) {
    jobs::parallelFor(1000, [&](size_t j) { total += long(j); }, 10);
  }, 1);
  CHECK_EQ(total.load(), 16L * 499500L);
  // реальная параллельность: несколько разных потоков
  if (jobs::workers() >= 2) {
    std::mutex m;
    std::set<std::thread::id> ids;
    jobs::parallelFor(64, [&](size_t) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      std::lock_guard<std::mutex> lk(m);
      ids.insert(std::this_thread::get_id());
    }, 1);
    CHECK(ids.size() >= 2);
  }
}

TEST(base_jobs_submit_futures) {
  std::vector<std::future<int>> fs;
  for (int i = 0; i < 500; i++) fs.push_back(jobs::submit([i] { return i * i; }));
  long s = 0;
  for (auto& f : fs) s += f.get();
  CHECK_EQ(s, 41541750L);
  auto bad = jobs::submit([]() -> int { throw std::runtime_error("сбой"); });
  CHECK_THROWS(bad.get());
  auto v = jobs::submit([] {});
  v.get();
  // перемещаемый результат
  auto big = jobs::submit([] { return std::vector<int>(1000, 7); });
  CHECK_EQ(big.get().size(), size_t(1000));
  // задача ставит parallelFor внутри пула
  auto inner = jobs::submit([] {
    std::atomic<int> n{0};
    jobs::parallelFor(5000, [&](size_t) { n++; });
    return n.load();
  });
  CHECK_EQ(inner.get(), 5000);
}
