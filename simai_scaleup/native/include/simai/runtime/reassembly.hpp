#ifndef SIMAI_RUNTIME_REASSEMBLY_HPP
#define SIMAI_RUNTIME_REASSEMBLY_HPP

#include <cstddef>
#include <unordered_set>
#include <vector>

#include "simai/contracts/ids.hpp"

namespace simai {
namespace runtime {

class PduReassembly {
 public:
  explicit PduReassembly(const std::vector<contracts::PduId>& expected);

  bool accept(contracts::PduId pdu);
  bool complete() const { return received_.size() == expected_.size(); }
  std::size_t unique_received() const { return received_.size(); }
  std::size_t duplicates() const { return duplicates_; }

 private:
  std::unordered_set<contracts::PduId> expected_;
  std::unordered_set<contracts::PduId> received_;
  std::size_t duplicates_;
};

}  // namespace runtime
}  // namespace simai

#endif
