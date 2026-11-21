# Multipath QUIC (MP-QUIC)

**Project:** Design, Implementation, and Evaluation of Multipath QUIC  
**Team Members:** `cs24btech11039`, `cs24btech11026`, `cs24btech11009`, `cs24btech11027`, `cs24btech11045`, `cs24btech11016`  
**Reference Paper:** Q. De Coninck and O. Bonaventure, "Multipath QUIC: Design and Evaluation," ACM CoNEXT 2017.

## 1. About the Project

Normal TCP locks a connection to one IP address and port. If you have two links like Wi-Fi and LTE, TCP only uses one link at a time, and if that link drops, the connection breaks. Kernel Multipath TCP (MPTCP) can use multiple links, but it requires OS kernel support, root configuration, and often gets stripped by middleboxes or firewalls.

MP-QUIC solves this in **user-space over standard UDP** (written in C++17 using Linux `epoll`):
- Uses a single **64-bit Connection ID (CID)** to manage multiple paths (Path A + Path B) at the same time.
- Keeps **independent packet numbers** (`1, 2, 3...`) per path so latency differences across paths never cause false loss detection.
- Supports **cross-path retransmission**: if a packet drops on a slow or broken link, its stream data is immediately resent on the healthy path without stalling the stream.

## 2. 7-Layer Protocol Stack

| Layer | Folder | What it does |
|---|---|---|
| **Layer 7: Applications** | `apps/` | `mpq_server`, `mpq_client` (file download with SHA-256 check), and `mpq_reqres` (request-response delay & handover test). |
| **Layer 6: Packet Scheduler** | `src/scheduler/` | **Lowest-RTT (MinRTT) Scheduler:** Sends packets on the lowest-RTT path first, and spills traffic to the second path only when the fast path's congestion window (`cwnd`) is full. |
| **Layer 5: Congestion Control** | `src/congestion_control/` | **Coupled OLIA + HyStart:** Couples window growth across paths for fairness at shared bottlenecks while steering traffic to cleaner links. On packet loss, only the lossy path halves its window. |
| **Layer 4: Reliability & Flow Control** | `src/reliability/`<br>`src/flow_control/` | Per-path packet numbering, Selective ACKs (SACK ranges), Tail Loss Probe (TLP), Retransmission Timeout (RTO), in-order byte sorting (`StreamFrameSorter`), and dynamic window autotuning (`WINDOW_UPDATE` triggered at 50% remaining buffer space). |
| **Layer 3: Connection & Paths** | `src/connection/`<br>`src/path/` | Maps 1 Connection ID to multiple paths. Exchanges extra IPs via `ADD_ADDRESS`, probes paths with `PING`, handles NAT rebinding, and marks a path as Potentially Failed (PF) when an RTO fires (~100-200 ms). |
| **Layer 2: Packets & Frames** | `src/packet/` | **17-byte header** (`8B Connection ID` + `1B Path ID` + `8B Packet Number`) + frames (`STREAM 0x01`, `ACK 0x02`, `CONNECTION_CLOSE 0x03`, `WINDOW_UPDATE 0x04`, `PING 0x07`, `ADD_ADDRESS 0x10`, `PATHS 0x12`) inside a 1350-byte max UDP payload. |
| **Layer 1: Sockets & Event Loop** | `src/sockets/` | Non-blocking UDP sockets (`SOCK_NONBLOCK`, 4 MB `SO_RCVBUF`/`SO_SNDBUF`) running on a single-threaded Linux `epoll()` loop. |

## 3. 4-VM Testbed Setup & IP Addresses

We run our experiments on 4 KVM VMs connected across two isolated paths (Path A and Path B) using Linux virtual bridges (`br-left-a`, `br-right-a`, `br-left-b`, `br-right-b`):

```text
                  Path A (10.0.0.0/24)                        Path A (10.0.2.0/24)
             eth0: 10.0.0.1 -------- 10.0.0.254 [Router-A] 10.0.2.254 -------- eth0: 10.0.2.2
[MPQ-Client]                                    (VyOS VM)                                     [MPQ-Server]
 (Ubuntu VM)                                                                                   (Ubuntu VM)
             eth1: 10.0.1.1 -------- 10.0.1.254 [Router-B] 10.0.3.254 -------- eth1: 10.0.3.2
                  Path B (10.0.1.0/24)          (VyOS VM)     Path B (10.0.3.0/24)
```

