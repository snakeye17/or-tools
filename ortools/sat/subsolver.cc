// Copyright 2010-2022 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "ortools/sat/subsolver.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/log/check.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/blocking_counter.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "ortools/base/logging.h"
#if !defined(__PORTABLE_PLATFORM__)
#include "ortools/base/threadpool.h"
#endif  // __PORTABLE_PLATFORM__

namespace operations_research {
namespace sat {

namespace {

// Returns the next SubSolver index from which to call GenerateTask(). Note that
// only SubSolvers for which TaskIsAvailable() is true are considered. Return -1
// if no SubSolver can generate a new task.
//
// For now we use a really basic logic: call the least frequently called.
int NextSubsolverToSchedule(std::vector<std::unique_ptr<SubSolver>>& subsolvers,
                            const std::vector<int64_t>& num_generated_tasks) {
  int best = -1;
  for (int i = 0; i < subsolvers.size(); ++i) {
    if (subsolvers[i] == nullptr) continue;
    if (subsolvers[i]->IsDone()) {
      // We can free the memory used by this solver for good.
      VLOG(1) << "Deleting " << subsolvers[i]->name();
      subsolvers[i].reset();
      continue;
    }
    if (subsolvers[i]->TaskIsAvailable()) {
      if (best == -1 || num_generated_tasks[i] < num_generated_tasks[best]) {
        best = i;
      }
    }
  }
  if (best != -1) VLOG(1) << "Scheduling " << subsolvers[best]->name();
  return best;
}

void SynchronizeAll(const std::vector<std::unique_ptr<SubSolver>>& subsolvers) {
  for (const auto& subsolver : subsolvers) {
    if (subsolver == nullptr) continue;
    subsolver->Synchronize();
  }
}

}  // namespace

void SequentialLoop(std::vector<std::unique_ptr<SubSolver>>& subsolvers) {
  int64_t task_id = 0;
  std::vector<int64_t> num_generated_tasks(subsolvers.size(), 0);
  while (true) {
    SynchronizeAll(subsolvers);
    const int best = NextSubsolverToSchedule(subsolvers, num_generated_tasks);
    if (best == -1) break;
    num_generated_tasks[best]++;
    subsolvers[best]->GenerateTask(task_id++)();
  }
}

#if defined(__PORTABLE_PLATFORM__)

// On portable platform, we don't support multi-threading for now.

void NonDeterministicLoop(std::vector<std::unique_ptr<SubSolver>>& subsolvers,
                          int num_threads) {
  SequentialLoop(subsolvers);
}

void DeterministicLoop(std::vector<std::unique_ptr<SubSolver>>& subsolvers,
                       int num_threads, int batch_size) {
  SequentialLoop(subsolvers);
}

#else  // __PORTABLE_PLATFORM__

void DeterministicLoop(std::vector<std::unique_ptr<SubSolver>>& subsolvers,
                       int num_threads, int batch_size) {
  CHECK_GT(num_threads, 0);
  CHECK_GT(batch_size, 0);
  if (batch_size == 1) {
    return SequentialLoop(subsolvers);
  }

  int64_t task_id = 0;
  std::vector<int64_t> num_generated_tasks(subsolvers.size(), 0);
  std::vector<std::function<void()>> to_run;
  to_run.reserve(batch_size);
  ThreadPool pool("DeterministicLoop", num_threads);
  pool.StartWorkers();
  while (true) {
    SynchronizeAll(subsolvers);

    // We first generate all task to run in this batch.
    // Note that we can't start the task right away since if a task finish
    // before we schedule everything, we will not be deterministic.
    for (int t = 0; t < batch_size; ++t) {
      const int best = NextSubsolverToSchedule(subsolvers, num_generated_tasks);
      if (best == -1) break;
      num_generated_tasks[best]++;
      to_run.push_back(subsolvers[best]->GenerateTask(task_id++));
    }
    if (to_run.empty()) break;

    // Schedule each task.
    absl::BlockingCounter blocking_counter(static_cast<int>(to_run.size()));
    for (auto& f : to_run) {
      pool.Schedule([f = std::move(f), &blocking_counter]() {
        f();
        blocking_counter.DecrementCount();
      });
    }
    to_run.clear();

    // Wait for all tasks of this batch to be done before scheduling another
    // batch.
    blocking_counter.Wait();
  }
}

void NonDeterministicLoop(std::vector<std::unique_ptr<SubSolver>>& subsolvers,
                          const int num_threads) {
  CHECK_GT(num_threads, 0);
  if (num_threads == 1) {
    return SequentialLoop(subsolvers);
  }

  // The mutex guards num_in_flight. This is used to detect when the search is
  // done.
  absl::Mutex mutex;
  int num_in_flight = 0;  // Guarded by `mutex`.
  // Predicate to be used with absl::Condition to detect that num_in_flight <
  // num_threads. Must only be called while locking `mutex`.
  const auto num_in_flight_lt_num_threads = [&num_in_flight, num_threads]() {
    return num_in_flight < num_threads;
  };

  ThreadPool pool("NonDeterministicLoop", num_threads);
  pool.StartWorkers();

  // The lambda below are using little space, but there is no reason
  // to create millions of them, so we use the blocking nature of
  // pool.Schedule() when the queue capacity is set.
  int64_t task_id = 0;
  std::vector<int64_t> num_generated_tasks(subsolvers.size(), 0);
  while (true) {
    // Set to true if no task is pending right now.
    bool all_done = false;
    {
      // Wait if num_in_flight == num_threads.
      const absl::MutexLock mutex_lock(
          &mutex, absl::Condition(&num_in_flight_lt_num_threads));

      // The stopping condition is that we do not have anything else to generate
      // once all the task are done and synchronized.
      if (num_in_flight == 0) all_done = true;
    }

    SynchronizeAll(subsolvers);
    const int best = NextSubsolverToSchedule(subsolvers, num_generated_tasks);
    if (best == -1) {
      if (all_done) break;

      // It is hard to know when new info will allows for more task to be
      // scheduled, so for now we just sleep for a bit. Note that in practice We
      // will never reach here except at the end of the search because we can
      // always schedule LNS threads.
      absl::SleepFor(absl::Milliseconds(1));
      continue;
    }

    // Schedule next task.
    num_generated_tasks[best]++;
    {
      absl::MutexLock mutex_lock(&mutex);
      num_in_flight++;
    }
    std::function<void()> task = subsolvers[best]->GenerateTask(task_id++);
    const std::string name = subsolvers[best]->name();
    pool.Schedule([task = std::move(task), name, &mutex, &num_in_flight]() {
      task();

      const absl::MutexLock mutex_lock(&mutex);
      VLOG(1) << name << " done.";
      num_in_flight--;
    });
  }
}

#endif  // __PORTABLE_PLATFORM__

}  // namespace sat
}  // namespace operations_research
