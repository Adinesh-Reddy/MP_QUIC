# MP-QUIC Demonstration & Benchmarking Manual (`demo.md`)

This guide provides end-to-end instructions for running the live demonstration, executing the 4-scenario benchmark suite, and generating comparison graphs comparing **UDP, TCP, MPTCP, and MP-QUIC** on the 4-node testbed.

---

## 1. Testbed Topology & IP Map

```
                 10.0.0.0/24 (mpqa1)                       10.0.2.0/24 (mpqa2)
              .1 ----------------------- .254 [Router A] .254 ----------------------- .2
  MPQ-Client                                                                              MPQ-Server
              .1 ----------------------- .254 [Router B] .254 ----------------------- .2
                 10.0.1.0/24 (mpqb1)                       10.0.3.0/24 (mpqb2)
```

| Node | Interface A (Path A) | Interface B (Path B) | Default Gateway / Route |
|---|---|---|---|
| **MPQ-Client** | `10.0.0.1` (to Router A) | `10.0.1.1` (to Router B) | `10.0.2.0/24 via 10.0.0.254`<br>`10.0.3.0/24 via 10.0.1.254` |
| **MPQ-Server** | `10.0.2.2` (to Router A) | `10.0.3.2` (to Router B) | `10.0.0.0/24 via 10.0.2.254`<br>`10.0.1.0/24 via 10.0.3.254` |
| **Router A** | `10.0.0.254` (client-facing) | `10.0.2.254` (server-facing) | Direct forwarding (`ip_forward=1`) |
| **Router B** | `10.0.1.254` (client-facing) | `10.0.3.254` (server-facing) | Direct forwarding (`ip_forward=1`) |

---

## 2. Pre-Flight Check: Build the Project

On **MPQ-Client** and **MPQ-Server**:
```bash
cd ~/MP_QUIC
make -j$(nproc)
```
Ensure binaries exist in `bin/` (`bin/mpq_server`, `bin/mpq_client`, `bin/mpq_reqres`).

---

## 3. Step-by-Step Demonstration Workflow

### Step 1: Start Server Daemons (on MPQ-Server)
Run this once on **MPQ-Server** and leave it running in its terminal:
```bash
cd ~/MP_QUIC
./evaluation/scripts/start_server.sh
```
**What this does:**
1. Generates test datasets (`test20M.bin`, `test5M.bin`, `test256K.bin`) and computes their SHA-256 digests.
2. Starts `mpq_server` on port `4433` on both `10.0.2.2` and `10.0.3.2`.
3. Starts the baseline `iperf3` server on port `5201` (TCP/UDP) and port `5202` (MPTCP).
4. Logs connections to `evaluation/logs/server_<scenario>.log`.

---

### Step 2: Live Quick Demonstration (on MPQ-Client)
Show this to your professor first to prove MP-QUIC is working:
```bash
cd ~/MP_QUIC
./evaluation/scripts/demo_quick_verify.sh
```
**What happens on screen:**
- Checks ping connectivity to Server over Path A (`10.0.0.1 -> 10.0.2.2`) and Path B (`10.0.1.1 -> 10.0.3.2`).
- Downloads a test file over MP-QUIC aggregating both paths.
- Displays live byte count on Link A vs Link B.
- Validates SHA-256 hash match against the server.
- Prints the green confirmation banner:
  ```text
  ========================================================================
                   🎉 DEMONSTRATION VERIFICATION PASSED! 🎉                 
  ========================================================================
    Status                : OK (Transfer completed seamlessly)
    Total Time            : 1820 ms
    Achieved Goodput      : 11180 kbps (11.18 Mbit/s)
    Active Paths Used     : 2
    SHA-256 Integrity     : MATCHED
  ------------------------------------------------------------------------
    Network Layer Traffic Share:
      - Link A (enp1s0) : 13800 KB (68%)
      - Link B (enp2s0) : 6200 KB (32%)
  ========================================================================
  ```

---

### Step 3: Run the 4 Benchmark Scenarios

The suite evaluates 4 distinct network conditions across 4 protocols (**UDP, TCP, MPTCP, MP-QUIC**):

| Scenario | Aliases | Emulated Profile | Default File | What it Evaluates |
|---|---|---|---|---|
| **Scenario 1** | `1`, `scenario1` | Path A: 8M/10ms <br> Path B: 4M/25ms | `test20M.bin` (20 MB) | **Bandwidth Aggregation:** Asymmetric links. Lowest-RTT scheduler and coupled OLIA saturate both links ($E_{\text{Ben}} \to 1.0$). |
| **Scenario 2** | `2`, `scenario2` | Path A: 8M/10ms, **1% loss** <br> Path B: 4M/25ms, **1% loss** | `test20M.bin` (20 MB) | **Loss Recovery & Robustness:** Cross-path retransmissions and OLIA window backoff under random drops. |
| **Scenario 3** | `3`, `scenario3` | Path A: 20M, **100ms** <br> Path B: 5M, **150ms** (q=200) | `test20M.bin` (20 MB) | **High BDP (Satellite / Transcontinental):** Large delay-bandwidth product and buffer scaling. |
| **Scenario 4** | `4`, `scenario4` | Path A: 8M/10ms <br> Path B: 4M/25ms | **`test256K.bin` (256 KB)** | **Short Transfers (Web Objects):** Handshake, initial burst, and slow start on small transfers. |

---

#### Option A: Run All 4 Scenarios Automatically in Sequence (Recommended)
On **MPQ-Client**:
```bash
./evaluation/scripts/run_all_scenarios.sh
```
**How this works:**
1. The script announces **Scenario 1** and displays the router commands.
2. It **pauses** and waits for you to run:
   - On Router A: `./evaluation/scripts/shape_router.sh 1 --router a`
   - On Router B: `./evaluation/scripts/shape_router.sh 1 --router b`
