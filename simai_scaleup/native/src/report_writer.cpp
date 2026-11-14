#include "simai/runtime/report_writer.hpp"

#include <sstream>

namespace simai {
namespace runtime {

std::string format_report(const contracts::SimulationReport& report) {
  report.require_valid();
  std::ostringstream output;
  output << "schema_version=" << report.schema_version << '\n';
  output << "deterministic_seed=" << report.deterministic_seed << '\n';
  output << "end_to_end_ns=" << report.end_to_end_ns << '\n';
  for (const contracts::LayerTiming& layer : report.layers) {
    output << "layer=" << layer.layer_index
           << ",name=" << layer.name
           << ",start_ns=" << layer.start_ns
           << ",compute_ns=" << layer.compute_duration_ns
           << ",compute_complete_ns=" << layer.compute_complete_ns
           << ",communication_issue_ns=" << layer.communication_issue_ns
           << ",communication_complete_ns="
           << layer.communication_complete_ns
           << ",exposed_communication_ns="
           << layer.exposed_communication_ns
           << ",finish_ns=" << layer.finish_ns
           << ",transferred_payload_bytes="
           << layer.transferred_payload_bytes << '\n';
  }
  for (const contracts::RailUtilization& rail : report.rails) {
    output << "rail=" << rail.rail_index
           << ",pdus=" << rail.pdus
           << ",pdu_payload_bytes=" << rail.pdu_payload_bytes
           << ",wire_bytes=" << rail.wire_bytes
           << ",busy_time_ns=" << rail.busy_time_ns
           << ",transmission_attempts=" << rail.transmission_attempts
           << ",retries=" << rail.retries
           << ",transmitted_wire_bytes="
           << rail.transmitted_wire_bytes << '\n';
  }
  for (const contracts::SwitchQueueUtilization& queue :
       report.switch_queues) {
    output << "switch_egress=" << queue.egress_endpoint
           << ",accepted_pdus=" << queue.accepted_pdus
           << ",departed_pdus=" << queue.departed_pdus
           << ",pdu_payload_bytes=" << queue.pdu_payload_bytes
           << ",wire_bytes=" << queue.wire_bytes
           << ",maximum_occupancy_pdus="
           << queue.maximum_occupancy_pdus
           << ",final_occupancy_pdus=" << queue.final_occupancy_pdus
           << ",backpressure_events=" << queue.backpressure_events
           << ",total_admission_delay_ns="
           << queue.total_admission_delay_ns
           << ",busy_time_ns=" << queue.busy_time_ns
           << ",initial_credits=" << queue.initial_credits
           << ",final_available_credits="
           << queue.final_available_credits
           << ",credits_consumed=" << queue.credits_consumed
           << ",credits_returned=" << queue.credits_returned
           << ",maximum_outstanding_credits="
           << queue.maximum_outstanding_credits
           << ",credit_starvation_events="
           << queue.credit_starvation_events
           << ",total_credit_starvation_ns="
           << queue.total_credit_starvation_ns
           << ",duplicate_pdus=" << queue.duplicate_pdus << '\n';
  }
  for (const contracts::ScheduleRecord& schedule : report.schedules) {
    output << "schedule=layer:" << schedule.layer_index
           << ",source:" << schedule.source
           << ",algorithm:" << schedule.algorithm
           << ",version:" << schedule.version << '\n';
  }
  for (const contracts::GpuUtilization& gpu : report.gpus) {
    output << "gpu=" << gpu.endpoint
           << ",compute_units=" << gpu.compute_units
           << ",workgroup_slots_per_compute_unit="
           << gpu.workgroup_slots_per_compute_unit
           << ",workgroups_dispatched=" << gpu.workgroups_dispatched
           << ",wavefronts_issued=" << gpu.wavefronts_issued
           << ",maximum_resident_workgroups="
           << gpu.maximum_resident_workgroups
           << ",resource_stall_events=" << gpu.resource_stall_events
           << ",total_resource_stall_ns=" << gpu.total_resource_stall_ns
           << ",wavefront_issue_busy_ns=" << gpu.wavefront_issue_busy_ns
           << ",reduction_busy_ns=" << gpu.reduction_busy_ns << '\n';
  }
  output << "counters="
         << "issued_operations:" << report.counters.issued_operations
         << ",finalized_operations:" << report.counters.finalized_operations
         << ",transactions:" << report.counters.transactions
         << ",logical_transaction_bytes:"
         << report.counters.logical_transaction_bytes
         << ",pdus:" << report.counters.pdus
         << ",packets:" << report.counters.packets
         << ",credits_consumed:" << report.counters.credits_consumed
         << ",credits_returned:" << report.counters.credits_returned
         << ",retries:" << report.counters.retries
         << ",completions:" << report.counters.completions
         << ",logical_operation_bytes:"
         << report.counters.logical_operation_bytes
         << ",pdu_payload_bytes:" << report.counters.pdu_payload_bytes
         << ",wire_bytes:" << report.counters.wire_bytes
         << ",transmitted_wire_bytes:"
         << report.counters.transmitted_wire_bytes << '\n';

  return output.str();
}

}  // namespace runtime
}  // namespace simai
