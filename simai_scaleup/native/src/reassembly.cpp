#include "simai/runtime/reassembly.hpp"

#include <stdexcept>

namespace simai {
namespace runtime {

PduReassembly::PduReassembly(
    const std::vector<contracts::PduId>& expected)
    : expected_(), received_(), duplicates_(0) {
  if (expected.empty()) {
    throw std::invalid_argument("reassembly requires at least one PDU");
  }
  for (const contracts::PduId pdu : expected) {
    if (!pdu.valid() || !expected_.insert(pdu).second) {
      throw std::invalid_argument(
          "reassembly PDU identities must be nonzero and unique");
    }
  }
}

bool PduReassembly::accept(contracts::PduId pdu) {
  if (expected_.find(pdu) == expected_.end()) {
    throw std::invalid_argument("reassembly received an unexpected PDU");
  }
  if (!received_.insert(pdu).second) {
    ++duplicates_;
    return false;
  }
  return true;
}

}  // namespace runtime
}  // namespace simai
