#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "simai/contracts/buffer.hpp"
#include "simai/contracts/collective.hpp"
#include "simai/contracts/completion.hpp"
#include "simai/contracts/event_queue.hpp"
#include "simai/contracts/fine_gpu.hpp"
#include "simai/contracts/report.hpp"
#include "simai/contracts/sue_transaction.hpp"
#include "simai/contracts/workload.hpp"

namespace {

using namespace simai::contracts;

int failures = 0;

void expect(bool condition, const std::string& name) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL: " << name << '\n';
  }
}

void expect_invalid(
    const std::function<void()>& action,
    const std::string& name) {
  try {
    action();
    expect(false, name);
  } catch (const std::invalid_argument&) {
    expect(true, name);
  }
}

BufferSlice slice(
    std::uint64_t device,
    std::uint64_t buffer,
    std::uint64_t offset,
    std::uint64_t length) {
  return BufferSlice{
      DeviceId(device),
      BufferId(buffer),
      offset,
      length,
      MemoryRegion::DeviceLocal};
}

void test_buffer_and_workload_contracts() {
  const BufferSlice valid = slice(1, 10, 64, 256);
  expect(valid.validate().empty(), "valid buffer slice");

  BufferSlice overflow = valid;
  overflow.offset_bytes = std::numeric_limits<std::uint64_t>::max() - 7;
  overflow.length_bytes = 8;
  expect(!overflow.validate().empty(), "buffer overflow rejected");

  const WorkloadEvent compute = WorkloadEvent::compute(
      WorkloadEventId(1), 0, {}, DeviceId(1), 50);
  expect(compute.validate().empty(), "valid compute workload event");

  const WorkloadEvent collective = WorkloadEvent::collective(
      WorkloadEventId(2),
      0,
      {WorkloadEventId(1)},
      CollectivePlanId(1));
  expect(collective.validate().empty(), "valid collective workload event");

  WorkloadEvent self_dependent = compute;
  self_dependent.dependencies.push_back(self_dependent.id);
  expect(
      !self_dependent.validate().empty(),
      "workload self dependency rejected");
}

void test_parallel_group_and_micro_operations() {
  const ParallelGroup group{
      ParallelGroupId(1), {DeviceId(1), DeviceId(2)}};
  expect(group.validate().empty(), "valid parallel group");

  const ParallelGroup duplicate{
      ParallelGroupId(2), {DeviceId(1), DeviceId(1)}};
  expect(!duplicate.validate().empty(), "duplicate group member rejected");

  const BufferSlice local_source = slice(1, 10, 0, 256);
  const BufferSlice remote_destination = slice(2, 20, 0, 256);
  const BufferSlice remote_source = slice(2, 21, 0, 256);
  const BufferSlice local_destination = slice(1, 11, 0, 256);

  const MicroOp put = MicroOp::put(
      OperationId(1),
      DeviceId(1),
      ThreadBlockId(1),
      0,
      {},
      local_source,
      remote_destination,
      MemoryOrder::Relaxed,
      OrderingDomainId(1),
      CompletionStage::RemoteVisible);
  expect(put.validate().empty(), "valid PUT operation");

  const MicroOp get = MicroOp::get(
      OperationId(2),
      DeviceId(1),
      ThreadBlockId(2),
      0,
      {},
      remote_source,
      local_destination,
      MemoryOrder::Ordered,
      OrderingDomainId(1),
      CompletionStage::LocalComplete);
  expect(get.validate().empty(), "valid GET operation");

  const MicroOp reduce = MicroOp::reduce(
      OperationId(3),
      DeviceId(1),
      ThreadBlockId(2),
      1,
      {OperationId(2)},
      local_source,
      local_destination,
      ReductionKind::Sum,
      MemoryOrder::Ordered,
      OrderingDomainId(1),
      CompletionStage::LocalComplete);
  expect(reduce.validate().empty(), "valid REDUCE operation");

  const MicroOp signal = MicroOp::signal(
      OperationId(4),
      DeviceId(1),
      ThreadBlockId(1),
      1,
      {OperationId(1)},
      DeviceId(2),
      SemaphoreId(1),
      7,
      MemoryOrder::Ordered,
      OrderingDomainId(1),
      CompletionStage::RemoteVisible);
  expect(signal.validate().empty(), "valid SIGNAL operation");

  const MicroOp wait = MicroOp::wait(
      OperationId(5),
      DeviceId(2),
      ThreadBlockId(1),
      0,
      {OperationId(4)},
      DeviceId(2),
      SemaphoreId(1),
      7,
      MemoryOrder::Ordered,
      OrderingDomainId(1),
      CompletionStage::LocalComplete);
  expect(wait.validate().empty(), "valid WAIT operation");

  MicroOp wrong_put = put;
  wrong_put.source.owner = DeviceId(2);
  expect(!wrong_put.validate().empty(), "nonlocal PUT source rejected");

  const CollectivePlan plan{
      CollectivePlanId(1),
      CollectiveKind::AllReduce,
      group,
      256,
      {put, signal, wait}};
  expect(plan.validate().empty(), "valid collective dependency DAG");

  CollectivePlan missing_dependency = plan;
  missing_dependency.operations[2].dependencies = {OperationId(99)};
  expect(
      !missing_dependency.validate().empty(),
      "missing operation dependency rejected");

  CollectivePlan cycle = plan;
  cycle.operations[0].dependencies = {OperationId(5)};
  expect(!cycle.validate().empty(), "operation dependency cycle rejected");

  CollectivePlan bad_order = plan;
  bad_order.operations[1].program_order = 0;
  expect(
      !bad_order.validate().empty(),
      "non-increasing thread-block order rejected");
}