3. You press **[Enter]** on the Client terminal.
4. It benchmarks Scenario 1, displays the table, and moves to Scenario 2.
5. It repeats for Scenarios 2, 3, and 4, and automatically plots all graphs at the end!

*(Note: If you have configured SSH keys from Client to the routers, you can pass `--auto-ssh` to run all 4 scenarios with zero manual prompts: `./evaluation/scripts/run_all_scenarios.sh --auto-ssh`).*

---

#### Option B: Run Scenarios One by One Manually

##### To run Scenario 1:
1. On Router A: `./evaluation/scripts/shape_router.sh 1 --router a`
2. On Router B: `./evaluation/scripts/shape_router.sh 1 --router b`
3. On Client:   `./evaluation/scripts/run_benchmarks.sh --scenario 1`

##### To run Scenario 2:
1. On Router A: `./evaluation/scripts/shape_router.sh 2 --router a`
2. On Router B: `./evaluation/scripts/shape_router.sh 2 --router b`
3. On Client:   `./evaluation/scripts/run_benchmarks.sh --scenario 2`

##### To run Scenario 3:
1. On Router A: `./evaluation/scripts/shape_router.sh 3 --router a`
2. On Router B: `./evaluation/scripts/shape_router.sh 3 --router b`
3. On Client:   `./evaluation/scripts/run_benchmarks.sh --scenario 3`

##### To run Scenario 4 (Short Transfer 256 KB):
1. On Router A: `./evaluation/scripts/shape_router.sh 4 --router a`
2. On Router B: `./evaluation/scripts/shape_router.sh 4 --router b`
3. On Client:   `./evaluation/scripts/run_benchmarks.sh --scenario 4`

---

### Step 4: The Benchmark Output Tables

After each scenario completes, the client outputs a 4-protocol comparison table:

```text
+------------------------------------------------------------------------------------------------+
|  4-PROTOCOL BENCHMARK RESULTS TABLE - Scenario 1: Low BDP / Asymmetric (8M/10ms + 4M/25ms)    |
+------------------------------------------------------------------------------------------------+
| Protocol   | Configuration              | Runs | Goodput      | Link A / B (%) | EBen  |
+================================================================================================+
| UDP        | UDP Capacity Link A        | 3    |     7.80 Mbps | 100.0% /   0.0% |   -   |
| UDP        | UDP Capacity Link B        | 3    |     3.90 Mbps |   0.0% / 100.0% |   -   |
|------------+----------------------------+------+--------------+----------------+-------|
| TCP        | TCP Single Path A          | 3    |     7.55 Mbps | 100.0% /   0.0% |   -   |
| TCP        | TCP Single Path B          | 3    |     3.75 Mbps |   0.0% / 100.0% |   -   |
|------------+----------------------------+------+--------------+----------------+-------|
| MPTCP      | MPTCP Both (Subflow A 1st) | 3    |    10.85 Mbps |  65.0% /  35.0% |  0.88 |
|------------+----------------------------+------+--------------+----------------+-------|
| MP-QUIC    | MP-QUIC Single Path A      | 3    |     7.52 Mbps | 100.0% /   0.0% |   -   |
| MP-QUIC    | MP-QUIC Single Path B      | 3    |     3.76 Mbps |   0.0% / 100.0% |   -   |
| MP-QUIC    | MP-QUIC Both (Path A first)| 3    |    11.18 Mbps |  70.0% /  30.0% |  0.97 |
| MP-QUIC    | MP-QUIC Both (Path B first)| 3    |    11.12 Mbps |  69.0% /  31.0% |  0.96 |
|------------+----------------------------+------+--------------+----------------+-------|
| UDP CAP    | Total Path Ceiling (A+B)   | -    |    11.70 Mbps |  66.7% /  33.3% |  1.00 |
+------------------------------------------------------------------------------------------------+
| EBen: 1.00 = perfect bandwidth aggregation (using sum of both paths)                           |
|       0.00 = no gain over best single path (single-path equivalent)                            |
+------------------------------------------------------------------------------------------------+
```

Results are stored in:
- `evaluation/results/results_scenario<N>.csv`
- `evaluation/results/table_scenario<N>.txt`
- `evaluation/logs/client_scenario<N>.log`

---

### Step 5: Generate All Graphs at Once

Execute on **MPQ-Client** (or copy `evaluation/results/` to host and run):
```bash
python3 evaluation/plot_benchmarks.py --results-dir evaluation/results --out-dir evaluation/plots
```

This generates 5 publication-grade figures in `evaluation/plots/`:

1. **`fig1_protocol_comparison_4way.png`**:
   Grouped bar chart for each scenario directly comparing:
   - **UDP Capacity** (ceiling)
   - **Single-Path TCP**
   - **MPTCP**
   - **MP-QUIC**
2. **`fig2_aggregation_benefit.png`**:
   Head-to-head bar chart comparing $E_{\text{Ben}}$ of **MP-QUIC vs MPTCP** across all scenarios.
3. **`fig3_detailed_breakdown.png`**:
   Full sub-configuration breakdown (Link A, Link B, and Multipath) across all 4 protocols.
4. **`fig4_traffic_distribution.png`**:
   Stacked bar chart showing percentage of bytes delivered on Link A vs Link B.
5. **`fig5_summary_dashboard.png`**:
   **The All-In-One 4-Panel Master Slide** containing the 4-way goodput chart, $E_{\text{Ben}}$ score, traffic distribution, and complete numerical table in a single image.

---

## 4. Resetting Link Shaping

When testing is finished, reset the routers:
- On Router A: `./evaluation/scripts/shape_router.sh reset --router a`
- On Router B: `./evaluation/scripts/shape_router.sh reset --router b`
