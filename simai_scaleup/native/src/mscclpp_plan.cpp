#include "simai/runtime/mscclpp_plan.hpp"

#include <cctype>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace simai {
namespace runtime {
namespace {

struct JsonValue {
  enum class Kind { Null, Boolean, Number, String, Array, Object };
  Kind kind = Kind::Null;
  bool boolean = false;
  std::uint64_t number = 0;
  std::string string;
  std::vector<JsonValue> array;
  std::map<std::string, JsonValue> object;
};

class JsonParser {
 public:
  explicit JsonParser(const std::string& text) : text_(text), position_(0) {}

  JsonValue parse() {
    JsonValue value = parse_value();
    skip_space();
    if (position_ != text_.size()) {
      fail("trailing content");
    }
    return value;
  }

 private:
  JsonValue parse_value() {
    skip_space();
    if (position_ == text_.size()) fail("unexpected end of input");
    const char token = text_[position_];
    if (token == '{') return parse_object();
    if (token == '[') return parse_array();
    if (token == '"') {
      JsonValue value;
      value.kind = JsonValue::Kind::String;
      value.string = parse_string();
      return value;
    }
    if (token >= '0' && token <= '9') return parse_number();
    if (consume("true")) {
      JsonValue value;
      value.kind = JsonValue::Kind::Boolean;
      value.boolean = true;
      return value;
    }
    if (consume("false")) {
      JsonValue value;
      value.kind = JsonValue::Kind::Boolean;
      return value;
    }
    if (consume("null")) return JsonValue();
    fail("unsupported token");
    return JsonValue();
  }

  JsonValue parse_object() {
    JsonValue value;
    value.kind = JsonValue::Kind::Object;
    ++position_;
    skip_space();
    if (take('}')) return value;
    while (true) {
      skip_space();
      if (position_ == text_.size() || text_[position_] != '"') {
        fail("object key must be a string");
      }
      const std::string key = parse_string();
      skip_space();
      if (!take(':')) fail("expected ':'");
      if (!value.object.emplace(key, parse_value()).second) {
        fail("duplicate object key");
      }
      skip_space();
      if (take('}')) return value;
      if (!take(',')) fail("expected ',' or '}'");
    }
  }

  JsonValue parse_array() {
    JsonValue value;
    value.kind = JsonValue::Kind::Array;
    ++position_;
    skip_space();
    if (take(']')) return value;
    while (true) {
      value.array.push_back(parse_value());
      skip_space();
      if (take(']')) return value;
      if (!take(',')) fail("expected ',' or ']'");
    }
  }

  JsonValue parse_number() {
    JsonValue value;
    value.kind = JsonValue::Kind::Number;
    while (position_ < text_.size() && std::isdigit(text_[position_])) {
      const std::uint64_t digit = static_cast<std::uint64_t>(text_[position_] - '0');
      if (value.number > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
        fail("integer overflow");
      }
      value.number = value.number * 10 + digit;
      ++position_;
    }
    return value;
  }

  std::string parse_string() {
    ++position_;
    std::string result;
    while (position_ < text_.size()) {
      const char character = text_[position_++];
      if (character == '"') return result;
      if (character == '\\') {
        if (position_ == text_.size()) fail("incomplete escape");
        const char escaped = text_[position_++];
        if (escaped == '"' || escaped == '\\' || escaped == '/') result += escaped;
        else if (escaped == 'n') result += '\n';
        else if (escaped == 'r') result += '\r';
        else if (escaped == 't') result += '\t';
        else fail("unsupported string escape");
      } else {
        result += character;
      }
    }
    fail("unterminated string");
    return std::string();
  }

  bool take(char expected) {
    if (position_ < text_.size() && text_[position_] == expected) {
      ++position_;
      return true;
    }
    return false;
  }

  bool consume(const char* word) {
    const std::string expected(word);
    if (text_.compare(position_, expected.size(), expected) != 0) return false;
    position_ += expected.size();
    return true;
  }

  void skip_space() {
    while (position_ < text_.size() && std::isspace(text_[position_])) ++position_;
  }

