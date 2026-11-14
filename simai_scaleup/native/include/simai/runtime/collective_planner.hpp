#ifndef SIMAI_RUNTIME_COLLECTIVE_PLANNER_HPP
#define SIMAI_RUNTIME_COLLECTIVE_PLANNER_HPP

#include <cstdint>
#include <string>

#include "simai/contracts/collective.hpp"
#include "simai/runtime/workload.hpp"

namespace simai {
namespace runtime {

struct CollectiveRequest {
  contracts::CollectivePlanId id;
  contracts::CollectiveKind kind;
  contracts::ParallelGroup group;
  std::uint64_t payload_bytes;
  contracts::ReductionKind reduction;
  CollectiveAlgorithmPolicy algorithm;
};

struct PlannerCapabilities {
  std::uint32_t maximum_endpoints;
  bool native_put;
  bool native_reduce;
};

struct ScheduleProvenance {
  std::string source;
  std::string algorithm;
  std::string version;
};

struct PlannedCollective {
  contracts::CollectivePlan plan;
  ScheduleProvenance provenance;
};

class CollectiveScheduleProvider {
 public:
  virtual ~CollectiveScheduleProvider() {}

  virtual PlannedCollective plan(
      const CollectiveRequest& request,
      const PlannerCapabilities& capabilities) const = 0;
};

class NativeCollectivePlanner final : public CollectiveScheduleProvider {
 public:
  PlannedCollective plan(
      const CollectiveRequest& request,
      const PlannerCapabilities& capabilities) const override;
};

}  // namespace runtime
}  // namespace simai

#endif
