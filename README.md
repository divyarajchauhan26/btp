# SimAI Scale-Up Research Simulator

This repository contains a C++14 research prototype for deterministic
simulation of accelerator scale-up communication and fine-grained GPU memory
operations.

The implementation is located in [`simai_scaleup/`](simai_scaleup/).

Implemented paths include collective planning, operation DAG execution,
compute and communication overlap, GPU resource contention, local and remote
Load/Store modeling, timestamped synthetic traces, one-sided communication,
multi-rail transport, finite queues, credit-based flow control, replay, and
destination reassembly.

The native analytical transport runs end to end. Persistent asynchronous
integration with the external ns-3/SUE-Sim backend remains active research
work and is not presented as complete.

