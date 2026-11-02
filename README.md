# MP-QUIC Mini: Project Phases & Progress Tracker

- [x] **Phase 1: Sockets (`UDPSocket` & `SocketManager`)**
  * Creates raw Linux UDP sockets and uses an `epoll` event loop to listen on multiple ports with zero CPU waste.

- [x] **Phase 2: Multiple Paths (`Path`)**
  * Bundles individual network interfaces (e.g. Path 0 = Wi-Fi, Path 1 = Ethernet) with their local and remote IP addresses.

- [x] **Phase 3: Custom Packet Header (`Packet`)**
  * Adds our 20-byte binary header (packet numbers, flags, timestamps) and converts it to/from network byte order.

- [ ] **Phase 4 & 5: Reliability & ACKs (`Reliability`)**
  * Chunks application data into packets, assigns sequence numbers (`#1, #2...`), and sends back ACK packets to measure live RTT latency.

- [ ] **Phase 6 & 7: Reorder Buffer & Loss Recovery**
  * **Sender:** Retransmits lost packets if no ACK arrives before the timeout (RTO).
  * **Receiver:** Holds out-of-order packets in a buffer and delivers them in strict original sequence.

- [ ] **Phase 8: Multipath Scheduler (`Scheduler`)**
  * The brain that decides which path sends each packet (e.g., Round-Robin across both, or Lowest-RTT-First).

- [ ] **Phase 9 & 10: Congestion Control (AIMD)**
  * Manages bandwidth independently on each path using Slow-Start and Additive-Increase / Multiplicative-Decrease so a congested path slows down without choking the other.

- [ ] **Phase 11 & 12: End-to-End Integration & Evaluation (For "Very High" Grade)**
  * Transfers real files across multiple paths, verifies data integrity with SHA-256, and benchmarks performance under simulated network delay/loss using `tc netem`.
