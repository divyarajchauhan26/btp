# MockNCCL, MSCCL++, PUT/GET and SimAI: A Beginner Guide

Date checked: 2026-10-05

This is the beginner version of
[`MOCKNCCL_ONE_SIDED_RESEARCH.md`](MOCKNCCL_ONE_SIDED_RESEARCH.md).
Read this file first. The research document is the detailed technical reference
for later implementation.

## 1. The whole problem in one sentence

An AI workload tells SimAI **what collective operation it wants**, but the
network simulator needs to know **which data moves where and when**.

Something must translate between those two descriptions.

That translator is why MockNCCL exists, and an equivalent translator is also
needed if we use MSCCL++ plans.

## 2. Five names to understand first

### NCCL

NCCL is NVIDIA's real communication library for GPUs. An AI framework can ask
it to perform operations such as AllReduce, AllGather or ReduceScatter.

Real NCCL runs on real machines using CUDA, GPUs and communication hardware.

### MockNCCL

MockNCCL is SimAI's simplified model of some NCCL decisions. It does not move
real data and does not launch real GPU kernels.

It answers questions such as:

- Should this collective use a ring or another algorithm?
- How many channels and chunks should it use?
- Which rank sends each chunk to which other rank?

Its output is a collection of simulated flows.

### MSCCL

Classic MSCCL is Microsoft's older system for creating customized collective
algorithms that run using an NCCL-derived runtime. Its schedules are commonly
represented as XML.

### MSCCL++

MSCCL++ is a newer and separate Microsoft project. It provides lower-level,
GPU-driven communication operations such as:

```text
PUT, GET, SIGNAL, WAIT and FLUSH
```

It also provides a Python DSL that can generate a JSON execution plan.

### SUE-Sim

In our project, SUE-Sim is intended to model the scale-up network that carries
the commands, data and responses produced by the collective execution model.

It should not decide the collective algorithm. It should simulate what happens
after a collective plan has been chosen.

## 3. A delivery-company analogy

Imagine four houses that must exchange boxes.

The AI workload says:

```text
Every house must finish with everybody's box.
```

That is similar to saying `AllGather`. It describes the final goal, but not the
delivery route.

MockNCCL acts like a route planner. It might produce:

```text
House 0 sends to House 1
House 1 sends to House 2
House 2 sends to House 3
House 3 sends to House 0
repeat for the remaining boxes
```

The network simulator acts like the roads and delivery vehicles. It determines
how long those deliveries take when roads are busy.

MSCCL++ is a more detailed instruction language for the delivery workers. It
can say:

```text
put this box directly in House 1's locker
fetch that box from House 2's locker
ring a bell after the box is placed
wait until a bell is heard
```

## 4. Why SimAI created MockNCCL

SimAI cannot simply run real NCCL inside a normal event-driven simulation:

- real NCCL expects CUDA and real GPU resources;
- it discovers and controls real hardware;
- it launches real kernels and communication operations;
- a simulator needs deterministic simulated events, not actual data movement;
- running real NCCL would defeat the purpose of exploring hardware that does
  not yet exist.

Therefore SimAI copied only enough NCCL-like planning behavior to turn a
collective into flows:

```text
AI workload
    ↓
AllReduce(group, bytes)
    ↓
MockNCCL chooses an algorithm and chunks
    ↓
Flow A: rank 0 → rank 1
Flow B: rank 1 → rank 2
Flow C: rank 2 → rank 3
    ↓
network simulation
```

This is useful, but the current flows contain only basic information such as
source, destination, byte count, channel and dependencies.

## 5. What PUT and GET mean

Suppose GPU 0 and GPU 1 each expose a memory buffer.

### PUT

GPU 0 places data into GPU 1's memory:

```text
GPU 0 ── data ──> GPU 1 memory
```

GPU 1 does not have to issue a matching receive operation. It still needs a
safe way to learn that the data is ready.

Using the locker analogy:

```text
I place a parcel in your locker and then notify you.
```

### GET

GPU 0 fetches data from GPU 1's memory:

```text
GPU 0 ── request ──> GPU 1
GPU 0 <── data ───── GPU 1
```

Using the analogy:

