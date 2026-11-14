#ifndef SIMAI_RUNTIME_SIMULATOR_HPP
#define SIMAI_RUNTIME_SIMULATOR_HPP

#include "simai/contracts/report.hpp"
#include "simai/runtime/collective_planner.hpp"
#include "simai/runtime/transport_backend.hpp"
#include "simai/runtime/workload.hpp"

namespace simai {
namespace runtime {

class ScaleUpSimulator {
 public:
  contracts::SimulationReport run(const WorkloadSpec& workload) const;
  contracts::SimulationReport run(
      const WorkloadSpec& workload,
      const CollectiveScheduleProvider& schedule_provider) const;
  contracts::SimulationReport run(
      const WorkloadSpec& workload,
      const CollectiveScheduleProvider& schedule_provider,
      const SueTransportBackendFactory& backend_factory) const;
};

}  // namespace runtime
}  // namespace simai

#endif
