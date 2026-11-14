#ifndef SIMAI_RUNTIME_FINE_SYNCHRONIZATION_HPP
#define SIMAI_RUNTIME_FINE_SYNCHRONIZATION_HPP

#include <cstdint>
#include <map>
#include <string>
#include <unordered_set>
#include <vector>

#include "simai/runtime/fine_wavefront_engine.hpp"

namespace simai {
namespace runtime {

struct FineSynchronizationCompletion {
  contracts::WavefrontInstanceId wavefront_instance;
  contracts::GpuInstructionId instruction;
};

class FineSynchronizationEngine {
 public:
  std::vector<FineSynchronizationCompletion> process_semaphore(
      const FineInstructionIssue& issue);

  void register_workgroup_barrier(
      contracts::WorkgroupInstanceId workgroup,
      contracts::BarrierId barrier,
      std::uint64_t generation,
      const std::vector<contracts::WavefrontInstanceId>& participants);
  std::vector<contracts::WavefrontInstanceId> arrive_workgroup_barrier(
      contracts::WorkgroupInstanceId workgroup,
      contracts::BarrierId barrier,
      std::uint64_t generation,
      contracts::WavefrontInstanceId participant);

  void register_kernel_barrier(
      contracts::KernelInstanceId kernel,
      contracts::BarrierId barrier,
      std::uint64_t generation,
      const std::vector<contracts::WorkgroupInstanceId>& participants);
  std::vector<contracts::WorkgroupInstanceId> arrive_kernel_barrier(
      contracts::KernelInstanceId kernel,
      contracts::BarrierId barrier,
      std::uint64_t generation,
      contracts::WorkgroupInstanceId participant);

  std::vector<std::string> blocked_state_diagnostics() const;
  bool drained() const;

 private:
  struct SemaphoreKey {
    contracts::DeviceId owner;
    contracts::SemaphoreId semaphore;

    bool operator<(const SemaphoreKey& other) const;
  };

  struct SemaphoreWaiter {
    std::uint64_t generation;
    FineSynchronizationCompletion completion;
  };

  struct SemaphoreState {
    std::uint64_t published_generation;
    std::vector<SemaphoreWaiter> waiters;
  };

  struct BarrierKey {
    std::uint64_t scope_instance;
    contracts::BarrierId barrier;
    std::uint64_t generation;

    bool operator<(const BarrierKey& other) const;
  };

  template <typename Participant>
  struct BarrierState {
    std::vector<Participant> participants;
    std::unordered_set<Participant> arrived;
  };

  std::map<SemaphoreKey, SemaphoreState> semaphores_;
  std::map<BarrierKey, BarrierState<contracts::WavefrontInstanceId>>
      workgroup_barriers_;
  std::map<BarrierKey, BarrierState<contracts::WorkgroupInstanceId>>
      kernel_barriers_;
};

}  // namespace runtime
}  // namespace simai

#endif
