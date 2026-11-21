# Multipath QUIC (MP-QUIC) — User-Space Multipath Transport Protocol

**Course Project:**  Multipath QUIC  
**Team Members:** `cs24btech11039`, `cs24btech11026`, `cs24btech11009`, `cs24btech11027`, `cs24btech11045`, `cs24btech11016`  
**Reference Paper:** Q. De Coninck and O. Bonaventure, *"Multipath QUIC: Design and Evaluation,"* ACM CoNEXT 2017.

---

## 1. What is this Project?

Normal TCP locks a connection to a single IP address and port. If you have both Wi-Fi and Mobile Data (LTE/5G), TCP only uses one link at a time, and if that link drops, your connection breaks. Kernel Multipath TCP (MPTCP) tries to fix this, but it needs OS kernel changes, root setup, and often gets blocked or stripped by middleboxes and firewalls.

**MP-QUIC** solves this completely in **user-space over standard UDP** (using C++17 and Linux `epoll`).
- It uses one **64-bit Connection ID (CID)** to combine multiple network interfaces (Path A + Path B) at the same time.
- Every path keeps its own **independent packet numbers** (`1, 2, 3...`), so a delay on one link never looks like a fake loss on another link.
- If a packet is lost on a slow or broken link, MP-QUIC immediately resends that data on the fast working link (**cross-path retransmission**) without stalling the connection.

---

## 2. How the 7-Layer Architecture Works

Our code is organized into a clean 7-layer stack inside `include/`, `src/`, and `apps/`:

| Layer | Folder | What it does in simple terms |
|---|---|---|
| **Layer 7: Applications** | `apps/` | `mpq_server`, `mpq_client` (file download with SHA-256 check), and `mpq_reqres` (request-response delay & handover tester). |
| **Layer 6: Packet Scheduler** | `src/scheduler/` | **Lowest-RTT (MinRTT) Scheduler with Overflow Spilling:** Sends all packets on the fastest link first. Only when the fast link's congestion window (`cwnd`) is completely full does it spill extra packets over to the slower link. |
| **Layer 5: Congestion Control** | `src/congestion_control/` | **Coupled OLIA + HyStart:** Coordinates window growth across both paths so it is fair to normal TCP users at shared bottlenecks, while pushing more traffic onto the cleaner link. On loss, only the lossy path halves its window. |
| **Layer 4: Reliability & Flow Control** | `src/reliability/`<br>`src/flow_control/` | Per-path sequence numbers, Selective ACKs (SACK ranges), Tail Loss Probe (TLP), Retransmission Timeout (RTO), in-order byte sorting (`StreamFrameSorter`), and dynamic flow-control window autotuning (sends `WINDOW_UPDATE` at 50% buffer room so the sender never pauses). |
| **Layer 3: Connection & Paths** | `src/connection/`<br>`src/path/` | Maps 1 Connection ID to multiple paths. Exchanges extra IPs via `ADD_ADDRESS`, probes paths with `PING`, handles NAT rebinding, and marks a path as **Potentially Failed (PF)** in ~1 RTT (~100–200 ms) when an RTO fires. |
| **Layer 2: Packets & Frames** | `src/packet/` | **17-byte header** (`8B Connection ID` + `1B Path ID` + `8B Packet Number`) + frames (`STREAM 0x01`, `ACK 0x02`, `CONNECTION_CLOSE 0x03`, `WINDOW_UPDATE 0x04`, `PING 0x07`, `ADD_ADDRESS 0x10`, `PATHS 0x12`) inside a 1350-byte max UDP packet. |
| **Layer 1: Sockets & Event Loop** | `src/sockets/` | Non-blocking UDP sockets (`SOCK_NONBLOCK` with 4 MB `SO_RCVBUF`/`SO_SNDBUF`) running on a single-threaded Linux `epoll()` loop. |

---

## 3. 4-VM Testbed Topology & IP Addresses

