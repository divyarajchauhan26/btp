#include "simai/runtime/simple_sue_link.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace simai {
namespace runtime {
namespace {

contracts::SimTimeNs checked_add(
    contracts::SimTimeNs lhs,
    contracts::SimTimeNs rhs,
    const char* message) {
  if (lhs > std::numeric_limits<contracts::SimTimeNs>::max() - rhs) {
    throw std::overflow_error(message);
  }
  return lhs + rhs;
}

}  // namespace

SimpleSueLink::SimpleSueLink(const LinkConfig& config)
    : config_(config), next_available_ns_(0) {
  if (config_.bandwidth_gbps == 0) {
    throw std::invalid_argument("SUE link bandwidth must be nonzero");
  }
}

contracts::SimTimeNs SimpleSueLink::serialization_delay_ns(
    std::uint64_t logical_bytes,
    std::uint64_t bandwidth_gbps) {
  if (logical_bytes == 0 || bandwidth_gbps == 0) {
    throw std::invalid_argument(
        "serialization requires nonzero bytes and bandwidth");
  }
  if (logical_bytes > std::numeric_limits<std::uint64_t>::max() / 8) {
    throw std::overflow_error("logical byte count overflows bit conversion");
  }
  const std::uint64_t bits = logical_bytes * 8;
  return bits / bandwidth_gbps +
      (bits % bandwidth_gbps == 0 ? 0 : 1);
}

LinkTransferTiming SimpleSueLink::transmit(
    const SuePdu& pdu,
    contracts::SimTimeNs issue_ns) {
  pdu.require_valid();
  const contracts::SimTimeNs serialization = serialization_delay_ns(
      pdu.wire_bytes, config_.bandwidth_gbps);
  const contracts::SimTimeNs start =
      std::max(issue_ns, next_available_ns_);
  const contracts::SimTimeNs serialization_complete = checked_add(
      start, serialization, "SUE serialization time overflow");
  const contracts::SimTimeNs arrival = checked_add(
      serialization_complete,
      config_.propagation_delay_ns,
      "SUE arrival time overflow");
  next_available_ns_ = serialization_complete;
  return LinkTransferTiming{
      issue_ns, start, serialization_complete, arrival};
}

}  // namespace runtime
}  // namespace simai
