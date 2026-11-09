#include "Tasks.h"
#include <utility>

namespace nativescript {

void Tasks::Register(std::function<void()> task) {
    tasks_.emplace_back(std::move(task));
}

void Tasks::Drain() {
    auto i = std::begin(tasks_);
    while (i != std::end(tasks_)) {
        std::function<void()> task = *i;
        task();
        i = tasks_.erase(i);
        ++i;
    }
}

void Tasks::ClearTasks() {
    tasks_.clear();
}

std::vector<std::function<void()>> Tasks::tasks_;

}