We test everything on 4 KVM virtual machines connected through isolated Linux virtual bridges (`br-left-a`, `br-right-a`, `br-left-b`, `br-right-b`) so Path A and Path B are completely separate:

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
| **MPQ-Client** | Ubuntu Linux | `eth0`: `10.0.0.1/24` | `eth1`: `10.0.1.1/24` | Runs `mpq_client`, `mpq_reqres`, `iperf3 -c`, and all benchmark scripts |
| **MPQ-Server** | Ubuntu Linux | `eth0`: `10.0.2.2/24` | `eth1`: `10.0.3.2/24` | Runs `mpq_server` (port `4433`), `iperf3 -s` (port `5201`), and MPTCP `iperf3 -s` (port `5202`) |
| **Router-A** | VyOS Router | `eth0`: `10.0.0.254/24` | `eth1`: `10.0.2.254/24` | Forwards Path A traffic & applies `tc netem` rate/delay/loss |
| **Router-B** | VyOS Router | `eth0`: `10.0.1.254/24` | `eth1`: `10.0.3.254/24` | Forwards Path B traffic & applies `tc netem` rate/delay/loss |

---

## 4. Ports We Use (Summary)

When we run the benchmarks, the Server VM listens on **3 ports**:
1. **Port `5201` (TCP)** — Normal **Single-Path TCP** using `iperf3 -s -p 5201`
2. **Port `5202` (MPTCP)** — **Multipath TCP** using `mptcpize run iperf3 -s -p 5202`
3. **Port `4433` (UDP)** — **QUIC & Multipath QUIC** using `./bin/mpq_server --listen 10.0.2.2:4433 --listen 10.0.3.2:4433`

---

## 5. Step-by-Step: What to Run in Each VM (Using SSH)

### Step 1: Set Up Passwordless SSH from Client VM to the Two Routers
Our benchmark scripts (`run_scenario1_20.sh`, `run_scenario2_short.sh`, `run_scenario3_handover.sh`) run on **MPQ-Client** and automatically SSH into **Router-A** (`vyos@10.0.0.254`) and **Router-B** (`vyos@10.0.1.254`) to change bandwidth, delay, and packet loss using `tc netem`.

On **MPQ-Client**, run this once so SSH does not ask for a password every time:
```bash
# Generate an SSH key if you don't already have one
ssh-keygen -t ed25519 -N "" -f ~/.ssh/id_ed25519

# Copy your SSH key to Router-A and Router-B (default VyOS password is usually 'vyos')
ssh-copy-id vyos@10.0.0.254
ssh-copy-id vyos@10.0.1.254

# Test that both routers respond without asking for a password
ssh vyos@10.0.0.254 "echo Router A ready"
ssh vyos@10.0.1.254 "echo Router B ready"
```

---

### Step 2: What to Run on **MPQ-Server VM** (`10.0.2.2` / `10.0.3.2`)

First, SSH into the **MPQ-Server VM** from your host or client:
```bash
ssh <user>@10.0.2.2
```

#### 1. Build the code and check test files
```bash
cd ~/MP_QUIC
make -j$(nproc)
```
Make sure `test20M.bin` (20 MB) and `test256K.bin` (256 KB) are inside `~/MP_QUIC/`. If you ever need to recreate them:
```bash
head -c 20971520 /dev/urandom > test20M.bin
head -c 262144 /dev/urandom > test256K.bin
```

#### 2. Enable Kernel MPTCP Endpoints on Server
So that MPTCP knows the server has two IP addresses (`10.0.2.2` on `eth0` and `10.0.3.2` on `eth1`), run:
```bash
sudo sysctl -w net.mptcp.enabled=1
sudo ip mptcp limits set subflows 2 add_addr_accepted 2
sudo ip mptcp endpoint flush
sudo ip mptcp endpoint add 10.0.2.2 dev eth0 signal
sudo ip mptcp endpoint add 10.0.3.2 dev eth1 signal
```

