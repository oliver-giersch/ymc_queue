#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

#include "queue.hpp"

int main() {
  const auto thread_count = 8;
  const auto count = 10 * 1000;

  std::vector<std::vector<int>> thread_elements{};
  thread_elements.reserve(thread_count);

  for (auto t = 0; t < thread_count; ++t) {
    thread_elements.emplace_back();
    thread_elements[t].reserve(count);

    for (auto i = 0; i < count; ++i) {
      thread_elements[t].push_back(i);
    }
  }

  std::vector<std::thread> threads{};
  threads.reserve(thread_count);

  std::atomic_bool start{ false };
  std::atomic_uint64_t sum{ 0 };

  ymc::queue<int> queue{ thread_count * 2 };

  for (auto thread = 0; thread < thread_count; ++thread) {
    threads.emplace_back([&, thread] {
      while (!start.load());

      for (auto op = 0; op < count; ++op) {
        queue.enqueue(&thread_elements[thread][op], thread);
      }
    });

    const auto deq_id = thread + thread_count;
    threads.emplace_back([&, deq_id] {
      while (!start.load());

      auto thread_sum = 0;
      for (auto op = 0; op < count; ++op) {
        const auto res = queue.dequeue(deq_id);
        if (res == nullptr) {
          op -= 1;
        } else {
          thread_sum += *res;
        }
      }

      sum.fetch_add(thread_sum);
    });
  }

  start.store(true);

  for (auto& thread : threads) {
    thread.join();
  }

  const auto res = sum.load();
  const auto expected = count * (count - 1) / 2;
  if (res != expected) {
    std::cerr << "incorrect element sum, got " << sum << ", expected " << expected << std::endl;
    return 1;
  }

  std::cout << "test successful" << std::endl;
}