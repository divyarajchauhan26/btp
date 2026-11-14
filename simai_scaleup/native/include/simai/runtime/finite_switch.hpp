#ifndef SIMAI_RUNTIME_FINITE_SWITCH_HPP
#define SIMAI_RUNTIME_FINITE_SWITCH_HPP

#include <cstdint>
#include <deque>
#include <unordered_set>
#include <vector>

#include "simai/contracts/report.hpp"
#include "simai/runtime/simple_sue_link.hpp"

namespace simai {
namespace runtime {

struct SwitchForwardTiming {
  contracts::SimTimeNs arrival_at_switch_ns;
  contracts::SimTimeNs admission_ns;
  contracts::SimTimeNs service_start_ns;
  contracts::SimTimeNs departure_ns;
  contracts::SimTimeNs destination_arrival_ns;
  bool backpressured;
  bool duplicate;
};

class FiniteSwitch {
 public:
  FiniteSwitch(const LinkConfig& config, std::uint32_t endpoint_count);

  SwitchForwardTiming forward(
      const SuePdu& pdu,
      contracts::SimTimeNs arrival_at_switch_ns);

  std::vector<contracts::SwitchQueueUtilization> finalize(
      contracts::SimTimeNs now_ns);

  const std::vector<contracts::PduId>& arbitration_trace(
      contracts::DeviceId destination) const;

 private:
  struct QueueState {
    QueueState(const LinkConfig& config, std::uint32_t endpoint);

    SimpleSueLink egress_link;
    std::deque<contracts::SimTimeNs> resident_departures;
    std::deque<contracts::SimTimeNs> credit_returns;
    std::uint64_t available_credits;
    std::unordered_set<contracts::PduId> seen_pdus;
    contracts::SwitchQueueUtilization counters;
    std::vector<contracts::PduId> arbitration;
  };

  void drain(QueueState& queue, contracts::SimTimeNs now_ns);
  void require_credit_conservation(const QueueState& queue) const;

  LinkConfig config_;
  std::uint64_t capacity_pdus_;
  contracts::SimTimeNs last_arrival_ns_;
  std::vector<QueueState> queues_;
};

}  // namespace runtime
}  // namespace simai

#endif