```text
I travel to your locker and collect the parcel myself.
```

GET is not simply PUT with two rank numbers swapped. GET has a request going in
one direction and data returning in the opposite direction.

## 6. Why SIGNAL and WAIT are needed

Imagine GPU 0 performs a PUT into GPU 1's memory. GPU 1 must not read that
memory too early.

The safe sequence is:

```text
GPU 0 performs PUT
GPU 0 waits until the data is actually placed
GPU 0 sends SIGNAL
GPU 1 performs WAIT
GPU 1 sees the signal
GPU 1 consumes the data
```

The signal is similar to saying, “The parcel is now in the locker.”

If the signal arrives before the data is visible, GPU 1 may read incomplete or
old data. That is why the simulator must distinguish:

- operation issued;
- data arrived at the remote GPU;
- data became visible in remote memory;
- completion signal became visible;
- the entire operation finished.

## 7. What a barrier means

A barrier means that a group must reach the same point before anybody moves to
the next phase.

Example:

```text
GPU 0 ready ─┐
GPU 1 ready ─┼─> everybody may continue
GPU 2 ready ─┤
GPU 3 ready ─┘
```

But there are several kinds of barriers:

- a barrier between threads inside one GPU block;
- a barrier between several GPU blocks;
- a barrier between ranks/GPUs;
- a memory fence ensuring data is visible;
- signal/wait between a producer and consumer.

They are related, but they are not interchangeable. A single generic delay
called `BARRIER` would be too vague for a correct simulator.

## 8. Is MSCCL++ a real project?

Yes. It is an active, open-source Microsoft project:

- Repository: <https://github.com/microsoft/mscclpp>
- Documentation: <https://microsoft.github.io/mscclpp/>
- Paper: <https://arxiv.org/abs/2504.09014>

The repository contains real runtime code, C++/CUDA/HIP primitives, a Python
DSL, examples, tests and an execution-plan executor. It is not a simulator.
It is designed to run communication on real GPU systems.

The DSL can generate a JSON execution plan describing operations for every
rank, including buffers, channels and synchronization points. The real
MSCCL++ executor can execute that plan on hardware.

For our simulator, we can read the same plan but simulate its operations
instead of launching real GPU communication.

## 9. Is there already an MSCCL++ simulator?

The strongest published example is ASTRA-sim 3.0. Its paper describes:

- parsing MSCCL++ collective plans;
- translating PUT/GET into fine-grained memory operations;
- translating signal/wait into semaphore operations;
- modeling GPU CUs, workgroups and wavefronts;
- modeling local memory, on-chip traffic and inter-GPU traffic.

Paper: <https://arxiv.org/abs/2606.10440>

However, as of this review date, the public ASTRA-sim repository still
identifies itself as ASTRA-sim 2.0, and public issues report that no ASTRA-sim
3.0 branch, tag or release is available. Therefore I used the ASTRA-sim 3.0
paper as a design reference; I did not copy a publicly released ASTRA-sim 3.0
implementation.

- Public repository: <https://github.com/astra-sim/astra-sim>
- Public release-status question:
  <https://github.com/astra-sim/astra-sim/issues/380>

This distinction is important:

```text
MSCCL++ repository: real and publicly available
ASTRA-sim 3.0 paper: publicly available
ASTRA-sim 3.0 implementation: not publicly released in the main repo
```

The `MscclParser` files already present in the older local SimAI/SUE work are
project prototypes. They are not the publicly released ASTRA-sim 3.0
implementation, and they should not be treated as the source of truth for
memory ordering or completion behavior.

## 10. Do we need to build MockMSCCL++?

We need to build the **simulation-facing part**, but not clone the complete
MSCCL++ runtime.

The real MSCCL++ path is:

```text
Python MSCCL++ algorithm
    ↓
JSON execution plan
    ↓
real MSCCL++ executor
    ↓
real GPU and network hardware
```

Our simulated path would be:

```text
MSCCL++ JSON execution plan
    ↓
MscclppPlanImporter
    ↓
our native PUT/GET/SIGNAL/WAIT representation
    ↓
our simulated operation executor
    ↓
SUE-Sim
```

