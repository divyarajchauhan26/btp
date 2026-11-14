#include "simai/contracts/fine_memory.hpp"

namespace simai {
namespace contracts {

ValidationErrors FineMemoryRequest::validate() const {
  ValidationErrors errors = target.validate();
  if (!id.valid() || !kernel_instance.valid() ||
      !workgroup_instance.valid() || !wavefront_instance.valid() ||
      !instruction.valid() || !issuer.valid()) {
    append_error(
        errors,
        ValidationCode::InvalidIdentity,
        "fine memory request identities must be nonzero");
  }
  return errors;
}

void FineMemoryRequest::require_valid() const {
  require_no_errors(validate(), "FineMemoryRequest");
}

ValidationErrors FineMemoryCompletion::validate() const {
  ValidationErrors errors = request.validate();
  if (service_start_ns < request.issue_ns ||
      service_end_ns < service_start_ns || completion_ns < service_end_ns) {
    append_error(
        errors,
        ValidationCode::InvalidValue,
        "fine memory completion timestamps must be monotonic");
  }
  return errors;
}

void FineMemoryCompletion::require_valid() const {
  require_no_errors(validate(), "FineMemoryCompletion");
}

}  // namespace contracts
}  // namespace simai