| VM Name | OS | Interface A (Path A) | Interface B (Path B) | Role |
|---|---|---|---|---|
| **MPQ-Client** | Ubuntu Linux | `eth0`: `10.0.0.1/24` | `eth1`: `10.0.1.1/24` | Runs `mpq_client`, `mpq_reqres`, `iperf3 -c`, and the benchmark scripts |
| **MPQ-Server** | Ubuntu Linux | `eth0`: `10.0.2.2/24` | `eth1`: `10.0.3.2/24` | Runs `mpq_server` (port `4433`), `iperf3 -s` (port `5201`), and MPTCP `iperf3 -s` (port `5202`) |
| **Router-A** | VyOS Router | `eth0`: `10.0.0.254/24` | `eth1`: `10.0.2.254/24` | Routes Path A traffic and applies `tc netem` rate, delay, and loss |
| **Router-B** | VyOS Router | `eth0`: `10.0.1.254/24` | `eth1`: `10.0.3.254/24` | Routes Path B traffic and applies `tc netem` rate, delay, and loss |

## 4. Ports Used on the Server VM

- **Port `5201` (TCP):** Single-Path TCP (`iperf3 -s -p 5201`)
- **Port `5202` (MPTCP):** Multipath TCP (`mptcpize run iperf3 -s -p 5202`)
- **Port `4433` (UDP):** Single-Path QUIC and Multipath QUIC (`./bin/mpq_server --listen 10.0.2.2:4433 --listen 10.0.3.2:4433`)

## 5. What to Run in Each VM (Using SSH)

### Step 1: Set Up Passwordless SSH from Client VM to Both Routers
The benchmark scripts on **MPQ-Client** use SSH to log into **Router-A** (`vyos@10.0.0.254`) and **Router-B** (`vyos@10.0.1.254`) and apply `tc netem` rules automatically.

Run this once on **MPQ-Client**:
```bash
ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519
ssh-copy-id vyos@10.0.0.254
ssh-copy-id vyos@10.0.1.254

# Verify SSH works without password
ssh vyos@10.0.0.254 "echo Router A ready"
ssh vyos@10.0.1.254 "echo Router B ready"
```

### Step 2: Commands to Run on MPQ-Server VM (`10.0.2.2` / `10.0.3.2`)

SSH into **MPQ-Server**:
```bash
ssh <user>@10.0.2.2
```

1. **Build the project:**
   ```bash
   cd ~/MP_QUIC
   make -j$(nproc)
   ```
   Make sure `test20M.bin` (20 MB) and `test256K.bin` (256 KB) are present in `~/MP_QUIC/`. If needed, create them with:
   ```bash
   head -c 20971520 /dev/urandom > test20M.bin
   head -c 262144 /dev/urandom > test256K.bin
   ```

2. **Configure MPTCP endpoints on the server:**
   ```bash
   sudo sysctl -w net.mptcp.enabled=1
   sudo ip mptcp limits set subflows 2 add_addr_accepted 2
   sudo ip mptcp endpoint flush
   sudo ip mptcp endpoint add 10.0.2.2 dev eth0 signal
   sudo ip mptcp endpoint add 10.0.3.2 dev eth1 signal
   ```

3. **Start `iperf3` (port 5201), MPTCP `iperf3` (port 5202), and `mpq_server` (port 4433):**
   ```bash
   # Single-Path TCP iperf3 server on port 5201
   iperf3 -s -p 5201 -D

   # Multipath TCP iperf3 server on port 5202
   mptcpize run iperf3 -s -p 5202 -D

   # MP-QUIC server on port 4433 listening on both Path A and Path B
   ./bin/mpq_server --listen 10.0.2.2:4433 --listen 10.0.3.2:4433 --dir .
   ```

### Step 3: Commands on Router-A (`10.0.0.254`) and Router-B (`10.0.1.254`)

You do not need to run commands manually on the routers during the benchmarks because the client scripts shape them over SSH. If you want to SSH in and set or clear `tc netem` manually:

