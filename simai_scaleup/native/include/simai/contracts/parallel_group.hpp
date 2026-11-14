#ifndef SIMAI_CONTRACTS_PARALLEL_GROUP_HPP
#define SIMAI_CONTRACTS_PARALLEL_GROUP_HPP

#include <unordered_set>
#include <vector>

#include "simai/contracts/ids.hpp"
#include "simai/contracts/validation.hpp"

namespace simai {
namespace contracts {

struct ParallelGroup {
  ParallelGroupId id;
  std::vector<DeviceId> members;

  bool contains(DeviceId device) const {
    for (const DeviceId member : members) {
      if (member == device) {
        return true;
      }
    }
    return false;
  }

  ValidationErrors validate() const {
    ValidationErrors errors;
    if (!id.valid()) {
      append_error(
          errors,
          ValidationCode::InvalidIdentity,
          "parallel-group identity must be nonzero");
    }
    if (members.size() < 2) {
      append_error(
          errors,
          ValidationCode::InvalidValue,
          "parallel group must contain at least two endpoints");
    }
    std::unordered_set<DeviceId> seen;
    for (const DeviceId member : members) {
      if (!member.valid()) {
        append_error(
            errors,
            ValidationCode::InvalidIdentity,
            "parallel-group member identity must be nonzero");
      } else if (!seen.insert(member).second) {
        append_error(
            errors,
            ValidationCode::DuplicateIdentity,
            "parallel group contains a duplicate endpoint");
      }
    }
    return errors;
  }

  void require_valid() const {
    require_no_errors(validate(), "ParallelGroup");
  }
};

}  // namespace contracts
}  // namespace simai

#endif
