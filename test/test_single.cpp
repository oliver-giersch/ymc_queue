#include <iostream>
#include <vector>

#include "queue.hpp"

int main() {
  const auto count = 10 * 1000;

  std::vector<int> storage{};
  storage.reserve(count);
  for (auto i = 0; i < count; ++i) {
    storage.push_back(i);
  }

  ymc::queue<int> queue{ 1 };

  for (auto& elem : storage) {
    queue.enqueue(&elem, 0);
  }

  for (auto i = 0; i < count; ++i) {
    auto res = queue.dequeue(0);
    if (res == nullptr) {
      std::cerr << "missing elements: " << i << "/" << count << std::endl;
      return 1;
    }

    if (*res != i) {
      std::cerr << "invalid element: " << *res << ", expected " << i << std::endl;
      return 1;
    }
  }

  if (queue.dequeue(0) != nullptr) {
    std::cerr << "too many elements in queue" << std::endl;
  }

  std::cout << "test successful" << std::endl;
}


