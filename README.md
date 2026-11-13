# MP-QUIC Mini: Project Phases & Progress Tracker

- [x] **Phase 1: Sockets (`UDPSocket` & `SocketManager`)**
  * Creates raw Linux UDP sockets and uses an `epoll` event loop to listen on multiple ports with zero CPU waste.

- [x] **Phase 2: Multiple Paths (`Path`)**
  * Bundles individual network interfaces (e.g. Path 0 = Wi-Fi, Path 1 = Ethernet) with their local and remote IP addresses.

- [x] **Phase 3: Custom Packet Header (`Packet`, `Frame`)**
  * Adds our 18-byte public header (flags, Connection ID, Path ID, Packet Number) and converts it to/from network byte order. Everything after the header is a list of frames (`STREAM`, `ACK`, `WINDOW_UPDATE`, `PING`, `PATHS`), as in the reference paper. An `ACK` frame can carry up to 256 packet number ranges.

- [x] **Phase 4 & 5: Reliability & ACKs (`SendStream`, `SentPacketHandler`, `ReceivedPacketHandler`, `RttStats`, `Connection`)**
  * Chunks application data into `STREAM` frames (stream ID + byte offset), gives every packet the next packet number of its path (`#1, #2...` per path), and sends back `ACK` frames (Path ID + packet number ranges + ACK delay) to measure the live RTT of each path.

- [x] **Phase 6 & 7: Reorder Buffer, Loss Recovery & Flow Control (`StreamFrameSorter`, `ReceiveStream`, `FlowController`, `SentPacketHandler`)**
  * **Sender:** Detects lost packets (time-based loss detection, tail loss probes, RTO with exponential backoff) and sends their frames again in new packets, on any path. A path whose RTO fires without any network activity is marked potentially failed and is not used until a packet is received on it again.
  * **Receiver:** Holds out-of-order `STREAM` frames in a reorder buffer and delivers the data in strict original sequence, every byte once.
  * **Flow control:** The receiver advertises its window with `WINDOW_UPDATE` frames (sent on all paths), at stream and connection level, and the sender never sends beyond it.

- [x] **Phase 8: Multipath Scheduler (`Scheduler`, `LowestRttScheduler`)**
  * The brain that decides which path sends each packet. Implemented: the Lowest-RTT-First scheduler of the reference paper. It takes the path with the lowest smoothed RTT whose congestion window has room, ignores potentially failed paths (and probes them with a `PING`), and duplicates the data on the other path while the RTT of the chosen path is still unknown. When a path becomes potentially failed, a `PATHS` frame tells the peer, so that it stops using that path as well.
  * `Scheduler` is an interface, so other schedulers (e.g., Round-Robin) can be added later and selected with `Connection::set_scheduler`.

- [x] **Phase 9 & 10: Congestion Control (OLIA) (`OliaSender`, `HybridSlowStart`)**
  * **Phase 9, the window of one path:** Slow-Start (the window grows by one packet per ACK, and stops early when the RTT starts to grow), halving of the window when a packet is lost (once per loss event), restart from 2 packets after a retransmission timeout.
  * **Phase 10, coupling of the paths:** in congestion avoidance the windows grow with OLIA, the coupled multipath congestion control used in the reference paper. The increase of a path depends on the windows and RTTs of all the paths, so the connection is fair to single-path flows, and window moves towards the best path. A congested path slows down without choking the other.

- [x] **Phase 11: End-to-End Integration (`Client`, `Server`, `PathManager`, programs in `apps/`)**
  * Real programs transfer real files across multiple paths: `mpq_server` serves files, `mpq_client` downloads one and verifies data integrity with SHA-256, `mpq_reqres` measures request/response delays.
  * The server needs no configuration of the clients: the first packet opens the connection, a path is created when its first packet arrives, and the server advertises its addresses (`ADD_ADDRESS`) so that the client opens the second path. A `CONNECTION_CLOSE` frame or a 30 s idle timeout ends a connection.

