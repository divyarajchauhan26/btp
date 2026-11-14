#ifndef SIMAI_RUNTIME_TRANSPORT_BACKEND_HPP
#define SIMAI_RUNTIME_TRANSPORT_BACKEND_HPP

#include <cstdint>
#include <memory>
#include <vector>

#include "simai/contracts/report.hpp"
#include "simai/runtime/multi_rail_fabric.hpp"
#include "simai/runtime/workload.hpp"

namespace simai {
namespace runtime {

struct SueTransportCompletion {
  contracts::PduId pdu;
  contracts::RailId rail;
  contracts::SimTimeNs issue_ns;
  contracts::SimTimeNs visible_ns;
};

class SueTransportBackend {
 public:
  virtual ~SueTransportBackend() {}
  virtual FabricIngressTiming transmit_ingress(
      const SuePdu& pdu, contracts::SimTimeNs issue_ns) = 0;
  virtual SwitchForwardTiming forward_from_switch(
      const SuePdu& pdu, contracts::SimTimeNs ingress_arrival_ns) = 0;
  virtual bool has_switch() const = 0;
  virtual const std::vector<contracts::RailUtilization>& rail_counters() const = 0;
  virtual std::vector<contracts::SwitchQueueUtilization> finalize_switch(
      contracts::SimTimeNs now_ns) = 0;
  virtual bool supports_asynchronous_submissions() const;
  virtual void submit_async(
      const SuePdu& pdu, contracts::SimTimeNs issue_ns);
  virtual bool has_pending_async() const;
  virtual bool advance_to_next_completion(
      contracts::SimTimeNs deadline_ns,
      SueTransportCompletion& completion);
};

class SueTransportBackendFactory {
 public:
  virtual ~SueTransportBackendFactory() {}
  virtual std::unique_ptr<SueTransportBackend> create(
      const LinkConfig& config, std::uint32_t endpoint_count) const = 0;
};

class NativeSueTransportBackend final : public SueTransportBackend {
 public:
  NativeSueTransportBackend(
      const LinkConfig& config, std::uint32_t endpoint_count);
  FabricIngressTiming transmit_ingress(
      const SuePdu& pdu, contracts::SimTimeNs issue_ns) override;
  SwitchForwardTiming forward_from_switch(
      const SuePdu& pdu, contracts::SimTimeNs ingress_arrival_ns) override;
  bool has_switch() const override;
  const std::vector<contracts::RailUtilization>& rail_counters() const override;
  std::vector<contracts::SwitchQueueUtilization> finalize_switch(
      contracts::SimTimeNs now_ns) override;

 private:
  MultiRailFabric fabric_;
};

class NativeSueTransportBackendFactory final
    : public SueTransportBackendFactory {
 public:
  std::unique_ptr<SueTransportBackend> create(
      const LinkConfig& config,
      std::uint32_t endpoint_count) const override;
};

}  // namespace runtime
}  // namespace simai

#endif
