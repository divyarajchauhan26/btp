# Open Decisions

These questions must be answered before the corresponding semantics are frozen.

## Research/Fidelity

1. Is Load/Store-semantic simulation definitely sufficient, or is actual ISA
   trace replay required?
2. Which published or measured reference establishes accuracy?
3. Is the target a generic configurable GPU or a named architecture?

## Kernel Input

4. Where does compute-kernel detail come from: profiling, static templates,
   compiler output, Chakra attributes, or distributions?
5. Should additional detail be embedded in Chakra or stored in a sidecar keyed
   by node ID?
6. Which Chakra schema/version must be supported first?
7. How are communication kernels associated with MSCCL++ plans?
8. Who generates those plans: official tooling, a plan library, or a future
   synthesizer?

## GPU Execution

9. Which workgroup scheduler is the reference: round-robin, least-occupied, or
   architecture-specific?
10. What resource requirements determine CU residency beyond slot count?
11. Are all wavefronts given the same operation sequence with disjoint ranges?
12. What issue width and instruction latency parameters are required?
13. How should compute/arithmetic duration be represented?

## Memory

14. Which first milestone is required: local/remote HBM only, or L1/L2/HBM?
15. Are cache hits/misses simulated, supplied by traces, or abstracted through
   resolved placement?
16. What cache-line size(s) and alignment behavior are required?
17. Is a register-file capacity model needed or only conceptual load/store
   endpoints?
18. Which local interconnect/NoC effects must be modeled before SUE?

## Synchronization

19. Which semaphore semantics match the target hardware/runtime?
20. Are semaphore values generations, counters, or arbitrary values?
21. Which Waitcnt request classes and thresholds are required?
22. Is inter-workgroup BarrierOp legal without cooperative kernel residency?
23. Which memory-ordering/fence semantics must be visible?

## SUE Boundary

24. Does SUE receive only inter-GPU requests or also on-chip/local NoC traffic?
25. Where exactly does source HBM read latency end and SUE transmission begin?
26. Does remote completion occur at GPU ingress, destination cache, or HBM
   visibility?
27. How are logical MSCCL++ channels mapped to SUE ports/rails?

## Output

28. Is a detailed execution table required as a formal deliverable?
29. Which metrics matter most: kernel latency, exposed communication, CU
   utilization, HBM traffic, semaphore stalls, or SUE congestion?
30. What maximum workload scale and acceptable wall-clock runtime are expected?