#### 3. Start `iperf3` (Port `5201`), MPTCP `iperf3` (Port `5202`), and `mpq_server` (Port `4433`)
Run these commands on **MPQ-Server**:
```bash
# 1. Start normal Single-Path TCP iperf3 server in background on port 5201
iperf3 -s -p 5201 -D

# 2. Start Multipath TCP (MPTCP) iperf3 server in background on port 5202
mptcpize run iperf3 -s -p 5202 -D

# 3. Start MP-QUIC server in foreground listening on BOTH Path A and Path B on port 4433
./bin/mpq_server --listen 10.0.2.2:4433 --listen 10.0.3.2:4433 --dir .
```
Leave this terminal open on `MPQ-Server` so you can watch the incoming connections and per-path stats.

---

### Step 3: What Runs on **Router-A** (`10.0.0.254`) and **Router-B** (`10.0.1.254`)

You normally **do not need to run anything manually** on Router-A or Router-B because the scripts on `MPQ-Client` automatically SSH into `vyos@10.0.0.254` and `vyos@10.0.1.254` to run `tc netem`.

However, if you want to SSH into the routers manually to check or set link shaping yourself:

- **SSH into Router-A (`10.0.0.254`):**
  ```bash
  ssh vyos@10.0.0.254
  # Example: Shape Path A to 8 Mbps, 10 ms delay, queue limit 50, 0% loss
  sudo tc qdisc replace dev eth0 root netem rate 8mbit delay 10ms limit 50 loss 0%
  sudo tc qdisc replace dev eth1 root netem rate 8mbit delay 10ms limit 50 loss 0%
  ```
- **SSH into Router-B (`10.0.1.254`):**
  ```bash
  ssh vyos@10.0.1.254
  # Example: Shape Path B to 4 Mbps, 25 ms delay, queue limit 50, 0% loss
  sudo tc qdisc replace dev eth0 root netem rate 4mbit delay 25ms limit 50 loss 0%
  sudo tc qdisc replace dev eth1 root netem rate 4mbit delay 25ms limit 50 loss 0%
  ```
- **To clear/reset shaping on a router:**
  ```bash
  sudo tc qdisc del dev eth0 root
  sudo tc qdisc del dev eth1 root
  ```

---

### Step 4: What to Run on **MPQ-Client VM** (`10.0.0.1` / `10.0.1.1`)

SSH into the **MPQ-Client VM**:
```bash
ssh <user>@10.0.0.1
```

#### 1. Build the binaries
```bash
cd ~/MP_QUIC
make -j$(nproc)
```

#### 2. Enable Kernel MPTCP Subflows on Client
So that the client automatically opens a second MPTCP subflow over `eth1` (`10.0.1.1`) when talking to port `5202`:
```bash
sudo sysctl -w net.mptcp.enabled=1
sudo ip mptcp limits set subflows 2 add_addr_accepted 2
sudo ip mptcp endpoint flush
sudo ip mptcp endpoint add 10.0.0.1 dev eth0 subflow
sudo ip mptcp endpoint add 10.0.1.1 dev eth1 subflow
```

---

## 6. Running Individual Tests Manually (From MPQ-Client)

If you want to test each protocol one by one from **MPQ-Client** before running the full benchmark scripts:

1. **Test Single-Path TCP (Port `5201`):**
   ```bash
   # Over Path A
   iperf3 -c 10.0.2.2 -B 10.0.0.1 -p 5201 -R -n 20M
   # Over Path B
   iperf3 -c 10.0.3.2 -B 10.0.1.1 -p 5201 -R -n 20M
   ```

2. **Test Multipath TCP (Port `5202`):**
   ```bash
   # Starts on Path A and automatically adds Path B subflow
   mptcpize run iperf3 -c 10.0.2.2 -B 10.0.0.1 -p 5202 -R -n 20M
   ```

3. **Test Single-Path QUIC (Port `4433`):**
   ```bash
   # Only give one --local IP so it stays on Path A
   ./bin/mpq_client --server 10.0.2.2:4433 --local 10.0.0.1 --get test20M.bin --out /tmp/out.bin
   ```

