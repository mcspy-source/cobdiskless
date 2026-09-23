# CobDiskless Concurrency & Threading Model

This document establishes the concurrency design for the CobDiskless platform to prevent uncontrolled thread spawning, race conditions, and synchronization bottlenecks as network and storage subsystems are integrated.

---

## 1. Concurrency Taxonomy by Subsystem

| Domain | Concurrency Pattern | Implementation Mechanism | Justification |
| :--- | :--- | :--- | :--- |
| **Core Lifecycle** | **Synchronous / Deterministic** | Single-threaded orchestrator | Subsystem registration, forward initialization, reverse shutdown, and failure rollbacks must be strictly deterministic. |
| **Configuration** | **Read-heavy / Synchronous** | Immutable after `initialize()` | Avoids locking overhead during steady-state diskless operations. Mutable only prior to core startup. |
| **Logging** | **Thread-Safe Concurrent** | `std::mutex` serialized sink | Subsystems and worker threads can log concurrently without output tearing or corrupted interleaving. |
| **DHCP Subsystem** | **Event-Driven Non-blocking** | UDP socket event polling / IOCP | DHCP packets (Discover/Request) are short and bursty. Spawning threads per packet is an anti-pattern. |
| **TFTP Subsystem** | **Event-Driven Non-blocking** | UDP socket event polling / timer wheels | Session state machines track windowing and block ACKs asynchronously without blocking threads. |
| **Storage (BlockDevice)** | **Worker Thread Pool / Async I/O** | Job queue with thread pool (`std::jthread` / IOCP) | File I/O (VHD reads, differential write-cache flushes) must not block network packet loops. |
| **SAN Service (iSCSI)** | **Hybrid Event-Driven + Pool** | Async TCP dispatch + Worker pool | Network PDUs received via completion events; SCSI Command execution dispatched to storage thread pool. |

---

## 2. Synchronization Rules & Invariants

1. **No Global Singletons:**
   Components must never access global mutable state. All dependencies (Configuration, Logger, storage handles) are passed explicitly via dependency injection during construction or initialization.
2. **Deterministic Teardown:**
   When `Core::shutdown()` or rollback is invoked, all worker threads belonging to child subsystems must be joined and terminated *before* the subsystem reports `Stopped`.
3. **BlockDevice Concurrent Read Safety:**
   Golden OS disk images are shared across multiple booting clients simultaneously. Implementations of `BlockDevice` must guarantee that overlapping `read()` calls from multiple threads are thread-safe and non-blocking.
4. **Isolated Write Caches:**
   Per-client writes are isolated to individual write-cache layers (differencing VHD / memory buffer). Cross-client lock contention is strictly avoided by ensuring each client operates on its own dedicated write session.
