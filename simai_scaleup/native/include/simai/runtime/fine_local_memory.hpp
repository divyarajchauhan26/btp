#ifndef SIMAI_RUNTIME_FINE_LOCAL_MEMORY_HPP
#define SIMAI_RUNTIME_FINE_LOCAL_MEMORY_HPP

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "simai/contracts/fine_memory.hpp"
#include "simai/runtime/fine_wavefront_engine.hpp"

namespace simai {
namespace runtime {

struct FineLocalMemoryTierConfig {
  contracts::FineMemorySpace space;
  contracts::SimTimeNs base_latency_ns;
  std::uint64_t bytes_per_ns;
  std::size_t queue_capacity;
  std::size_t max_outstanding;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
};

struct FineLocalMemorySystemConfig {
  std::uint32_t device_count;
  std::vector<FineLocalMemoryTierConfig> tiers;

  contracts::ValidationErrors validate() const;
  void require_valid() const;
};

struct FineMemorySubmitResult {
  bool accepted;
  contracts::FineStallReason stall_reason;
  contracts::FineMemoryCompletion completion;
};

contracts::FineMemoryRequest lower_local_memory_request(
    contracts::MemoryRequestId request_id,
    const FineInstructionIssue& issue);

class FineLocalMemorySystem {
 public:
  explicit FineLocalMemorySystem(const FineLocalMemorySystemConfig& config);

  FineMemorySubmitResult submit(const contracts::FineMemoryRequest& request);
  contracts::FineMemoryCompletion complete(
      contracts::MemoryRequestId request,
      contracts::SimTimeNs completion_ns);

  std::size_t active_requests() const { return requests_.size(); }
  std::size_t active_requests(
      contracts::DeviceId device,
      contracts::FineMemorySpace space) const;
  bool drained() const { return requests_.empty(); }

 private:
  struct TierState {
    contracts::SimTimeNs next_service_ns;
    std::vector<contracts::MemoryRequestId> requests;
  };

  struct RequestState {
    std::size_t tier_state;
    contracts::FineMemoryCompletion expected;
  };

  std::size_t tier_config_index(contracts::FineMemorySpace space) const;
  std::size_t tier_state_index(
      contracts::DeviceId device, std::size_t tier_config) const;

  FineLocalMemorySystemConfig config_;
  std::vector<TierState> tiers_;
  std::unordered_map<contracts::MemoryRequestId, RequestState> requests_;
};

}  // namespace runtime
}  // namespace simai

#endif
