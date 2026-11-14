#include "simai/runtime/transport_backend.hpp"

#include <stdexcept>

namespace simai {
namespace runtime {

bool SueTransportBackend::supports_asynchronous_submissions() const {
  return false;
}

void SueTransportBackend::submit_async(
    const SuePdu&, contracts::SimTimeNs) {
  throw std::logic_error(
      "transport backend does not support asynchronous submissions");
}

bool SueTransportBackend::has_pending_async() const { return false; }

bool SueTransportBackend::advance_to_next_completion(
    contracts::SimTimeNs, SueTransportCompletion&) {
  throw std::logic_error(
      "transport backend does not support asynchronous completion polling");
}

NativeSueTransportBackend::NativeSueTransportBackend(
    const LinkConfig& config, std::uint32_t endpoint_count)
    : fabric_(config, endpoint_count) {}

FabricIngressTiming NativeSueTransportBackend::transmit_ingress(
    const SuePdu& pdu, contracts::SimTimeNs issue_ns) {
  return fabric_.transmit_ingress(pdu, issue_ns);
}

SwitchForwardTiming NativeSueTransportBackend::forward_from_switch(
    const SuePdu& pdu, contracts::SimTimeNs ingress_arrival_ns) {
  return fabric_.forward_from_switch(pdu, ingress_arrival_ns);
}

bool NativeSueTransportBackend::has_switch() const { return fabric_.has_switch(); }

const std::vector<contracts::RailUtilization>&
NativeSueTransportBackend::rail_counters() const {
  return fabric_.rail_counters();
}

std::vector<contracts::SwitchQueueUtilization>
NativeSueTransportBackend::finalize_switch(contracts::SimTimeNs now_ns) {
  return fabric_.finalize_switch(now_ns);
}

std::unique_ptr<SueTransportBackend> NativeSueTransportBackendFactory::create(
    const LinkConfig& config, std::uint32_t endpoint_count) const {
  return std::unique_ptr<SueTransportBackend>(
      new NativeSueTransportBackend(config, endpoint_count));
}

}  // namespace runtime
}  // namespace simai
