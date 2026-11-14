#ifndef SIMAI_CONTRACTS_COMPLETION_HPP
#define SIMAI_CONTRACTS_COMPLETION_HPP

#include <vector>

#include "simai/contracts/completion_stage.hpp"
#include "simai/contracts/ids.hpp"
#include "simai/contracts/validation.hpp"

namespace simai {
namespace contracts {

struct CompletionRecord {
  OperationId operation;
  CompletionStage stage;
  SimTimeNs time_ns;
};

class CompletionTimeline {
 public:
  CompletionTimeline(OperationId operation, CompletionStage release_stage);

  ValidationErrors can_record(CompletionStage stage, SimTimeNs time_ns) const;
  void record(CompletionStage stage, SimTimeNs time_ns);

  OperationId operation() const { return operation_; }
  CompletionStage release_stage() const { return release_stage_; }
  const std::vector<CompletionRecord>& records() const { return records_; }
  bool finalized() const;

 private:
  OperationId operation_;
  CompletionStage release_stage_;
  std::vector<CompletionRecord> records_;
};

}  // namespace contracts
}  // namespace simai

#endif