  [[noreturn]] void fail(const std::string& message) const {
    throw std::invalid_argument(
        "invalid MSCCL++ JSON at byte " + std::to_string(position_) + ": " + message);
  }

  const std::string& text_;
  std::size_t position_;
};

const JsonValue& field(const JsonValue& object, const std::string& name) {
  if (object.kind != JsonValue::Kind::Object) {
    throw std::invalid_argument("MSCCL++ value containing '" + name + "' is not an object");
  }
  const auto found = object.object.find(name);
  if (found == object.object.end()) {
    throw std::invalid_argument("MSCCL++ plan is missing '" + name + "'");
  }
  return found->second;
}

const JsonValue* optional_field(const JsonValue& object, const std::string& name) {
  const auto found = object.object.find(name);
  return found == object.object.end() ? nullptr : &found->second;
}

std::string string_field(const JsonValue& object, const std::string& name) {
  const JsonValue& value = field(object, name);
  if (value.kind != JsonValue::Kind::String) {
    throw std::invalid_argument("MSCCL++ field '" + name + "' must be a string");
  }
  return value.string;
}

std::uint64_t number_field(const JsonValue& object, const std::string& name) {
  const JsonValue& value = field(object, name);
  if (value.kind != JsonValue::Kind::Number) {
    throw std::invalid_argument("MSCCL++ field '" + name + "' must be an integer");
  }
  return value.number;
}

bool bool_field(const JsonValue& object, const std::string& name) {
  const JsonValue& value = field(object, name);
  if (value.kind != JsonValue::Kind::Boolean) {
    throw std::invalid_argument("MSCCL++ field '" + name + "' must be boolean");
  }
  return value.boolean;
}

const std::vector<JsonValue>& array_field(const JsonValue& object, const std::string& name) {
  const JsonValue& value = field(object, name);
  if (value.kind != JsonValue::Kind::Array) {
    throw std::invalid_argument("MSCCL++ field '" + name + "' must be an array");
  }
  return value.array;
}

std::vector<std::uint64_t> number_array(const JsonValue& object, const std::string& name) {
  std::vector<std::uint64_t> result;
  for (const JsonValue& value : array_field(object, name)) {
    if (value.kind != JsonValue::Kind::Number) {
      throw std::invalid_argument("MSCCL++ field '" + name + "' must contain integers");
    }
    result.push_back(value.number);
  }
  return result;
}

std::vector<std::string> string_array(const JsonValue& object, const std::string& name) {
  std::vector<std::string> result;
  for (const JsonValue& value : array_field(object, name)) {
    if (value.kind != JsonValue::Kind::String) {
      throw std::invalid_argument("MSCCL++ field '" + name + "' must contain strings");
    }
    result.push_back(value.string);
  }
  return result;
}

MscclppBufferRef parse_buffer(const JsonValue& value) {
  const JsonValue* type = optional_field(value, "type");
  const JsonValue* buffer_id = optional_field(value, "buffer_id");
  if ((type == nullptr) == (buffer_id == nullptr)) {
    throw std::invalid_argument("MSCCL++ buffer must name exactly one local type or remote buffer_id");
  }
  MscclppBufferRef result;
  result.remote = buffer_id != nullptr;
  result.type = result.remote ? std::string() : string_field(value, "type");
  result.buffer_id = result.remote ? number_field(value, "buffer_id") : 0;
  result.index = number_field(value, "index");
  result.size = number_field(value, "size");
  if (result.size == 0) throw std::invalid_argument("MSCCL++ buffer size must be nonzero");
  return result;
}

std::vector<MscclppBufferRef> parse_buffers(const JsonValue& operation, const std::string& name) {
  std::vector<MscclppBufferRef> result;
  const JsonValue* values = optional_field(operation, name);
  if (values == nullptr) return result;
  if (values->kind != JsonValue::Kind::Array) {
    throw std::invalid_argument("MSCCL++ field '" + name + "' must be an array");
  }
  for (const JsonValue& value : values->array) result.push_back(parse_buffer(value));
  return result;
}

MscclppOperation parse_operation(const JsonValue& value) {
  MscclppOperation operation;
  operation.name = string_field(value, "name");
  const JsonValue* channel_type = optional_field(value, "channel_type");
  operation.channel_type = channel_type == nullptr ? std::string() : string_field(value, "channel_type");
  if (optional_field(value, "channel_ids") != nullptr) {
    operation.channel_ids = number_array(value, "channel_ids");
  }
  operation.sources = parse_buffers(value, "src_buff");
  operation.destinations = parse_buffers(value, "dst_buff");
  const JsonValue* reduce_op = optional_field(value, "reduce_op");
  operation.reduce_op = reduce_op == nullptr ? std::string() : string_field(value, "reduce_op");
  return operation;
}

contracts::CollectiveKind collective_kind(const std::string& name) {
  if (name == "allreduce") return contracts::CollectiveKind::AllReduce;
  if (name == "reduce_scatter") return contracts::CollectiveKind::ReduceScatter;
  if (name == "allgather") return contracts::CollectiveKind::AllGather;
  if (name == "alltoall") return contracts::CollectiveKind::AllToAll;
  if (name == "broadcast") return contracts::CollectiveKind::Broadcast;
  throw std::invalid_argument("unsupported MSCCL++ collective '" + name + "'");
}

contracts::BufferId local_buffer_id(std::uint64_t rank, const std::string& type) {
  std::uint64_t base = 0;
  if (type == "i") base = 1000;
  else if (type == "o") base = 2000;
  else if (type == "s") base = 3000;
  else throw std::invalid_argument("unsupported MSCCL++ buffer type '" + type + "'");
  return contracts::BufferId(base + rank + 1);
}

contracts::BufferSlice buffer_slice(
    const MscclppPlan& plan,
    const MscclppGpu& gpu,
    const MscclppBufferRef& reference,
    std::uint64_t chunk_bytes) {
  std::uint64_t owner = gpu.id;
  std::string type = reference.type;
  if (reference.remote) {
    if (reference.buffer_id >= gpu.remote_buffers.size()) {
      throw std::invalid_argument("MSCCL++ remote buffer id is out of range");
    }
    const MscclppRemoteBuffer& remote = gpu.remote_buffers[reference.buffer_id];
    owner = remote.rank;
    type = remote.type;
  }
  if (owner >= plan.gpus.size()) throw std::invalid_argument("MSCCL++ buffer rank is out of range");
  if (reference.index > std::numeric_limits<std::uint64_t>::max() / chunk_bytes ||
      reference.size > std::numeric_limits<std::uint64_t>::max() / chunk_bytes) {
    throw std::invalid_argument("MSCCL++ buffer byte range overflows");
  }
  return contracts::BufferSlice{
      contracts::DeviceId(owner + 1), local_buffer_id(owner, type),
      reference.index * chunk_bytes, reference.size * chunk_bytes,
      contracts::MemoryRegion::DeviceLocal};
}

std::uint64_t peer_for_channel(
    const MscclppGpu& gpu, std::uint64_t channel_id) {
  if (channel_id >= gpu.channels.size() ||
      gpu.channels[channel_id].type != "memory" ||
      gpu.channels[channel_id].connected_to.size() != 1) {
    throw std::invalid_argument(
        "safe MSCCL++ subset requires one peer on each memory channel");
  }
  return gpu.channels[channel_id].connected_to.front();
}

void require_single_buffers(const MscclppOperation& operation) {
  if (operation.sources.size() != 1 || operation.destinations.size() != 1) {
    throw std::invalid_argument(
        "safe MSCCL++ subset requires exactly one source and destination");
  }
}

contracts::FineMemoryReference fine_reference(
    const contracts::BufferSlice& slice,
    contracts::DeviceId issuer,
    std::uint64_t alignment) {
  return contracts::FineMemoryReference{
      slice.owner,
      slice.owner == issuer ? contracts::FineMemorySpace::LocalHbm
                            : contracts::FineMemorySpace::RemoteHbm,
      slice.buffer,
      slice.offset_bytes,
      slice.length_bytes,
      alignment};
}

contracts::FineMemoryReference semaphore_reference(
    contracts::DeviceId owner,
    contracts::SemaphoreId semaphore) {
  return contracts::FineMemoryReference{
      owner,
      contracts::FineMemorySpace::SemaphoreMemory,
      contracts::BufferId(4000000 + owner.value()),
      (semaphore.value() - 1) * 8,
      8,
      8};
}

}  // namespace

MscclppPlan parse_mscclpp_plan(std::istream& input) {
  std::ostringstream text;
  text << input.rdbuf();
  const JsonValue root = JsonParser(text.str()).parse();
  MscclppPlan plan;
  plan.name = string_field(root, "name");
  plan.collective = string_field(root, "collective");
  plan.protocol = string_field(root, "protocol");
  plan.inplace = bool_field(root, "inplace");
  plan.reuse_resources = bool_field(root, "reuse_resources");
  for (const JsonValue& gpu_value : array_field(root, "gpus")) {
    MscclppGpu gpu;
    gpu.id = number_field(gpu_value, "id");
    gpu.input_chunks = number_field(gpu_value, "input_chunks");
    gpu.output_chunks = number_field(gpu_value, "output_chunks");
    gpu.scratch_chunks = number_field(gpu_value, "scratch_chunks");
    for (const JsonValue& threadblock_value : array_field(gpu_value, "threadblocks")) {
      MscclppThreadblock threadblock;
      threadblock.id = number_field(threadblock_value, "id");
      for (const JsonValue& operation : array_field(threadblock_value, "ops")) {
        threadblock.operations.push_back(parse_operation(operation));
      }
      gpu.threadblocks.push_back(threadblock);
    }
    for (const JsonValue& channel_value : array_field(gpu_value, "channels")) {
      gpu.channels.push_back(MscclppChannel{
          string_field(channel_value, "channel_type"),
          number_array(channel_value, "connected_to")});
    }
    for (const JsonValue& remote_value : array_field(gpu_value, "remote_buffers")) {
      gpu.remote_buffers.push_back(MscclppRemoteBuffer{
          number_field(remote_value, "rank"), string_field(remote_value, "type"),
          string_array(remote_value, "access_channel_types")});
    }
    plan.gpus.push_back(gpu);
  }
  return plan;
}

MscclppPlan parse_mscclpp_plan_file(const std::string& path) {
  std::ifstream input(path.c_str());
  if (!input) throw std::invalid_argument("could not open MSCCL++ plan: " + path);
  return parse_mscclpp_plan(input);
}

contracts::ValidationErrors MscclppFineLoweringConfig::validate() const {
  contracts::ValidationErrors errors;
  if (workgroup_residency_slots == 0 || memory_alignment_bytes == 0 ||
      (memory_alignment_bytes & (memory_alignment_bytes - 1)) != 0 ||
      reduce_cycles == 0) {
    contracts::append_error(
        errors,
        contracts::ValidationCode::InvalidResourceRequirement,
        "MSCCL++ fine lowering values must be nonzero and alignment power-of-two");
  }
  return errors;
}

void MscclppFineLoweringConfig::require_valid() const {
  contracts::require_no_errors(validate(), "MscclppFineLoweringConfig");
}

std::vector<contracts::FineKernelDefinition> lower_mscclpp_fine_kernels(
    const MscclppPlan& plan,
    std::uint64_t payload_bytes,
    const MscclppFineLoweringConfig& config) {
  config.require_valid();
  if (payload_bytes == 0 || plan.protocol != "Simple" ||
      plan.reuse_resources || plan.gpus.empty()) {
    throw std::invalid_argument(
        "MSCCL++ fine lowering requires a nonempty safe Simple plan");
  }
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t>
      signal_generations;
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t>
      wait_generations;
  std::vector<contracts::FineKernelDefinition> kernels;
  for (std::size_t gpu_index = 0; gpu_index < plan.gpus.size(); ++gpu_index) {
    const MscclppGpu& gpu = plan.gpus[gpu_index];
    if (gpu.id != gpu_index || gpu.input_chunks == 0 ||
        payload_bytes % gpu.input_chunks != 0) {
      throw std::invalid_argument(
          "MSCCL++ fine lowering GPU/chunk mapping is invalid");
    }
    const contracts::DeviceId issuer(gpu.id + 1);
    const std::uint64_t chunk_bytes = payload_bytes / gpu.input_chunks;
    std::vector<contracts::FineWorkgroupDefinition> workgroups;
    std::uint64_t next_operation = 1;
    for (const MscclppThreadblock& threadblock : gpu.threadblocks) {
      std::vector<contracts::FineGpuOperation> operations;
      for (const MscclppOperation& operation : threadblock.operations) {
        if (operation.name == "nop") continue;
        const contracts::GpuOperationId operation_id(next_operation++);
        if (operation.name == "put" || operation.name == "get" ||
            operation.name == "copy") {
          require_single_buffers(operation);
          const contracts::BufferSlice source =
              buffer_slice(plan, gpu, operation.sources.front(), chunk_bytes);
          const contracts::BufferSlice destination =
              buffer_slice(plan, gpu, operation.destinations.front(), chunk_bytes);
          if (operation.name == "put" &&
              (source.owner != issuer || destination.owner == issuer)) {
            throw std::invalid_argument(
                "MSCCL++ fine put requires local source and remote destination");
          }
          if (operation.name == "get" &&
              (source.owner == issuer || destination.owner != issuer)) {
            throw std::invalid_argument(
                "MSCCL++ fine get requires remote source and local destination");
          }
          operations.push_back(contracts::FineGpuOperation::memcpy(
              operation_id,
              fine_reference(source, issuer, config.memory_alignment_bytes),
              fine_reference(
                  destination, issuer, config.memory_alignment_bytes)));
        } else if (operation.name == "signal" || operation.name == "wait") {
          if (operation.channel_type != "memory" ||
              operation.channel_ids.size() != 1) {
            throw std::invalid_argument(
                "MSCCL++ fine semaphore requires one memory channel");
          }
          const std::uint64_t channel = operation.channel_ids.front();
          const std::uint64_t owner_rank = operation.name == "signal"
              ? peer_for_channel(gpu, channel)
              : gpu.id;
          const std::pair<std::uint64_t, std::uint64_t> key(owner_rank, channel);
          std::uint64_t& generation = operation.name == "signal"
              ? signal_generations[key]
              : wait_generations[key];
          ++generation;
          const contracts::DeviceId owner(owner_rank + 1);
          const contracts::SemaphoreId semaphore(channel + 1);
          const contracts::FineMemoryReference location =
              semaphore_reference(owner, semaphore);
          operations.push_back(
              operation.name == "signal"
              ? contracts::FineGpuOperation::semaphore_release(
                    operation_id, owner, semaphore, generation, location)
              : contracts::FineGpuOperation::semaphore_acquire(
                    operation_id, owner, semaphore, generation, location));
        } else if (operation.name == "reduce") {
          require_single_buffers(operation);
          if (operation.reduce_op != "sum") {
            throw std::invalid_argument(
                "MSCCL++ fine lowering supports sum reduction only");
          }
          const contracts::BufferSlice source =
              buffer_slice(plan, gpu, operation.sources.front(), chunk_bytes);
          const contracts::BufferSlice destination =
              buffer_slice(plan, gpu, operation.destinations.front(), chunk_bytes);
          if (source.owner != issuer || destination.owner != issuer) {
            throw std::invalid_argument(
                "MSCCL++ fine reduction buffers must be local");
          }
          operations.push_back(contracts::FineGpuOperation::reduce(
              operation_id, config.reduce_cycles));
        } else {
          throw std::invalid_argument(
              "unsupported MSCCL++ fine operation: " + operation.name);
        }
      }
      if (operations.empty()) continue;
      const contracts::WorkgroupId workgroup(threadblock.id + 1);
      workgroups.push_back(contracts::FineWorkgroupDefinition{
          workgroup,
          contracts::ThreadBlockId(
              gpu.id * 100000 + threadblock.id + 1),
          contracts::FineWorkgroupResources{
              config.workgroup_residency_slots},
          {contracts::FineWavefrontProgram{
              contracts::WavefrontId(threadblock.id + 1), operations}}});
    }
    contracts::FineKernelDefinition kernel{
        contracts::KernelId(gpu.id + 1),
        issuer,
        contracts::FineKernelKind::Communication,
        workgroups};
    kernel.require_valid();
    kernels.push_back(kernel);
  }
  return kernels;
}

void MscclppPlanRegistry::add(const MscclppPlanRegistryEntry& entry) {
  if (entry.plan.name.empty() || entry.plan.gpus.empty() ||
      entry.plan.protocol.empty() || entry.minimum_payload_bytes == 0 ||
      entry.minimum_payload_bytes > entry.maximum_payload_bytes ||
      entry.payload_multiple_bytes == 0) {
    throw std::invalid_argument("invalid MSCCL++ plan registry entry");
  }
  collective_kind(entry.plan.collective);
  entries_.push_back(entry);
}

const MscclppPlan& MscclppPlanRegistry::select(
    contracts::CollectiveKind collective,
    std::uint32_t participant_count,
    const std::string& protocol,
    std::uint64_t payload_bytes) const {
  const MscclppPlan* selected = nullptr;
  for (const MscclppPlanRegistryEntry& entry : entries_) {
    const bool matches = collective_kind(entry.plan.collective) == collective &&
        entry.plan.gpus.size() == participant_count &&
        entry.plan.protocol == protocol &&
        payload_bytes >= entry.minimum_payload_bytes &&
        payload_bytes <= entry.maximum_payload_bytes &&
        payload_bytes % entry.payload_multiple_bytes == 0;
    if (!matches) continue;
    if (selected != nullptr) {
      throw std::invalid_argument("ambiguous MSCCL++ plan registry selection");
    }
    selected = &entry.plan;
  }
  if (selected == nullptr) {
    throw std::invalid_argument("no MSCCL++ plan matches the selection key");
  }
  return *selected;
}

MscclppScheduleProvider::MscclppScheduleProvider(const MscclppPlan& plan)
    : plan_(plan) {}

MscclppScheduleProvider::MscclppScheduleProvider(const std::string& plan_path)
    : plan_(parse_mscclpp_plan_file(plan_path)) {}

PlannedCollective MscclppScheduleProvider::plan(
    const CollectiveRequest& request,
    const PlannerCapabilities& capabilities) const {
  const contracts::CollectiveKind kind = collective_kind(plan_.collective);
  if (kind != request.kind || plan_.protocol != "Simple" || plan_.reuse_resources ||
      plan_.gpus.size() != request.group.members.size() ||
      plan_.gpus.size() > capabilities.maximum_endpoints) {
    throw std::invalid_argument("MSCCL++ plan does not match request or safe subset");
  }
  std::vector<contracts::MicroOp> operations;
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> signal_generations;
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> wait_generations;
  std::uint64_t next_operation = 1;
  for (std::size_t gpu_index = 0; gpu_index < plan_.gpus.size(); ++gpu_index) {
    const MscclppGpu& gpu = plan_.gpus[gpu_index];
    if (gpu.id != gpu_index || gpu.input_chunks == 0 ||
        request.payload_bytes % gpu.input_chunks != 0 ||
        request.group.members[gpu_index] != contracts::DeviceId(gpu.id + 1)) {
      throw std::invalid_argument("MSCCL++ GPU/chunk mapping does not match request");
    }
    const std::uint64_t chunk_bytes = request.payload_bytes / gpu.input_chunks;
    for (const MscclppThreadblock& threadblock : gpu.threadblocks) {
      contracts::OperationId previous;
      std::uint32_t program_order = 0;
      const contracts::ThreadBlockId threadblock_id(gpu.id * 100000 + threadblock.id + 1);
      for (const MscclppOperation& operation : threadblock.operations) {
        if (operation.name == "nop") continue;
        const contracts::OperationId id(next_operation++);
        const std::vector<contracts::OperationId> dependencies =
            previous.valid() ? std::vector<contracts::OperationId>{previous}
                             : std::vector<contracts::OperationId>();
        const contracts::DeviceId issuer(gpu.id + 1);
        if (operation.name == "put" || operation.name == "get" ||
            operation.name == "reduce") {
          require_single_buffers(operation);
          const contracts::BufferSlice source =
              buffer_slice(plan_, gpu, operation.sources.front(), chunk_bytes);
          const contracts::BufferSlice destination =
              buffer_slice(plan_, gpu, operation.destinations.front(), chunk_bytes);
          if (operation.name == "put") {
            if (source.owner != issuer || destination.owner == issuer) {
              throw std::invalid_argument("MSCCL++ put requires local source and remote destination");
            }
            operations.push_back(contracts::MicroOp::put(
                id, issuer, threadblock_id, program_order, dependencies, source,
                destination, contracts::MemoryOrder::Ordered,
                contracts::OrderingDomainId(threadblock_id.value()),
                contracts::CompletionStage::RemoteVisible));
          } else if (operation.name == "get") {
            if (source.owner == issuer || destination.owner != issuer) {
              throw std::invalid_argument("MSCCL++ get requires remote source and local destination");
            }
            operations.push_back(contracts::MicroOp::get(
                id, issuer, threadblock_id, program_order, dependencies, source,
                destination, contracts::MemoryOrder::Ordered,
                contracts::OrderingDomainId(threadblock_id.value()),
                contracts::CompletionStage::LocalComplete));
          } else {
            if (source.owner != issuer || destination.owner != issuer ||
                operation.reduce_op != "sum") {
              throw std::invalid_argument("safe MSCCL++ reduce requires local buffers and sum");
            }
            operations.push_back(contracts::MicroOp::reduce(
                id, issuer, threadblock_id, program_order, dependencies, source,
                destination, contracts::ReductionKind::Sum,
                contracts::MemoryOrder::Ordered,
                contracts::OrderingDomainId(threadblock_id.value()),
                contracts::CompletionStage::LocalComplete));
          }
        } else if (operation.name == "signal" || operation.name == "wait") {
          if (operation.channel_type != "memory" || operation.channel_ids.size() != 1) {
            throw std::invalid_argument("safe MSCCL++ semaphore op requires one memory channel");
          }
          const std::uint64_t channel = operation.channel_ids.front();
          const std::uint64_t owner_rank = operation.name == "signal"
              ? peer_for_channel(gpu, channel) : gpu.id;
          const std::pair<std::uint64_t, std::uint64_t> key(owner_rank, channel);
          std::uint64_t& generation = operation.name == "signal"
              ? signal_generations[key] : wait_generations[key];
          ++generation;
          if (operation.name == "signal") {
            operations.push_back(contracts::MicroOp::signal(
                id, issuer, threadblock_id, program_order, dependencies,
                contracts::DeviceId(owner_rank + 1), contracts::SemaphoreId(channel + 1),
                generation, contracts::MemoryOrder::Ordered,
                contracts::OrderingDomainId(threadblock_id.value()),
                contracts::CompletionStage::RemoteVisible));
          } else {
            operations.push_back(contracts::MicroOp::wait(
                id, issuer, threadblock_id, program_order, dependencies, issuer,
                contracts::SemaphoreId(channel + 1), generation,
                contracts::MemoryOrder::Ordered,
                contracts::OrderingDomainId(threadblock_id.value()),
                contracts::CompletionStage::LocalComplete));
          }
        } else {
          throw std::invalid_argument(
              "unsupported MSCCL++ operation in safe subset: " + operation.name);
        }
        previous = id;
        ++program_order;
      }
    }
  }
  contracts::CollectivePlan translated{
      request.id, kind, request.group, request.payload_bytes, operations};
  translated.require_valid();
  return PlannedCollective{
      translated, ScheduleProvenance{"mscclpp", plan_.name, "official-json-v1"}};
}

}  // namespace runtime
}  // namespace simai