You may casually call the lower path “MockMSCCL++,” but technically it is only
an MSCCL++-compatible frontend plus a simulator-native executor.

We would not recreate:

- CUDA kernels;
- real GPU memory registration;
- real network connections;
- real proxy threads;
- real NCCL API replacement;
- every MSCCL++ optimization.

## 11. What exactly must we build?

### Part A: a common instruction format

Our simulator needs a small internal language such as:

```text
PUT
GET
SIGNAL
WAIT
FENCE
FLUSH
REDUCE
BARRIER
```

This is the native collective-program representation.

### Part B: an MSCCL++ plan importer

This component reads MSCCL++ JSON and translates each supported operation into
our common format.

It is a translator, not a GPU runtime.

### Part C: an operation executor

The executor decides when an operation is allowed to start and when each stage
completes. It tracks signals, waits, dependencies and memory visibility.

### Part D: the SUE adapter

This component turns the operations into SUE commands, data, responses and
completion events.

### Part E: native algorithms

We should also implement a few simple PUT/GET algorithms ourselves. Then the
simulator can work even when no MSCCL++ JSON file is supplied.

## 12. Should MockNCCL disappear?

No. Keep it as a baseline while adding the new path.

The desired design is:

```text
                         ┌─ MockNCCL planner
AI workload ─────────────┤     ring/tree/NCCL-like baseline
                         │
                         └─ MSCCL++ plan importer
                               custom PUT/GET plan
                                      ↓
                         common collective-program format
                                      ↓
                              simulated executor
                                      ↓
                                  SUE-Sim
```

This lets us compare:

```text
normal NCCL-like collective
versus
custom one-sided MSCCL++-style collective
```

If we deleted MockNCCL immediately, we would lose the baseline and fallback
algorithm source.

## 13. A tiny two-GPU example

Initially:

```text
GPU 0 output: [A, empty]
GPU 1 output: [empty, B]
```

The desired AllGather result is:

```text
GPU 0 output: [A, B]
GPU 1 output: [A, B]
```

A simplified PUT-based plan could be:

```text
GPU 0: PUT A into GPU 1's first slot
GPU 0: SIGNAL GPU 1 that A is ready

GPU 1: PUT B into GPU 0's second slot
GPU 1: SIGNAL GPU 0 that B is ready

GPU 0: WAIT for B-ready signal
GPU 1: WAIT for A-ready signal
```

The simulator must model not only two data transfers, but also:

- the destination buffer slots;
- the data placement time;
- the two signals;
- the two waits;
- the rule that each signal follows data visibility.

That information cannot be represented accurately by the current
`SingleFlow(src, dest, bytes)` alone.

## 14. What is our safest implementation strategy?

Proceed in this order:

1. Define the common PUT/GET/SIGNAL/WAIT format.
2. Make a small untimed executor and verify the operation order.
3. Implement correct PUT.
4. Implement correct GET, including request and response.
5. Add signals, waits, memory visibility and barriers.
6. Add one simple native AllGather and ReduceScatter.
7. Add the MSCCL++ JSON importer.
8. Connect the same operations to SUE-Sim.
9. Add detailed GPU modeling only when the simpler model is correct.

Starting with the importer before defining our own semantics would be risky:
we could successfully parse a JSON file while still simulating its operations
incorrectly.

## 15. Is MSCCL++ automatically better?

No. It gives us more control, not guaranteed performance.

It can help when:

- PUT/GET reduces target-side coordination;
- communication overlaps with computation;
- a custom algorithm matches the topology;
- barriers and chunks are carefully optimized.

It can hurt when:

- synchronization adds too much overhead;
- a plan was optimized for different hardware;
- GET requests become blocked behind data traffic;
- many small operations create excessive control traffic;
- a normal NCCL algorithm already uses the network efficiently.

That is precisely why simulating both approaches is valuable.

## 16. The final mental model

Remember these four sentences:

1. **The workload states the collective goal.**
2. **MockNCCL or an MSCCL++ plan states the communication algorithm.**
3. **Our executor gives PUT/GET/synchronization their simulated meaning.**
4. **SUE-Sim determines how the resulting network activity behaves.**

That is the complete architecture at beginner level.
