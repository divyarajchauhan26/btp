#ifndef SIMAI_CONTRACTS_WORKLOAD_HPP
#define SIMAI_CONTRACTS_WORKLOAD_HPP

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "simai/contracts/ids.hpp"
#include "simai/contracts/validation.hpp"

namespace simai {
namespace contracts {

enum class WorkloadEventKind { Compute, Collective };

struct WorkloadEvent {
  WorkloadEventId id;
  WorkloadEventKind kind;
  std::uint32_t layer_index;
  std::vector<WorkloadEventId> dependencies;
  DeviceId compute_device;
  SimTimeNs compute_duration_ns;
  CollectivePlanId collective_plan;

  static WorkloadEvent compute(
      WorkloadEventId id,
      std::uint32_t layer_index,
      const std::vector<WorkloadEventId>& dependencies,
      DeviceId device,
      SimTimeNs duration_ns) {
    return WorkloadEvent{
        id,
        WorkloadEventKind::Compute,
        layer_index,
        dependencies,
        device,
        duration_ns,
        CollectivePlanId()};
  }

  static WorkloadEvent collective(
      WorkloadEventId id,
      std::uint32_t layer_index,
      const std::vector<WorkloadEventId>& dependencies,
      CollectivePlanId plan) {
    return WorkloadEvent{
        id,
        WorkloadEventKind::Collective,
        layer_index,
        dependencies,
        DeviceId(),
        0,
        plan};
  }

  ValidationErrors validate() const {
    ValidationErrors errors;
    if (!id.valid()) {
      append_error(
          errors,
          ValidationCode::InvalidIdentity,
          "workload-event identity must be nonzero");
    }
    std::unordered_set<WorkloadEventId> seen;
    for (const WorkloadEventId dependency : dependencies) {
      if (!dependency.valid()) {
        append_error(
            errors,
            ValidationCode::InvalidIdentity,
            "workload dependency identity must be nonzero");
      } else if (dependency == id) {
        append_error(
            errors,
            ValidationCode::SelfDependency,
            "workload event may not depend on itself");
      } else if (!seen.insert(dependency).second) {
        append_error(
            errors,
            ValidationCode::DuplicateDependency,
            "workload event has a duplicate dependency");
      }
    }
    if (kind == WorkloadEventKind::Compute) {
      if (!compute_device.valid() || collective_plan.valid()) {
        append_error(
            errors,
            ValidationCode::InvalidOperand,
            "compute event requires an endpoint and no collective plan");
      }
    } else if (!collective_plan.valid() || compute_device.valid() ||
               compute_duration_ns != 0) {
      append_error(
          errors,
          ValidationCode::InvalidOperand,
          "collective event requires plan identity only");
    }
    return errors;
  }

  void require_valid() const {
    require_no_errors(validate(), "WorkloadEvent");
  }
};

}  // namespace contracts
}  // namespace simai

#endif