void test_completion_timeline() {
  CompletionTimeline timeline(
      OperationId(1), CompletionStage::RemoteVisible);
  timeline.record(CompletionStage::Issued, 10);
  timeline.record(CompletionStage::LocalComplete, 10);
  timeline.record(CompletionStage::RemoteArrived, 20);
  timeline.record(CompletionStage::RemoteVisible, 25);
  timeline.record(CompletionStage::Finalized, 25);
  expect(timeline.finalized(), "five-stage completion timeline finalized");
  expect(timeline.records().size() == 5, "all completion stages retained");

  CompletionTimeline skipped(OperationId(2), CompletionStage::LocalComplete);
  expect_invalid(
      [&skipped]() {
        skipped.record(CompletionStage::LocalComplete, 1);
      },
      "skipped completion stage rejected");

  CompletionTimeline backward(
      OperationId(3), CompletionStage::RemoteArrived);
  backward.record(CompletionStage::Issued, 9);
  expect_invalid(
      [&backward]() {
        backward.record(CompletionStage::LocalComplete, 8);
      },
      "backward completion time rejected");
}

void test_sue_transactions() {
  const BufferSlice local_source = slice(1, 10, 0, 256);
  const BufferSlice remote_destination = slice(2, 20, 0, 256);
  const BufferSlice remote_source = slice(2, 21, 0, 256);
  const BufferSlice local_destination = slice(1, 11, 0, 256);

  const SueTransaction put_command = SueTransaction::put_command(
      TransactionId(1),
      OperationId(1),
      local_source,
      remote_destination,
      0,
      OrderingDomainId(1));
  expect(put_command.validate().empty(), "valid SUE PUT command");

  const SueTransaction put_response = SueTransaction::put_response(
      TransactionId(2),
      OperationId(1),
      DeviceId(2),
      DeviceId(1),
      0,
      OrderingDomainId(1),
      SueResponseStatus::Success);
  expect(put_response.validate().empty(), "valid SUE PUT response");

  const SueTransaction get_command = SueTransaction::get_command(
      TransactionId(3),
      OperationId(2),
      DeviceId(1),
      remote_source,
      local_destination,
      3,
      OrderingDomainId(1));
  expect(get_command.validate().empty(), "valid SUE GET command");

  const SueTransaction get_response = SueTransaction::get_response(
      TransactionId(4),
      OperationId(2),
      remote_source,
      local_destination,
      3,
      OrderingDomainId(1),
      SueResponseStatus::Success);
  expect(get_response.validate().empty(), "valid SUE GET response");

  SueTransaction bad_channel = put_command;
  bad_channel.virtual_channel = 4;
  expect(
      !bad_channel.validate().empty(),
      "out-of-range virtual channel rejected");

  SueTransaction oversized = put_command;
  oversized.logical_bytes = 257;
  oversized.source.length_bytes = 257;
  oversized.destination.length_bytes = 257;
  expect(
      !oversized.validate().empty(),
      "oversized SUE service transaction rejected");

  SueTransaction failed_get = get_response;
  failed_get.response_status = SueResponseStatus::Failed;
  expect(
      !failed_get.validate().empty(),
      "failed GET response may not carry data");
}

void test_deterministic_event_queue() {
  DeterministicEventQueue queue;
  queue.schedule(EventId(1), 10, EventKind::WorkloadReady);
  queue.schedule(EventId(2), 5, EventKind::MicroOpReady);
  queue.schedule(EventId(3), 10, EventKind::CompletionReady);

  expect(queue.pop().id == EventId(2), "earlier timestamp wins");
  expect(queue.pop().id == EventId(1), "equal-time insertion order first");
  expect(queue.pop().id == EventId(3), "equal-time insertion order second");
  expect(queue.now() == 10, "event queue advances integer time");

  expect_invalid(
      [&queue]() {
        queue.schedule(EventId(4), 9, EventKind::SueTransactionReady);
      },
      "event scheduling into past rejected");
}

FineMemoryReference fine_memory(
    std::uint64_t device,
    FineMemorySpace space,
    std::uint64_t buffer,
    std::uint64_t offset,
    std::uint64_t length,
    std::uint64_t alignment = 128) {
  return FineMemoryReference{
      DeviceId(device),
      space,
      BufferId(buffer),
      offset,
      length,
      alignment};
}

