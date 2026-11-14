#ifndef SIMAI_CONTRACTS_VALIDATION_HPP
#define SIMAI_CONTRACTS_VALIDATION_HPP

#include <stdexcept>
#include <string>
#include <vector>

namespace simai {
namespace contracts {

enum class ValidationCode {
  InvalidIdentity,
  InvalidValue,
  InvalidBuffer,
  InvalidOperand,
  DuplicateIdentity,
  DuplicateDependency,
  SelfDependency,
  MissingDependency,
  DependencyCycle,
  EndpointOutsideGroup,
  InvalidProgramOrder,
  InvalidCompletionOrder,
  InvalidCompletionTime,
  InvalidTransaction,
  InvalidReport,
  InvalidMemoryReference,
  InvalidGpuOperation,
  InvalidGpuKernel,
  InvalidResourceRequirement
};

struct ValidationError {
  ValidationCode code;
  std::string detail;
};

using ValidationErrors = std::vector<ValidationError>;

inline void append_error(
    ValidationErrors& errors,
    ValidationCode code,
    const std::string& detail) {
  errors.push_back(ValidationError{code, detail});
}

inline void require_no_errors(
    const ValidationErrors& errors,
    const std::string& object_name) {
  if (!errors.empty()) {
    throw std::invalid_argument(object_name + ": " + errors.front().detail);
  }
}

}  // namespace contracts
}  // namespace simai

#endif
