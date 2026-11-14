#include "simai/contracts/fine_remote.hpp"

namespace simai {
namespace contracts {
namespace {

void append_reference(
    ValidationErrors& errors,
    bool present,
    const FineMemoryReference& reference) {
  if (!present) return;
  const ValidationErrors reference_errors = reference.validate();
  errors.insert(errors.end(), reference_errors.begin(), reference_errors.end());
}

}  // namespace

ValidationErrors FineRemoteRequest::validate() const {
  ValidationErrors errors;
  if (!id.valid() || !kernel_instance.valid() ||
      !workgroup_instance.valid() || !wavefront_instance.valid() ||
      !instruction.valid() || !issuer.valid()) {
    append_error(
        errors,
        ValidationCode::InvalidIdentity,
        "fine remote request identities must be nonzero");
  }
  append_reference(errors, has_source, source);
  append_reference(errors, has_destination, destination);

  if (kind == FineMemoryRequestKind::RemoteLoadRequest) {
    if (!has_source || !has_destination || has_semaphore ||
        source.space != FineMemorySpace::RemoteHbm || source.owner == issuer ||
        destination.space != FineMemorySpace::RegisterFile ||
        destination.owner != issuer ||
        source.length_bytes != destination.length_bytes ||
        completion_stage != CompletionStage::Finalized) {
      append_error(
          errors,
          ValidationCode::InvalidTransaction,
          "fine remote Load must return matching bytes to issuer registers");
    }
  } else if (kind == FineMemoryRequestKind::RemoteStore) {
    if (!has_source || !has_destination || has_semaphore ||
        source.space != FineMemorySpace::RegisterFile || source.owner != issuer ||
        destination.space != FineMemorySpace::RemoteHbm ||
        destination.owner == issuer ||
        source.length_bytes != destination.length_bytes ||
        completion_stage != CompletionStage::RemoteVisible) {
      append_error(
          errors,
          ValidationCode::InvalidTransaction,
          "fine remote Store must write matching issuer bytes remotely");
    }
  } else if (kind == FineMemoryRequestKind::SemaphoreAcquire ||
             kind == FineMemoryRequestKind::SemaphoreRelease) {
    const FineMemoryReference& location =
        kind == FineMemoryRequestKind::SemaphoreAcquire ? source : destination;
    if (!has_semaphore || !semaphore_owner.valid() || !semaphore.valid() ||
        semaphore_generation == 0 || semaphore_owner == issuer ||
        location.space != FineMemorySpace::SemaphoreMemory ||
        location.owner != semaphore_owner || location.length_bytes > 8 ||
        (kind == FineMemoryRequestKind::SemaphoreAcquire &&
         (!has_source || has_destination ||
          completion_stage != CompletionStage::Finalized)) ||
        (kind == FineMemoryRequestKind::SemaphoreRelease &&
         (has_source || !has_destination ||
          completion_stage != CompletionStage::RemoteVisible))) {
      append_error(
          errors,
          ValidationCode::InvalidTransaction,
          "fine remote semaphore request is inconsistent");
    }
  } else {
    append_error(
        errors,
        ValidationCode::InvalidTransaction,
        "fine remote request kind is unsupported");
  }
  return errors;
}

void FineRemoteRequest::require_valid() const {
  require_no_errors(validate(), "FineRemoteRequest");
}

}  // namespace contracts
}  // namespace simai
