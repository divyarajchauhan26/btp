#include "simai/contracts/collective.hpp"

#include <functional>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace simai {
namespace contracts {
namespace {

BufferSlice empty_slice() {
  return BufferSlice{DeviceId(), BufferId(), 0, 0, MemoryRegion::DeviceLocal};
}

MicroOp data_operation(
    OperationId id,
    MicroOpKind kind,
    DeviceId issuer,
    ThreadBlockId thread_block,
    std::uint32_t program_order,
    const std::vector<OperationId>& dependencies,
    const BufferSlice& source,
    const BufferSlice& destination,
    ReductionKind reduction,
    MemoryOrder memory_order,
    OrderingDomainId ordering_domain,
    CompletionStage release_stage) {
  return MicroOp{
      id,
      kind,
      issuer,
      thread_block,
      program_order,
      dependencies,
      memory_order,
      ordering_domain,
      release_stage,
      true,
      source,
      true,
      destination,
      false,
      DeviceId(),
      SemaphoreId(),
      0,
      reduction};
}

MicroOp semaphore_operation(
    OperationId id,
    MicroOpKind kind,
    DeviceId issuer,
    ThreadBlockId thread_block,
    std::uint32_t program_order,
    const std::vector<OperationId>& dependencies,
    DeviceId semaphore_owner,
    SemaphoreId semaphore,
    std::uint64_t value,
    MemoryOrder memory_order,
    OrderingDomainId ordering_domain,
    CompletionStage release_stage) {
  return MicroOp{
      id,
      kind,
      issuer,
      thread_block,
      program_order,
      dependencies,
      memory_order,
      ordering_domain,
      release_stage,
      false,
      empty_slice(),
      false,
      empty_slice(),
      true,
      semaphore_owner,
      semaphore,
      value,
      ReductionKind::None};
}

void append_all(ValidationErrors& destination, const ValidationErrors& source) {
  destination.insert(destination.end(), source.begin(), source.end());
}

}  // namespace

MicroOp MicroOp::put(
    OperationId id,
    DeviceId issuer,
    ThreadBlockId thread_block,
    std::uint32_t program_order,
    const std::vector<OperationId>& dependencies,
    const BufferSlice& source,
    const BufferSlice& destination,
    MemoryOrder memory_order,
    OrderingDomainId ordering_domain,
    CompletionStage release_stage) {
  return data_operation(
      id,
      MicroOpKind::Put,
      issuer,
      thread_block,
      program_order,
      dependencies,
      source,
      destination,
      ReductionKind::None,
      memory_order,
      ordering_domain,
      release_stage);
}

MicroOp MicroOp::get(
    OperationId id,
    DeviceId issuer,
    ThreadBlockId thread_block,
    std::uint32_t program_order,
    const std::vector<OperationId>& dependencies,
    const BufferSlice& source,
    const BufferSlice& destination,
    MemoryOrder memory_order,
    OrderingDomainId ordering_domain,
    CompletionStage release_stage) {
  return data_operation(
      id,
      MicroOpKind::Get,
      issuer,
      thread_block,
      program_order,
      dependencies,
      source,
      destination,
      ReductionKind::None,
      memory_order,
      ordering_domain,
      release_stage);
}

MicroOp MicroOp::signal(
    OperationId id,
    DeviceId issuer,
    ThreadBlockId thread_block,
    std::uint32_t program_order,
    const std::vector<OperationId>& dependencies,
    DeviceId semaphore_owner,
    SemaphoreId semaphore,
    std::uint64_t value,
    MemoryOrder memory_order,
    OrderingDomainId ordering_domain,
    CompletionStage release_stage) {
  return semaphore_operation(
      id,
      MicroOpKind::Signal,
      issuer,
      thread_block,
      program_order,
      dependencies,
      semaphore_owner,
      semaphore,
      value,
      memory_order,
      ordering_domain,
      release_stage);
}

MicroOp MicroOp::wait(
    OperationId id,
    DeviceId issuer,
    ThreadBlockId thread_block,
    std::uint32_t program_order,
    const std::vector<OperationId>& dependencies,
    DeviceId semaphore_owner,
    SemaphoreId semaphore,
    std::uint64_t value,
    MemoryOrder memory_order,
    OrderingDomainId ordering_domain,
    CompletionStage release_stage) {
  return semaphore_operation(
      id,
      MicroOpKind::Wait,
      issuer,
      thread_block,
      program_order,
      dependencies,
      semaphore_owner,
      semaphore,
      value,
      memory_order,
      ordering_domain,
      release_stage);
}

MicroOp MicroOp::reduce(
    OperationId id,
    DeviceId issuer,
    ThreadBlockId thread_block,
    std::uint32_t program_order,
    const std::vector<OperationId>& dependencies,
    const BufferSlice& source,
    const BufferSlice& destination,
    ReductionKind reduction,
    MemoryOrder memory_order,
    OrderingDomainId ordering_domain,
    CompletionStage release_stage) {
  return data_operation(
      id,
      MicroOpKind::Reduce,
      issuer,
      thread_block,
      program_order,
      dependencies,
      source,
      destination,
      reduction,
      memory_order,
      ordering_domain,
      release_stage);
}

ValidationErrors MicroOp::validate() const {
  ValidationErrors errors;
  if (!id.valid() || !issuer.valid() || !thread_block.valid() ||
      !ordering_domain.valid()) {
    append_error(
        errors,
        ValidationCode::InvalidIdentity,
        "operation, issuer, thread-block, and ordering-domain identities must be nonzero");
  }
  if (release_stage == CompletionStage::Issued ||
      release_stage == CompletionStage::Finalized) {
    append_error(
        errors,
        ValidationCode::InvalidCompletionOrder,
        "release stage must be local completion, remote arrival, or remote visibility");
  }

  std::unordered_set<OperationId> seen_dependencies;
  for (const OperationId dependency : dependencies) {
    if (!dependency.valid()) {
      append_error(
          errors,
          ValidationCode::InvalidIdentity,
          "operation dependency identity must be nonzero");
    } else if (dependency == id) {
      append_error(
          errors,
          ValidationCode::SelfDependency,
          "operation may not depend on itself");
    } else if (!seen_dependencies.insert(dependency).second) {
      append_error(
          errors,
          ValidationCode::DuplicateDependency,
          "operation has a duplicate dependency");
    }
  }

  if (has_source) {
    append_all(errors, source.validate());
  }
  if (has_destination) {
    append_all(errors, destination.validate());
  }

  const bool is_data_operation = kind == MicroOpKind::Put ||
      kind == MicroOpKind::Get || kind == MicroOpKind::Reduce;
  const bool is_semaphore_operation =
      kind == MicroOpKind::Signal || kind == MicroOpKind::Wait;

  if (is_data_operation) {
    if (!has_source || !has_destination || has_semaphore ||
        source.length_bytes != destination.length_bytes) {
      append_error(
          errors,
          ValidationCode::InvalidOperand,
          "data operation requires equal-length source and destination only");
    }
  } else if (is_semaphore_operation) {
    if (has_source || has_destination || !has_semaphore ||
        !semaphore_owner.valid() || !semaphore.valid()) {
      append_error(
          errors,
          ValidationCode::InvalidOperand,
          "semaphore operation requires owner, semaphore, and no buffers");
    }
  }

  if (kind == MicroOpKind::Put && has_source && has_destination &&
      (source.owner != issuer || destination.owner == issuer)) {
    append_error(
        errors,
        ValidationCode::InvalidOperand,
        "PUT source must be local and destination must be remote");
  }
  if (kind == MicroOpKind::Get && has_source && has_destination &&
      (source.owner == issuer || destination.owner != issuer)) {
    append_error(
        errors,
        ValidationCode::InvalidOperand,
        "GET source must be remote and destination must be local");
  }
  if (kind == MicroOpKind::Reduce) {
    if (reduction == ReductionKind::None || !has_source || !has_destination ||
        source.owner != issuer || destination.owner != issuer) {
      append_error(
          errors,
          ValidationCode::InvalidOperand,
          "REDUCE requires an explicit reduction and two local buffers");
    }
  } else if (reduction != ReductionKind::None) {
    append_error(
        errors,
        ValidationCode::InvalidOperand,
        "only REDUCE may name a reduction operation");
  }

  return errors;
}

void MicroOp::require_valid() const {
  require_no_errors(validate(), "MicroOp");
}

ValidationErrors CollectivePlan::validate() const {
  ValidationErrors errors;
  if (!id.valid()) {
    append_error(
        errors,
        ValidationCode::InvalidIdentity,
        "collective-plan identity must be nonzero");
  }
  append_all(errors, group.validate());
  if (payload_bytes == 0 || operations.empty()) {
    append_error(
        errors,
        ValidationCode::InvalidValue,
        "collective plan requires payload bytes and at least one operation");
  }

  std::unordered_map<OperationId, std::size_t> operation_index;
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint32_t>
      last_program_order;

  for (std::size_t index = 0; index < operations.size(); ++index) {
    const MicroOp& operation = operations[index];
    append_all(errors, operation.validate());
    if (!operation_index.insert(std::make_pair(operation.id, index)).second) {
      append_error(
          errors,
          ValidationCode::DuplicateIdentity,
          "collective plan contains a duplicate operation identity");
    }
    if (!group.contains(operation.issuer)) {
      append_error(
          errors,
          ValidationCode::EndpointOutsideGroup,
          "operation issuer is outside the parallel group");
    }
    if (operation.has_source && !group.contains(operation.source.owner)) {
      append_error(
          errors,
          ValidationCode::EndpointOutsideGroup,
          "operation source owner is outside the parallel group");
    }
    if (operation.has_destination &&
        !group.contains(operation.destination.owner)) {
      append_error(
          errors,
          ValidationCode::EndpointOutsideGroup,
          "operation destination owner is outside the parallel group");
    }
    if (operation.has_semaphore &&
        !group.contains(operation.semaphore_owner)) {
      append_error(
          errors,
          ValidationCode::EndpointOutsideGroup,
          "semaphore owner is outside the parallel group");
    }

    const std::pair<std::uint64_t, std::uint64_t> thread_key(
        operation.issuer.value(), operation.thread_block.value());
    const auto prior = last_program_order.find(thread_key);
    if (prior != last_program_order.end() &&
        operation.program_order <= prior->second) {
      append_error(
          errors,
          ValidationCode::InvalidProgramOrder,
          "thread-block program order must be strictly increasing");
    }
    last_program_order[thread_key] = operation.program_order;
  }

  for (const MicroOp& operation : operations) {
    for (const OperationId dependency : operation.dependencies) {
      if (operation_index.find(dependency) == operation_index.end()) {
        append_error(
            errors,
            ValidationCode::MissingDependency,
            "operation dependency does not exist in the plan");
      }
    }
  }

  std::vector<unsigned char> visit_state(operations.size(), 0);
  bool cycle_found = false;
  std::function<void(std::size_t)> visit = [&](std::size_t index) {
    if (cycle_found || visit_state[index] == 2) {
      return;
    }
    if (visit_state[index] == 1) {
      cycle_found = true;
      return;
    }
    visit_state[index] = 1;
    for (const OperationId dependency : operations[index].dependencies) {
      const auto found = operation_index.find(dependency);
      if (found != operation_index.end()) {
        visit(found->second);
      }
    }
    visit_state[index] = 2;
  };
  for (std::size_t index = 0; index < operations.size(); ++index) {
    visit(index);
  }
  if (cycle_found) {
    append_error(
        errors,
        ValidationCode::DependencyCycle,
        "collective plan contains a dependency cycle");
  }
  return errors;
}

void CollectivePlan::require_valid() const {
  require_no_errors(validate(), "CollectivePlan");
}

}  // namespace contracts
}  // namespace simai