- [ ] **Phase 12: Evaluation (For "Very High" Grade)**
  * Benchmarks performance under simulated network delay/loss using `tc netem`, on virtual machines: large file, short file and network handover, as in the reference paper.

## Project layout

| Folder | Contents | Phase |
| --- | --- | --- |
| `include/sockets`, `src/sockets` | UDP socket, `epoll` event loop | 1 |
| `include/path`, `src/path` | `Path`, `PathManager` | 2, 11 |
| `include/packet`, `src/packet` | Public header, frames | 3 |
| `include/reliability`, `src/reliability` | Stream chunking, ACKs, RTT, loss recovery, reorder buffer | 4 to 7 |
| `include/flow_control`, `src/flow_control` | Flow control windows | 7 |
| `include/scheduler`, `src/scheduler` | Packet scheduler (lowest RTT) | 8 |
| `include/congestion_control`, `src/congestion_control` | OLIA, hybrid slow start | 9, 10 |
| `include/connection`, `src/connection` | `Connection`, `Client`, `Server` | 4 to 11 |
| `apps` | The programs `mpq_server`, `mpq_client`, `mpq_reqres`, SHA-256 | 11 |
| `tests/unit` | Tests of one class at a time, no network | |
| `tests/integration` | Tests with real UDP sockets on loopback | |
| `evaluation` | Scripts, scenarios and figures of the evaluation | 12 |
| `docs` | Design notes | |

## Build and run

```
make            # builds the library and the programs into bin/
make test       # runs every test (make test-unit / make test-integration for one group)
```

Server, on a host with two interfaces (10.0.0.2 and 10.0.1.2, directly connected to the client):

```
./bin/mpq_server --listen 10.0.0.2:4433 --listen 10.0.1.2:4433 --dir files
```

Client, on a host with two interfaces (10.0.0.1 and 10.0.1.1). The first `--local` address carries the initial path; a single `--local` gives a single-path transfer.

```
./bin/mpq_client --server 10.0.0.2:4433 --local 10.0.0.1 --local 10.0.1.1 --get big.bin --out big.bin
./bin/mpq_reqres --server 10.0.0.2:4433 --local 10.0.0.1 --local 10.0.1.1 --out delays.csv
```

## Evaluation on four virtual machines

The topology of the paper (Figure 3): a client and a server with two interfaces each, joined by two disjoint paths. A router on each path gives the path its capacity, delay, loss and queue.

```
           10.0.0.0/24                      10.0.2.0/24
        .1 ------------ .254 [router A] .254 ------------ .2
client                                                       server
        .1 ------------ .254 [router B] .254 ------------ .2
           10.0.1.0/24                      10.0.3.0/24
```

The scripts of `evaluation/scripts` are run from the project folder and find the interfaces by their addresses.

| Script | Machine | What it does |
|---|---|---|
| `setup_addresses.sh client\|server\|router-a\|router-b IF1 IF2` | all four | Addresses, routes, forwarding on the routers |
| `shape_links.sh [--rate-a 8mbit --delay-a 10ms --loss-a 0 ...]` | both routers, same command | Capacity, delay, loss and queue of the two paths (`show`, `clear`) |
| `start_server.sh` | server | Test files, `mpq_server`, and the iperf3 servers for TCP, UDP and Multipath TCP |
| `run_transfers.sh [--file F --runs N --label L]` | client | MP-QUIC download over two paths (either network first) and over one path, SHA-256 checked |
| `run_baselines.sh [--bytes 20M --runs N --label L]` | client | UDP, TCP and Multipath TCP on the same links, with iperf3 |
| `run_handover.sh [--single-path \| --no-cut]` | client | Request/response traffic, link A is cut after 3 s |
| `summary.sh [--label L]` | client | Table of the goodputs and the experimental aggregation benefit of the paper |

Measurements are written to `evaluation/results/` (`throughput.csv`, `handover_*.csv`).