4. **Test Multipath QUIC (Port `4433`):**
   ```bash
   # Pass both --local IPs so MP-QUIC aggregates Path A (10.0.0.1) and Path B (10.0.1.1)
   ./bin/mpq_client --server 10.0.2.2:4433 --local 10.0.0.1 --local 10.0.1.1 --get test20M.bin --out /tmp/out.bin
   ```

---

## 7. Running the 3 Benchmark Scenarios & Generating Plots

All three evaluation scripts are inside `scripts/` and should be run from the root folder (`~/MP_QUIC`) on **MPQ-Client** while `mpq_server` and `iperf3` are running on **MPQ-Server**.

### Scenario 1: Bulk 20 MB Transfers (20 Asymmetric Network Scenarios)
- **What it tests:** Downloads `test20M.bin` (20 MB) across 20 different combinations of bandwidth (5–25 Mbps), delay (5–50 ms), queue size, and random packet loss (0–2%) over all 4 protocols (**TCP, MPTCP, Single-Path QUIC, MP-QUIC**), taking the median of 3 runs per scenario.
- **Commands to run on MPQ-Client:**
  ```bash
  cd ~/MP_QUIC
  ./scripts/run_scenario1_20.sh
  python3 scripts/plot_scenario1.py results_scenario1_20.csv
  ```
- **Outputs generated:**
  - `results_scenario1_20.csv` (raw goodput and $E_{\text{Ben}}$ numbers)
  - `time_ratio_cdf_20.png` (Download Time Ratio CDF showing MP-QUIC beating MPTCP in 95% of runs)
  - `eben_comparison_20.png` (Two-path aggregation benefit $E_{\text{Ben}}$ boxplot)

---

### Scenario 2: Short 256 KB Transfers (10 Low-BDP Scenarios)
- **What it tests:** Downloads `test256K.bin` (256 KB) across 10 scenarios varying bandwidth and RTT (0% loss) to show how handshake latency dominates small web transfers (~3.2x faster on QUIC vs TCP) and why multipath doesn't help tiny files that finish in 3–4 RTTs.
- **Commands to run on MPQ-Client:**
  ```bash
  cd ~/MP_QUIC
  ./scripts/run_scenario2_short.sh
  python3 scripts/plot_scenario2_short.py results_scenario2_short.csv
  ```
- **Outputs generated:**
  - `results_scenario2_short.csv` (completion time in microseconds, goodput, and $E_{\text{Ben}}$)
  - `completion_time_cdf.png` (CDF of download completion time in $\mu\text{s}$)
  - `eben_short_boxplot.png` ($E_{\text{Ben}}$ boxplot for short transfers)

---

### Scenario 3: Active Link Handover & Path Failure
- **What it tests:** Shapes Path A to `8 Mbps / 10 ms` and Path B to `4 Mbps / 25 ms`, then runs `./bin/mpq_reqres` sending a 750-byte request every 400 ms (35 requests total). At **`t = 3.2s`**, the script completely cuts Path A (`100% packet loss`).
  - **Single-Path QUIC** gets stuck on dead Path A and aborts.
  - **MP-QUIC** detects the failure in ~200 ms (RTO triggers Potentially Failed state), requeues unacked packets onto Path B, and delivers **100% of requests (35/35)** without dropping the connection.
- **Commands to run on MPQ-Client:**
  ```bash
  cd ~/MP_QUIC
  ./scripts/run_scenario3_handover.sh
  ```
  *(Note: `run_scenario3_handover.sh` automatically calls `plot_scenario3_handover.py` at the end and restores Router-A's traffic control filters when finished).*
- **Outputs generated:**
  - `scripts/results_scenario3_handover.csv` & `scripts/results_scenario3_single_fail.csv`
  - `handover_delay.png` (Request round-trip delay over time showing the failover spike at `t = 3.2s` and recovery on Path B)
  - `handover_cdf.png` (Request completion delay CDF comparing MP-QUIC 35/35 delivery vs Single-Path failure)
