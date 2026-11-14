#include "simai/runtime/fine_synchronization.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace simai {
namespace runtime {
namespace {

template <typename Participant>
void require_participants(const std::vector<Participant>& participants) {
  if (participants.empty()) {
    throw std::invalid_argument("fine barrier participant set must not be empty");
  }
  std::unordered_set<Participant> unique;
  for (Participant participant : participants) {
    if (!participant.valid() || !unique.insert(participant).second) {
      throw std::invalid_argument(
          "fine barrier participants must be valid and unique");
    }
  }
}

}  // namespace

bool FineSynchronizationEngine::SemaphoreKey::operator<(
    const SemaphoreKey& other) const {
  if (owner != other.owner) return owner < other.owner;
  return semaphore < other.semaphore;
}

bool FineSynchronizationEngine::BarrierKey::operator<(
    const BarrierKey& other) const {
  if (scope_instance != other.scope_instance) {
    return scope_instance < other.scope_instance;
  }
  if (barrier != other.barrier) return barrier < other.barrier;
  return generation < other.generation;
}

std::vector<FineSynchronizationCompletion>
FineSynchronizationEngine::process_semaphore(
    const FineInstructionIssue& issue) {
  const contracts::FineGpuInstruction& instruction = issue.instruction;
  const bool acquire =
      instruction.kind == contracts::FineGpuInstructionKind::SemaphoreAcquire;
  const bool release =
      instruction.kind == contracts::FineGpuInstructionKind::SemaphoreRelease;
  if (!acquire && !release) {
    throw std::invalid_argument("fine synchronization requires a semaphore instruction");
  }
  instruction.require_valid();
  const SemaphoreKey key{instruction.semaphore_owner, instruction.semaphore};
  SemaphoreState& state = semaphores_[key];
  const FineSynchronizationCompletion current{
      issue.wavefront_instance, instruction.id};
  if (acquire) {
    if (state.published_generation >= instruction.semaphore_generation) {
      return {current};
    }
    for (const SemaphoreWaiter& waiter : state.waiters) {
      if (waiter.completion.wavefront_instance == issue.wavefront_instance &&
          waiter.completion.instruction == instruction.id) {
        throw std::invalid_argument("fine semaphore acquire is already waiting");
      }
    }
    state.waiters.push_back(
        SemaphoreWaiter{instruction.semaphore_generation, current});
    return {};
  }

  if (instruction.semaphore_generation <= state.published_generation) {
    throw std::invalid_argument("fine semaphore release generation must advance");
  }
  state.published_generation = instruction.semaphore_generation;
  std::vector<FineSynchronizationCompletion> completed{current};
  std::vector<SemaphoreWaiter> remaining;
  for (const SemaphoreWaiter& waiter : state.waiters) {
    if (waiter.generation <= state.published_generation) {
      completed.push_back(waiter.completion);
    } else {
      remaining.push_back(waiter);
    }
  }
  state.waiters.swap(remaining);
  return completed;
}

void FineSynchronizationEngine::register_workgroup_barrier(
    contracts::WorkgroupInstanceId workgroup,
    contracts::BarrierId barrier,
    std::uint64_t generation,
    const std::vector<contracts::WavefrontInstanceId>& participants) {
  if (!workgroup.valid() || !barrier.valid() || generation == 0) {
    throw std::invalid_argument("fine workgroup barrier identity is invalid");
  }
  require_participants(participants);
  const BarrierKey key{workgroup.value(), barrier, generation};
  if (!workgroup_barriers_.insert(
          std::make_pair(
              key,
              BarrierState<contracts::WavefrontInstanceId>{participants, {}}))
           .second) {
    throw std::invalid_argument("fine workgroup barrier is already registered");
  }
}

std::vector<contracts::WavefrontInstanceId>
FineSynchronizationEngine::arrive_workgroup_barrier(
    contracts::WorkgroupInstanceId workgroup,
    contracts::BarrierId barrier,
    std::uint64_t generation,
    contracts::WavefrontInstanceId participant) {
  const BarrierKey key{workgroup.value(), barrier, generation};
  const auto found = workgroup_barriers_.find(key);
  if (found == workgroup_barriers_.end()) {
    throw std::invalid_argument("fine workgroup barrier is not registered");
  }
  BarrierState<contracts::WavefrontInstanceId>& state = found->second;
  if (std::find(
          state.participants.begin(), state.participants.end(), participant) ==
      state.participants.end()) {
    throw std::invalid_argument("fine workgroup barrier arrival is not a participant");
  }
  if (!state.arrived.insert(participant).second) {
    throw std::invalid_argument("fine workgroup barrier participant arrived twice");
  }
  if (state.arrived.size() != state.participants.size()) return {};
  const std::vector<contracts::WavefrontInstanceId> released =
      state.participants;
  workgroup_barriers_.erase(found);
  return released;
}

void FineSynchronizationEngine::register_kernel_barrier(
    contracts::KernelInstanceId kernel,
    contracts::BarrierId barrier,
    std::uint64_t generation,
    const std::vector<contracts::WorkgroupInstanceId>& participants) {
  if (!kernel.valid() || !barrier.valid() || generation == 0) {
    throw std::invalid_argument("fine kernel barrier identity is invalid");
  }
  require_participants(participants);
  const BarrierKey key{kernel.value(), barrier, generation};
  if (!kernel_barriers_.insert(std::make_pair(
          key,
          BarrierState<contracts::WorkgroupInstanceId>{participants, {}}))
           .second) {
    throw std::invalid_argument("fine kernel barrier is already registered");
  }
}

std::vector<contracts::WorkgroupInstanceId>
FineSynchronizationEngine::arrive_kernel_barrier(
    contracts::KernelInstanceId kernel,
    contracts::BarrierId barrier,
    std::uint64_t generation,
    contracts::WorkgroupInstanceId participant) {
  const BarrierKey key{kernel.value(), barrier, generation};
  const auto found = kernel_barriers_.find(key);
  if (found == kernel_barriers_.end()) {
    throw std::invalid_argument("fine kernel barrier is not registered");
  }
  BarrierState<contracts::WorkgroupInstanceId>& state = found->second;
  if (std::find(
          state.participants.begin(), state.participants.end(), participant) ==
      state.participants.end()) {
    throw std::invalid_argument("fine kernel barrier arrival is not a participant");
  }
  if (!state.arrived.insert(participant).second) {
    throw std::invalid_argument("fine kernel barrier participant arrived twice");
  }
  if (state.arrived.size() != state.participants.size()) return {};
  const std::vector<contracts::WorkgroupInstanceId> released =
      state.participants;
  kernel_barriers_.erase(found);
  return released;
}

std::vector<std::string>
FineSynchronizationEngine::blocked_state_diagnostics() const {
  std::vector<std::string> diagnostics;
  for (const auto& entry : semaphores_) {
    for (const SemaphoreWaiter& waiter : entry.second.waiters) {
      std::ostringstream text;
      text << "semaphore owner=" << entry.first.owner.value()
           << " id=" << entry.first.semaphore.value()
           << " published=" << entry.second.published_generation
           << " waits_for=" << waiter.generation
           << " wavefront=" << waiter.completion.wavefront_instance.value();
      diagnostics.push_back(text.str());
    }
  }
  for (const auto& entry : workgroup_barriers_) {
    std::ostringstream text;
    text << "workgroup barrier scope=" << entry.first.scope_instance
         << " id=" << entry.first.barrier.value()
         << " generation=" << entry.first.generation
         << " arrived=" << entry.second.arrived.size()
         << " required=" << entry.second.participants.size();
    diagnostics.push_back(text.str());
  }
  for (const auto& entry : kernel_barriers_) {
    std::ostringstream text;
    text << "kernel barrier scope=" << entry.first.scope_instance
         << " id=" << entry.first.barrier.value()
         << " generation=" << entry.first.generation
         << " arrived=" << entry.second.arrived.size()
         << " required=" << entry.second.participants.size();
    diagnostics.push_back(text.str());
  }
  return diagnostics;
}

bool FineSynchronizationEngine::drained() const {
  for (const auto& entry : semaphores_) {
    if (!entry.second.waiters.empty()) return false;
  }
  return workgroup_barriers_.empty() && kernel_barriers_.empty();
}

}  // namespace runtime
}  // namespace simai
