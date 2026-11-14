#ifndef SIMAI_RUNTIME_MULTI_RAIL_FABRIC_HPP
#define SIMAI_RUNTIME_MULTI_RAIL_FABRIC_HPP

#include <cstddef>
#include <memory>
#include <vector>

#include "simai/contracts/report.hpp"
#include "simai/runtime/finite_switch.hpp"
#include "simai/runtime/simple_sue_link.hpp"

namespace simai {
namespace runtime {

struct FabricIngressTiming {
  contracts::RailId rail;
  contracts::SimTimeNs local_complete_ns;
  contracts::SimTimeNs ingress_arrival_ns;
  std::uint64_t transmission_attempts;
  std::uint64_t retries;
  std::uint64_t transmitted_wire_bytes;
};

class MultiRailFabric {
 public:
  MultiRailFabric(const LinkConfig& config, std::uint32_t endpoint_count);

  FabricIngressTiming transmit_ingress(
      const SuePdu& pdu,
      contracts::SimTimeNs issue_ns);

  SwitchForwardTiming forward_from_switch(
      const SuePdu& pdu,
      contracts::SimTimeNs ingress_arrival_ns);

  bool has_switch() const { return static_cast<bool>(switch_); }

  const std::vector<contracts::RailUtilization>& rail_counters() const {
    return rail_counters_;
  }

  std::vector<contracts::SwitchQueueUtilization> finalize_switch(
      contracts::SimTimeNs now_ns);

 private:
  std::size_t select_rail(const SuePdu& pdu) const;

  LinkConfig config_;
  std::vector<SimpleSueLink> rails_;
  std::vector<contracts::RailUtilization> rail_counters_;
  std::unique_ptr<FiniteSwitch> switch_;
};

}  // namespace runtime
}  // namespace simai

#endif