```bash
# On Router-A
ssh vyos@10.0.0.254
sudo tc qdisc replace dev eth0 root netem rate 8mbit delay 10ms limit 50 loss 0%
sudo tc qdisc replace dev eth1 root netem rate 8mbit delay 10ms limit 50 loss 0%

# On Router-B
ssh vyos@10.0.1.254
sudo tc qdisc replace dev eth0 root netem rate 4mbit delay 25ms limit 50 loss 0%
sudo tc qdisc replace dev eth1 root netem rate 4mbit delay 25ms limit 50 loss 0%
```

### Step 4: Commands to Run on MPQ-Client VM (`10.0.0.1` / `10.0.1.1`)

SSH into **MPQ-Client**:
```bash
ssh <user>@10.0.0.1
```

1. **Build the project:**
   ```bash
   cd ~/MP_QUIC
   make -j$(nproc)
   ```

2. **Configure MPTCP subflows on the client:**
   ```bash
   sudo sysctl -w net.mptcp.enabled=1
   sudo ip mptcp limits set subflows 2 add_addr_accepted 2
   sudo ip mptcp endpoint flush
   sudo ip mptcp endpoint add 10.0.0.1 dev eth0 subflow
   sudo ip mptcp endpoint add 10.0.1.1 dev eth1 subflow
   ```

## 6. Running Quick Manual Tests from MPQ-Client

- **Single-Path TCP (Port 5201):**
  ```bash
  iperf3 -c 10.0.2.2 -B 10.0.0.1 -p 5201 -R -n 20M
  iperf3 -c 10.0.3.2 -B 10.0.1.1 -p 5201 -R -n 20M
  ```

- **Multipath TCP (Port 5202):**
  ```bash
  mptcpize run iperf3 -c 10.0.2.2 -B 10.0.0.1 -p 5202 -R -n 20M
  ```

- **Single-Path QUIC (Port 4433):**
  ```bash
  ./bin/mpq_client --server 10.0.2.2:4433 --local 10.0.0.1 --get test20M.bin --out /tmp/out.bin
  ```

- **Multipath QUIC (Port 4433):**
  ```bash
  ./bin/mpq_client --server 10.0.2.2:4433 --local 10.0.0.1 --local 10.0.1.1 --get test20M.bin --out /tmp/out.bin
  ```

## 7. Running the 3 Benchmark Scenarios & Plotting Graphs

Run these from `~/MP_QUIC` on **MPQ-Client**:

### Scenario 1: Bulk 20 MB Transfers (20 Network Profiles)
Tests 20 MB downloads (`test20M.bin`) across 20 asymmetric scenarios varying bandwidth (5-25 Mbps), delay (5-50 ms), queue size, and loss (0-2%) across TCP, MPTCP, QUIC, and MP-QUIC (3 runs each, median taken).
```bash
./scripts/run_scenario1_20.sh
python3 scripts/plot_scenario1.py results_scenario1_20.csv
```
- **Saved CSV:** `results_scenario1_20.csv`
- **Saved Plots:** `time_ratio_cdf_20.png`, `eben_comparison_20.png`

### Scenario 2: Short 256 KB Downloads (10 Network Profiles)
Tests 256 KB downloads (`test256K.bin`) across 10 low-BDP scenarios (0% loss) to compare handshake latency and short-flow completion times across all 4 protocols.
```bash
./scripts/run_scenario2_short.sh
python3 scripts/plot_scenario2_short.py results_scenario2_short.csv
```
- **Saved CSV:** `results_scenario2_short.csv`
- **Saved Plots:** `completion_time_cdf.png`, `eben_short_boxplot.png`

### Scenario 3: Active Handover & Path Failure
Shapes Path A to `8 Mbps / 10 ms` and Path B to `4 Mbps / 25 ms`, sends 35 requests (750 B every 400 ms) using `./bin/mpq_reqres`, and cuts Path A (`100% loss`) at `t = 3.2s` to measure failover to Path B.
```bash
./scripts/run_scenario3_handover.sh
```
- **Saved CSVs:** `scripts/results_scenario3_handover.csv`, `scripts/results_scenario3_single_fail.csv`
- **Saved Plots:** `handover_delay.png`, `handover_cdf.png`
