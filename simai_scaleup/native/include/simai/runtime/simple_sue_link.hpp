#ifndef SIMAI_RUNTIME_SIMPLE_SUE_LINK_HPP
#define SIMAI_RUNTIME_SIMPLE_SUE_LINK_HPP

#include "simai/runtime/sue_pdu.hpp"
#include "simai/runtime/workload.hpp"

namespace simai {
namespace runtime {

struct LinkTransferTiming {
  contracts::SimTimeNs issue_ns;
  contracts::SimTimeNs serialization_start_ns;
  contracts::SimTimeNs serialization_complete_ns;
  contracts::SimTimeNs arrival_ns;
};

class SimpleSueLink {
 public:
  explicit SimpleSueLink(const LinkConfig& config);

  static contracts::SimTimeNs serialization_delay_ns(
      std::uint64_t logical_bytes,
      std::uint64_t bandwidth_gbps);

  LinkTransferTiming transmit(
      const SuePdu& pdu,
      contracts::SimTimeNs issue_ns);

  contracts::SimTimeNs next_available_ns() const {
    return next_available_ns_;
  }

 private:
  LinkConfig config_;
  contracts::SimTimeNs next_available_ns_;
};

}  // namespace runtime
}  // namespace simai

#endif
