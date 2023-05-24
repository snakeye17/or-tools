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

// Simple framework for choosing and distributing a solver "sub-tasks" on a set
// of threads.

#ifndef OR_TOOLS_SAT_SUBSOLVER_H_
#define OR_TOOLS_SAT_SUBSOLVER_H_

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ortools/base/integral_types.h"

#if !defined(__PORTABLE_PLATFORM__)
#include "ortools/base/threadpool.h"
#endif  // __PORTABLE_PLATFORM__

namespace operations_research {
namespace sat {

// The API used for distributing work. Each subsolver can generate tasks and
// synchronize itself with the rest of the world.
//
// Note that currently only the main thread interact with subsolvers. Only the
// tasks generated by GenerateTask() are executed in parallel in a threadpool.
class SubSolver {
 public:
  enum SubsolverType { FULL_PROBLEM, FIRST_SOLUTION, INCOMPLETE, HELPER };

  SubSolver(const std::string& name, SubsolverType type)
      : name_(name), type_(type) {}
  virtual ~SubSolver() = default;

  // Synchronizes with the external world from this SubSolver point of view.
  // Also incorporate the results of the latest completed tasks if any.
  //
  // Note(user): The intended implementation for determinism is that tasks
  // update asynchronously (and so non-deterministically) global "shared"
  // classes, but this global state is incorporated by the Subsolver only when
  // Synchronize() is called.
  virtual void Synchronize() = 0;

  // Returns true if this SubSolver is done and its memory can be freed. Note
  // that the *Loop(subsolvers) functions below takes a reference in order to be
  // able to clear the memory of a SubSolver as soon as it is done. Once this is
  // true, the subsolver in question will be deleted and never used again.
  //
  // This is needed since some subsolve can be done before the overal Solve() is
  // finished. This is the case for first solution subsolvers for instances.
  virtual bool IsDone() { return false; }

  // Returns true iff GenerateTask() can be called.
  virtual bool TaskIsAvailable() = 0;

  // Returns a task to run. The task_id is just an ever increasing counter that
  // correspond to the number of total calls to GenerateTask().
  //
  // TODO(user): We could use a more complex selection logic and pass in the
  // deterministic time limit this subtask should run for. Unclear at this
  // stage.
  virtual std::function<void()> GenerateTask(int64_t task_id) = 0;

  // Returns the total deterministic time spend by the completed tasks before
  // the last Synchronize() call.
  double deterministic_time() const { return deterministic_time_; }

  // Returns the name of this SubSolver. Used in logs.
  std::string name() const { return name_; }

  // Returns the type of the subsolver.
  SubsolverType type() const { return type_; }

  // Returns search statistics.
  virtual std::string StatisticsString() const { return std::string(); }

 protected:
  const std::string name_;
  const SubsolverType type_;
  double deterministic_time_ = 0.0;
};

// A simple wrapper to add a synchronization point in the list of subsolvers.
class SynchronizationPoint : public SubSolver {
 public:
  explicit SynchronizationPoint(const std::string& name,
                                std::function<void()> f)
      : SubSolver(name, HELPER), f_(std::move(f)) {}
  bool TaskIsAvailable() final { return false; }
  std::function<void()> GenerateTask(int64_t /*task_id*/) final {
    return nullptr;
  }
  void Synchronize() final { f_(); }

 private:
  std::function<void()> f_;
};

// Executes the following loop:
// 1/ Synchronize all in given order.
// 2/ generate and schedule one task from the current "best" subsolver.
// 3/ repeat until no extra task can be generated and all tasks are done.
//
// The complexity of each selection is in O(num_subsolvers), but that should
// be okay given that we don't expect more than 100 such subsolvers.
//
// Note that it is okay to incorporate "special" subsolver that never produce
// any tasks. This can be used to synchronize classes used by many subsolvers
// just once for instance.
void NonDeterministicLoop(std::vector<std::unique_ptr<SubSolver>>& subsolvers,
                          int num_threads);

// Similar to NonDeterministicLoop() except this should result in a
// deterministic solver provided that all SubSolver respect the Synchronize()
// contract.
//
// Executes the following loop:
// 1/ Synchronize all in given order.
// 2/ generate and schedule up to batch_size tasks using an heuristic to select
//    which one to run.
// 3/ wait for all task to finish.
// 4/ repeat until no task can be generated in step 2.
void DeterministicLoop(std::vector<std::unique_ptr<SubSolver>>& subsolvers,
                       int num_threads, int batch_size);

// Same as above, but specialized implementation for the case num_threads=1.
// This avoids using a Threadpool altogether. It should have the same behavior
// than the functions above with num_threads=1 and batch_size=1. Note that an
// higher batch size will not behave in the same way, even if num_threads=1.
void SequentialLoop(std::vector<std::unique_ptr<SubSolver>>& subsolvers);

}  // namespace sat
}  // namespace operations_research

#endif  // OR_TOOLS_SAT_SUBSOLVER_H_
