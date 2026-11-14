#include "simai/contracts/completion.hpp"

namespace simai {
namespace contracts {

CompletionTimeline::CompletionTimeline(
    OperationId operation,
    CompletionStage release_stage)
    : operation_(operation), release_stage_(release_stage), records_() {
  ValidationErrors errors;
  if (!operation_.valid()) {
    append_error(
        errors,
        ValidationCode::InvalidIdentity,
        "completion timeline requires a nonzero operation identity");
  }
  if (release_stage_ == CompletionStage::Issued ||
      release_stage_ == CompletionStage::Finalized) {
    append_error(
        errors,
        ValidationCode::InvalidCompletionOrder,
        "completion release stage must precede finalization");
  }
  require_no_errors(errors, "CompletionTimeline");
}

ValidationErrors CompletionTimeline::can_record(
    CompletionStage stage,
    SimTimeNs time_ns) const {
  ValidationErrors errors;
  const std::uint8_t expected = static_cast<std::uint8_t>(records_.size());
  if (completion_stage_index(stage) != expected) {
    append_error(
        errors,
        ValidationCode::InvalidCompletionOrder,
        "completion stages must be recorded once and without gaps");
  }
  if (!records_.empty() && time_ns < records_.back().time_ns) {
    append_error(
        errors,
        ValidationCode::InvalidCompletionTime,
        "completion timestamp may not move backward");
  }
  if (stage == CompletionStage::Finalized &&
      completion_stage_index(release_stage_) >=
          completion_stage_index(CompletionStage::Finalized)) {
    append_error(
        errors,
        ValidationCode::InvalidCompletionOrder,
        "finalization requires a preceding configured release stage");
  }
  return errors;
}

void CompletionTimeline::record(CompletionStage stage, SimTimeNs time_ns) {
  require_no_errors(can_record(stage, time_ns), "CompletionTimeline::record");
  records_.push_back(CompletionRecord{operation_, stage, time_ns});
}

bool CompletionTimeline::finalized() const {
  return !records_.empty() &&
      records_.back().stage == CompletionStage::Finalized;
}

}  // namespace contracts
}  // namespace simai