void test_fine_gpu_contracts() {
  const FineMemoryReference local_hbm = fine_memory(
      1, FineMemorySpace::LocalHbm, 100, 0, 256);
  const FineMemoryReference registers = fine_memory(
      1, FineMemorySpace::RegisterFile, 101, 0, 256);
  const FineMemoryReference remote_hbm = fine_memory(
      2, FineMemorySpace::RemoteHbm, 200, 0, 256);
  const FineMemoryReference remote_semaphore = fine_memory(
      2, FineMemorySpace::SemaphoreMemory, 201, 0, 128);

  expect(local_hbm.validate().empty(), "valid fine local memory reference");
  FineMemoryReference unaligned = local_hbm;
  unaligned.offset_bytes = 64;
  expect(
      !unaligned.validate().empty(),
      "unaligned fine memory reference rejected");

  const FineGpuOperation load = FineGpuOperation::load(
      GpuOperationId(1), local_hbm, registers);
  const FineGpuOperation store = FineGpuOperation::store(
      GpuOperationId(2), registers, remote_hbm);
  const FineGpuOperation copy = FineGpuOperation::memcpy(
      GpuOperationId(3), local_hbm, remote_hbm);
  const FineGpuOperation release = FineGpuOperation::semaphore_release(
      GpuOperationId(4),
      DeviceId(2),
      SemaphoreId(1),
      1,
      remote_semaphore);
  const FineGpuOperation reduce =
      FineGpuOperation::reduce(GpuOperationId(5), 20);
  const FineGpuOperation barrier = FineGpuOperation::workgroup_barrier(
      GpuOperationId(6), BarrierId(1));

  expect(load.validate().empty(), "valid fine Load operation");
  expect(store.validate().empty(), "valid fine Store operation");
  expect(copy.validate().empty(), "valid fine Memcpy operation");
  expect(release.validate().empty(), "valid fine semaphore release");
  expect(reduce.validate().empty(), "valid fine Reduce operation");
  expect(barrier.validate().empty(), "valid fine workgroup barrier");

  FineGpuOperation invalid_reduce = reduce;
  invalid_reduce.reduce_cycles = 0;
  expect(
      !invalid_reduce.validate().empty(),
      "zero-cycle fine Reduce rejected");

  const FineWavefrontProgram wavefront_one{
      WavefrontId(1), {load, store, release, reduce, barrier}};
  const FineWavefrontProgram wavefront_two{
      WavefrontId(2),
      {FineGpuOperation::memcpy(
          GpuOperationId(7), local_hbm, remote_hbm)}};
  const FineWorkgroupDefinition workgroup{
      WorkgroupId(1),
      ThreadBlockId(1),
      FineWorkgroupResources{1},
      {wavefront_one, wavefront_two}};
  const FineKernelDefinition kernel{
      KernelId(1),
      DeviceId(1),
      FineKernelKind::Communication,
      {workgroup}};
  expect(kernel.validate().empty(), "valid fine GPU kernel definition");

  FineKernelDefinition duplicate_wavefront = kernel;
  duplicate_wavefront.workgroups.push_back(FineWorkgroupDefinition{
      WorkgroupId(2),
      ThreadBlockId(2),
      FineWorkgroupResources{1},
      {wavefront_one}});
  expect(
      !duplicate_wavefront.validate().empty(),
      "globally duplicate fine wavefront identity rejected");

  FineKernelDefinition wrong_local_owner = kernel;
  wrong_local_owner.workgroups[0].wavefronts[0].operations[0].source.owner =
      DeviceId(2);
  expect(
      !wrong_local_owner.validate().empty(),
      "fine local memory owned by another device rejected");

  FineKernelDefinition wrong_remote_owner = kernel;
  wrong_remote_owner.workgroups[0].wavefronts[0].operations[1]
      .destination.owner = DeviceId(1);
  expect(
      !wrong_remote_owner.validate().empty(),
      "fine remote memory owned by local device rejected");

  FineWorkgroupDefinition empty_workgroup = workgroup;
  empty_workgroup.wavefronts.clear();
  expect(
      !empty_workgroup.validate().empty(),
      "fine workgroup without wavefronts rejected");
}

void test_report_contract() {
  const ConservationCounters counters{
      3, 3, 2, 512, 0, 0, 0, 0, 0, 3, 512, 0, 0, 0};
  const SimulationReport report{
      1,
      1234,
      100,
      {LayerTiming{0, "layer_0", 0, 40, 40, 80, 80, 40, 40, 512},
       LayerTiming{1, "layer_1", 80, 90, 90, 100, 100, 10, 10, 0}},
      counters,
      {},
      {},
      {},
      {}};
  expect(report.validate().empty(), "valid simulation report");

  SimulationReport invalid = report;
  invalid.counters.finalized_operations = 4;
  expect(!invalid.validate().empty(), "report conservation violation rejected");
}

}  // namespace

int main() {
  test_buffer_and_workload_contracts();
  test_parallel_group_and_micro_operations();
  test_completion_timeline();
  test_sue_transactions();
  test_deterministic_event_queue();
  test_fine_gpu_contracts();
  test_report_contract();

  if (failures != 0) {
    std::cerr << failures << " contract test(s) failed\n";
    return 1;
  }
  std::cout << "All native contract tests passed\n";
  return 0;
}
