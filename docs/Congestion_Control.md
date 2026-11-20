# MP-QUIC Technical Reference: Congestion Control Architecture

This document provides a comprehensive, file-by-file technical reference for the **`congestion_control/`** subsystem in MP-QUIC. It covers the abstract interface contracts, Hybrid Slow Start (HyStart), the Opportunistic Linked-Increases Algorithm (OLIA), 128-bit fixed-point arithmetic, loss recovery behavior, and the test suite.

---

## Table of Contents
1. [Architectural Overview & Core Philosophy](#1-architectural-overview--core-philosophy)
2. [`include/congestion_control/congestion.hpp` & `src/congestion_control/congestion.cpp`](#2-includecongestion_controlcongestionhpp--srccongestion_controlcongestioncpp)
   - 2.1 Default Constants (`namespace CongestionDefaults`)
   - 2.2 The `CongestionController` Interface
   - 2.3 The Deterministic `FixedWindowCongestion` Class
3. [`include/congestion_control/hybrid_slow_start.hpp` & `src/congestion_control/hybrid_slow_start.cpp`](#3-includecongestion_controlhybrid_slow_starthpp--srccongestion_controlhybrid_slow_startcpp)
   - 3.1 Motivation: Bufferbloat Prevention
   - 3.2 Round Boundaries & Sample Collection
   - 3.3 Delay Threshold Calculation
   - 3.4 Early Exit Conditions & RTO Resets
4. [`include/congestion_control/olia_sender.hpp` & `src/congestion_control/olia_sender.cpp`](#4-includecongestion_contrololia_senderhpp--srccongestion_contrololia_sendercpp)
   - 4.1 Internal State & Sizing Parameters
   - 4.2 The Coupled OLIA Equation Explained
   - 4.3 Path Quality & Epsilon ($\epsilon$) Capacity Shifting
   - 4.4 128-Bit Fixed-Point Arithmetic & Fractional Accumulation
   - 4.5 Window Limiting (`window_is_limiting`) & Burst Tolerances
   - 4.6 Loss Response & Single-Path Recovery Isolation
   - 4.7 Retransmission Timeout (RTO) Reset
5. [Unit Test Verification (`tests/unit/test_congestion.cpp`)](#5-unit-test-verification-testsunittest_congestioncpp)
6. [Academic References & Literature Foundations](#6-academic-references--literature-foundations)

---

## 1. Architectural Overview & Core Philosophy

In single-path transport, a congestion controller adjusts a single window ($CWND$) to match the bottleneck link. In **Multipath transport**, running uncoupled independent controllers on each path introduces two fatal flaws:

1. **Unfairness to Single-Path Flows:** If an MP-QUIC flow uses 2 paths across a shared bottleneck router competing with a single-path TCP stream, uncoupled additive increases would award the MP-QUIC connection **$2\times$ the bandwidth**, starving the TCP flow.
2. **Failure to Reroute Around Congestion:** If Path 0 is fast with zero loss and Path 1 experiences heavy congestion, an uncoupled protocol continues to blast packets into Path 1. A coupled multipath controller must dynamically **shift traffic away from the congested path onto the higher-quality path**.

MP-QUIC Mini solves this with **OLIA (Opportunistic Linked-Increases Algorithm)** coupled across all path senders, initialized with **Hybrid Slow Start (HyStart)**.

```
┌────────────────────────────────────────────────────────────────────────┐
│                              Connection                                │
│                     (Links all paths via peers table)                  │
└───────────────────┬────────────────────────────────┬───────────────────┘
                    │                                │
                    ▼                                ▼
         ┌─────────────────────┐          ┌─────────────────────┐
         │    Path 0 (Wi-Fi)   │          │    Path 1 (LTE)     │
         ├─────────────────────┤          ├─────────────────────┤
         │ RttStats (SRTT = 20)│          │ RttStats (SRTT = 80)│
         │ OliaSender          │◄────────►│ OliaSender          │
         │ (HybridSlowStart)   │  Shared  │ (HybridSlowStart)   │
         │ CWND = 64 packets   │ Coupling │ CWND = 16 packets   │
         └─────────────────────┘          └─────────────────────┘
```

---

## 2. `include/congestion_control/congestion.hpp` & `src/congestion_control/congestion.cpp`

This file pair defines the baseline constants, abstract class contract, and deterministic test stub.

### 2.1 Default Constants (`namespace CongestionDefaults`)

Defined in [`include/congestion_control/congestion.hpp` (lines 8–15)](file:///home/adinesh-reddy/MP_QUIC/include/congestion_control/congestion.hpp#L8-L15):
```cpp
namespace CongestionDefaults
{
    constexpr uint64_t MSS_BYTES = 1460;
    constexpr uint64_t INITIAL_WINDOW_PACKETS = 32; // start window
    constexpr uint64_t MIN_WINDOW_PACKETS = 2;      // min window after loss
    constexpr uint64_t MAX_WINDOW_PACKETS = 2500;   // max ~3.6 MB per path
}
```

* **`MSS_BYTES = 1460`:** Congestion windows are tracked in integer packet counts ($w$). Multiplication by 1460 bytes converts packet counts to byte limits for flow and socket comparisons.
* **`INITIAL_WINDOW_PACKETS = 32` (~46.7 KiB):** Modern standard TCP uses 10 packets. MP-QUIC uses 32 packets to aggressively utilize high-bandwidth links from the first RTT without waiting through multiple slow-start rounds.
* **`MIN_WINDOW_PACKETS = 2`:** The absolute floor. After severe loss or RTO, $CWND$ never drops below 2. This guarantees that the sender can transmit 2 packets back-to-back, satisfying the receiver's `DATA_PACKETS_PER_ACK = 2` rule and eliciting an immediate ACK without waiting for a 25 ms delayed ACK timer.
* **`MAX_WINDOW_PACKETS = 2500` (~3.65 MiB):** Upper cap on path memory.

---

### 2.2 The `CongestionController` Interface

[`include/congestion_control/congestion.hpp` (lines 17–36)](file:///home/adinesh-reddy/MP_QUIC/include/congestion_control/congestion.hpp#L17-L36):
```cpp
class CongestionController
{
public:
    virtual ~CongestionController() {}

    virtual void on_packet_sent(uint64_t now_us, uint64_t bytes_in_flight, uint64_t packet_number,
                                uint64_t bytes, bool is_retransmittable) = 0;
    virtual void on_packet_acked(uint64_t packet_number, uint64_t acked_bytes, uint64_t bytes_in_flight) = 0;
    virtual void on_packet_lost(uint64_t packet_number, uint64_t lost_bytes, uint64_t bytes_in_flight) = 0;
    virtual void on_retransmission_timeout(bool packets_retransmitted) = 0;
    virtual void maybe_exit_slow_start() = 0;
    virtual uint64_t get_congestion_window() const = 0; // in bytes
    virtual string get_name() const = 0;
};
```

#### Lifecycle Hooks:
1. **`on_packet_sent`:** Called by `SentPacketHandler::sent_packet`. Stretches `newest_sent` and notifies HyStart. Pure ACK packets (`is_retransmittable == false`) are ignored.
2. **`on_packet_acked`:** Triggered when an incoming SACK acknowledges packet numbers. Advances running `acked_total`, triggers HyStart round checks, and invokes window growth.
3. **`on_packet_lost`:** Triggered by time-based loss detection or RTO. Halves the window on that specific path.
4. **`on_retransmission_timeout`:** Fired when total RTO expires. Resets window to `MIN_WINDOW_PACKETS = 2`, resets HyStart, and clears coupling state.
5. **`maybe_exit_slow_start`:** Evaluates HyStart delay conditions upon every new RTT measurement.
6. **`get_congestion_window`:** Returns $w \times 1460$ bytes to determine if `Path::sending_allowed()` permits transmission.

---

### 2.3 The Deterministic `FixedWindowCongestion` Class

Defined in [`src/congestion_control/congestion.cpp`](file:///home/adinesh-reddy/MP_QUIC/src/congestion_control/congestion.cpp):
* Stores a fixed `congestion_window` packet count (default 32).
* All lifecycle hooks (`on_packet_sent`, `on_packet_acked`, `on_packet_lost`, `on_retransmission_timeout`, `maybe_exit_slow_start`) are empty no-ops (`{}`).
* **Why it exists:** Provides deterministic behavior for unit testing loss recovery, packet sorters, and schedulers without dynamic window interference.

---

## 3. `include/congestion_control/hybrid_slow_start.hpp` & `src/congestion_control/hybrid_slow_start.cpp`

### 3.1 Motivation: Bufferbloat Prevention
Standard TCP Slow Start doubles $CWND$ every RTT ($+1$ packet per ACK) until a router queue overflows and drops packets. In modern networks, large router buffers (Bufferbloat) cause latency to skyrocket by hundreds of milliseconds before any drop occurs. 

**HyStart exits slow start early by measuring delay increase rather than waiting for loss.**

---

### 3.2 Constants and State Variables

In [`include/congestion_control/hybrid_slow_start.hpp`](file:///home/adinesh-reddy/MP_QUIC/include/congestion_control/hybrid_slow_start.hpp#L33-L48):
```cpp
static constexpr uint64_t MIN_EXIT_WINDOW_PACKETS = 16;
static constexpr uint32_t SAMPLES_PER_ROUND = 8;
static constexpr uint32_t RISE_SHIFT = 3;           // 1/8 (min_rtt / 8)
static constexpr uint64_t MIN_RISE_US = 4000;       // 4 ms floor
static constexpr uint64_t MAX_RISE_US = 16000;      // 16 ms ceiling

uint64_t round_last_packet = 0;
uint64_t newest_sent = 0;
bool round_open = false;
uint64_t round_min_rtt = 0;
uint32_t round_samples = 0;
bool rise_detected = false;
```

---

### 3.3 Delay Threshold Calculation & Exit Logic

In [`src/congestion_control/hybrid_slow_start.cpp` (lines 20–50)](file:///home/adinesh-reddy/MP_QUIC/src/congestion_control/hybrid_slow_start.cpp#L20-L50):

```cpp
bool HybridSlowStart::should_exit_slow_start(uint64_t latest_rtt_us, uint64_t min_rtt_us, uint64_t congestion_window)
{
    if (!round_open) {
        start_receive_round(newest_sent); // round ends when newest_sent is acked
    }
    if (rise_detected) return true;

    // Track minimum RTT in first 8 samples of the round
    round_samples++;
    if (round_samples <= SAMPLES_PER_ROUND) {
        if (round_min_rtt == 0 || round_min_rtt > latest_rtt_us) {
            round_min_rtt = latest_rtt_us;
        }
    }

    // Evaluate on the 8th sample:
    if (round_samples == SAMPLES_PER_ROUND) {
        // Threshold = min_rtt / 8, clamped to [4ms, 16ms]
        uint64_t threshold = clamp(min_rtt_us >> RISE_SHIFT, MIN_RISE_US, MAX_RISE_US);

        if (round_min_rtt > min_rtt_us + threshold) {
            rise_detected = true; // Queue buildup detected!
        }
    }

    // Exit only if window is large enough (>= 16 packets)
    return congestion_window >= MIN_EXIT_WINDOW_PACKETS && rise_detected;
}
```

1. **Round Tracking:** A round spans roughly one RTT. It begins with `start_receive_round(newest_sent)` and ends when an ACK arrives for a packet sent after `round_last_packet`.
2. **Sampling:** It monitors the first `SAMPLES_PER_ROUND = 8` RTT measurements in each round to find the round's minimum RTT (`round_min_rtt`).
3. **Threshold Formula:**
   $$\text{threshold} = \min\left(\max\left(\frac{\text{min\_rtt}}{8},\, 4000\,\mu\text{s}\right),\, 16000\,\mu\text{s}\right)$$
4. **Trigger Condition:** If $\text{round\_min\_rtt} > \text{min\_rtt} + \text{threshold}$, router buffers are filling. HyStart signals `true`.
5. **Guard:** Exits only when $\text{window} \ge 16$ packets, ensuring the connection does not prematurely throttle in small windows.

---

## 4. `include/congestion_control/olia_sender.hpp` & `src/congestion_control/olia_sender.cpp`

### 4.1 Internal State & Sizing Parameters

In [`include/congestion_control/olia_sender.hpp`](file:///home/adinesh-reddy/MP_QUIC/include/congestion_control/olia_sender.hpp#L12-L23, #L82-L104):
* `MAX_BURST_BYTES = 3 * 1460 = 4380 B`: Burst allowance for window-limiting checks.
* `SCALE_SHIFT = 10` ($2^{10} = 1024$): Scaling factor for fixed-point integer math.
* `LOSS_DIVISOR = 2`: Halves $CWND$ on packet loss (like TCP Reno).
* `window`: Current congestion window in packets.
* `ssthresh`: Slow start threshold in packets.
* `recovery_end`: Packet number marking the end of a loss recovery window.
* `window_credit`: Accumulator tracking fractional packet increments ($\pm 1024$ represents 1 full packet).
* `epsilon_num` & `epsilon_den`: Numerator and denominator of the capacity-shifting parameter $\epsilon$.

---

### 4.2 The Coupled OLIA Equation Explained

In Congestion Avoidance ($\text{cwnd} \ge \text{ssthresh}$), for every valid ACK received on path $r$:

$$\Delta w_r = \underbrace{\frac{\frac{w_r}{\text{rtt}_r^2}}{\left(\sum_{p \in P} \frac{w_p}{\text{rtt}_p}\right)^2}}_{\text{Term 1: TCP-Friendliness}} + \underbrace{\frac{\epsilon_r}{w_r}}_{\text{Term 2: Capacity Shifting}}$$

#### Mathematical Meaning:
* **Term 1 (Global Fairness):** The denominator sums the aggregate rate across all active paths $P$:
  $$\text{AggRate} = \sum_{p \in P} \frac{w_p}{\text{rtt}_p}$$
  Squaring this term ensures that the total rate increase across all paths combined does not exceed the rate increase of a single standard TCP flow on the same bottleneck link.
* **Term 2 (Capacity Shifting):** Shifting parameter $\epsilon_r$ moves bandwidth from congested, large-window paths to the best paths.

---

### 4.3 Path Quality & Epsilon ($\epsilon$) Capacity Shifting

In [`src/congestion_control/olia_sender.cpp` (lines 13–33, 204–253)](file:///home/adinesh-reddy/MP_QUIC/src/congestion_control/olia_sender.cpp#L13-L33):

#### Quality Metric:
$$\text{quality}_p = \frac{\text{bytes acknowledged between losses}_p}{\text{rtt}_p^2}$$
* `bytes_between_losses` is calculated as $\max(\text{loss}_3 - \text{loss}_2, \text{loss}_2 - \text{loss}_1)$ across the last two loss events.
* A high quality means low packet loss and low latency.

#### Epsilon Rules:
Let $M$ be the set of paths with the maximum window ($w_p = \max_q w_q$).
Let $B$ be the set of paths with the highest quality.

1. **If $r \in B \setminus M$ (Best quality path with smaller window):**
   $$\epsilon_r = +\frac{1}{|P| \cdot |B \setminus M|}$$
   This path gets a **positive boost**, growing faster than standard TCP to capture available bandwidth.
2. **If $r \in M$ and $B \setminus M \neq \emptyset$ (Largest window path, but inferior quality):**
   $$\epsilon_r = -\frac{1}{|P| \cdot |M|}$$
   This path gets a **negative penalty**, slowing its growth or gently deflating its window.
3. **Otherwise:** $\epsilon_r = 0$.

---

### 4.4 128-Bit Fixed-Point Arithmetic & Fractional Accumulation

In [`src/congestion_control/olia_sender.cpp` (lines 259–309)](file:///home/adinesh-reddy/MP_QUIC/src/congestion_control/olia_sender.cpp#L259-L309):

```cpp
typedef unsigned __int128 uint128;
```

#### Why 128-bit integers are required:
When scaling by $S = 2^{10} = 1024$, multiplying $w_r S \times \text{own\_rtt}$ and squaring the rate produces numbers exceeding $2^{64}-1$:
$$\text{rate} = \left(1 + \sum_{p} w_p S \cdot \frac{\text{own\_rtt}}{\text{rtt}_p}\right)^2$$
Using `unsigned __int128` prevents integer overflow without resorting to slow floating-point operations.

#### Fractional Accumulation:
The fractional change per ACK is scaled and added to `window_credit`:
```cpp
const int64_t one_packet = (1 << SCALE_SHIFT) - 1; // 1023

if (window_credit >= one_packet) {
    new_window++;       // Gained 1 full packet
    window_credit = 0;
} else if (window_credit <= -one_packet) {
    new_window = max<uint64_t>(1, window - 1); // Reduced 1 packet
    window_credit = 0;
}
```

---

### 4.5 Window Limiting (`window_is_limiting`)

In [`src/congestion_control/olia_sender.cpp` (lines 151–160)](file:///home/adinesh-reddy/MP_QUIC/src/congestion_control/olia_sender.cpp#L151-L160):
A congestion window should only grow if the application is actually utilizing it. If an app only sends 1 packet every second, growing $CWND$ to 2000 would cause catastrophic bursts when the app suddenly starts streaming.

```cpp
bool OliaSender::window_is_limiting(uint64_t bytes_in_flight) const
{
    uint64_t window_bytes = get_congestion_window();
    if (bytes_in_flight >= window_bytes) return true;

    uint64_t room_left = window_bytes - bytes_in_flight;
    bool slow_start_limited = in_slow_start() && bytes_in_flight > window_bytes / 2;
    return slow_start_limited || room_left <= MAX_BURST_BYTES;
}
```
$CWND$ grows **only if**:
* `bytes_in_flight >= window_bytes`, OR
* Remaining space $\le \text{MAX\_BURST\_BYTES} = 4380\text{ bytes}$ (3 packets), OR
* In Slow Start and $\text{bytes\_in\_flight} > \text{window\_bytes} / 2$.

---

### 4.6 Loss Response & Single-Path Recovery Isolation

In [`src/congestion_control/olia_sender.cpp` (lines 311–329)](file:///home/adinesh-reddy/MP_QUIC/src/congestion_control/olia_sender.cpp#L311-L329):

```cpp
void OliaSender::on_packet_lost(uint64_t packet_number, ...)
{
    // Ignore multiple losses belonging to the same burst event
    if (packet_number <= recovery_end) return;

    acked_at_older_loss = acked_at_last_loss;
    acked_at_last_loss = acked_total;

    // Halve the window on this path only:
    window = window / LOSS_DIVISOR;
    if (window < min_window) window = min_window;
    ssthresh = window;
    recovery_end = newest_sent;
}
```

* **Multiplicative Decrease:** $w \leftarrow \max(2, \lfloor w / 2 \rfloor)$.
* **Single Loss Event Guard:** Multiple lost packets from the same flight do not trigger repeated cuts. `recovery_end = newest_sent` shields the window until packets transmitted *after* the loss cut are acknowledged.
* **Path Isolation:** The loss affects **only the path where the packet was dropped**. If Wi-Fi loses a packet, Wi-Fi's window cuts from 32 to 16, while Cellular continues unaffected at 20.

---

### 4.7 Retransmission Timeout (RTO) Reset

In [`src/congestion_control/olia_sender.cpp` (lines 331–343)](file:///home/adinesh-reddy/MP_QUIC/src/congestion_control/olia_sender.cpp#L331-L343):

```cpp
void OliaSender::on_retransmission_timeout(bool packets_retransmitted)
{
    recovery_end = 0;
    if (!packets_retransmitted) return;

    hybrid_slow_start.restart();
    reset_coupling();
    ssthresh = window / 2;
    window = min_window; // Reset to 2 packets!
}
```
* Drops window back to the minimum floor (**`2 packets`**).
* Halves `ssthresh`.
* Resets `HybridSlowStart` and clears OLIA loss-interval coupling history.

---

## 5. Unit Test Verification (`tests/unit/test_congestion.cpp`)

File: [`tests/unit/test_congestion.cpp`](file:///home/adinesh-reddy/MP_QUIC/tests/unit/test_congestion.cpp)

This suite runs 8 targeted unit tests verifying every edge case:

1. **`test_hybrid_slow_start`:**
   - Verifies flat RTT (50 ms vs 50 ms) keeps Slow Start active.
   - Verifies that a rising RTT (60 ms vs 50 ms min) exceeds the threshold ($\text{min} + 6.25\text{ ms}$) and exits Slow Start on the 8th sample.
   - Verifies that HyStart will not exit while $CWND < 16$ packets.
   - Asserts clamping of rise thresholds to $[4\text{ ms}, 16\text{ ms}]$.
   - Verifies `restart()` resets flags after RTO.
2. **`test_slow_start`:**
   - Asserts $+1$ packet per ACK when window is full ($32 \to 42$ after 10 ACKs).
   - Verifies zero growth when window is not limiting.
   - Verifies window growth stops at `max_window = 40`.
3. **`test_loss_and_recovery`:**
   - Verifies loss halves window ($35 \to 17$).
   - Verifies that subsequent losses in the same flight (`packet_number <= recovery_end`) do not cut again.
   - Asserts no window growth while `in_recovery() == true`.
   - Verifies that an ACK for packet 41 (sent after cut) exits recovery.
   - Asserts floor clamping at `MIN_WINDOW_PACKETS = 2`.
4. **`test_retransmission_timeout`:**
   - Asserts RTO drops window to 2 packets and sets $\text{ssthresh} = \text{window} / 2$.
   - Verifies slow-start ramp back up to `ssthresh`.
5. **`test_single_path_congestion_avoidance`:**
   - In single-path mode, $\epsilon = 0$.
   - Asserts standard TCP Reno growth ($\approx +1$ packet per RTT).
6. **`test_two_equal_paths`:**
   - Verifies coupled denominator: with two equal paths, window growth per path is $4\times$ slower, keeping total multipath bandwidth identical to single-path TCP.
7. **`test_traffic_shifting_with_epsilon`:**
   - Pairs a high-loss path ($M$, max window) with a low-loss path ($B$, best quality).
   - Asserts best path gets $\epsilon = +1/2$ and max-window path gets $\epsilon = -1/2$.
   - Verifies traffic shifting: the best path increases its window faster, while the max-window path slows down.
8. **`test_fixed_window_congestion`:**
   - Asserts that `FixedWindowCongestion` never alters its window upon ACKs, losses, or RTO.

---

## 6. Academic References & Literature Foundations

The design and mathematical proofs behind the coupled OLIA algorithm implemented in this codebase originate from the following foundational literature:

1. **Original Proposal (ACM CoNEXT 2012):**
   * *Title:* MPTCP is not Pareto-Optimal: Performance Issues and a Possible Solution
   * *Authors:* Ramin Khalili, Nicolas Gast, Miroslav Popovic, Upamanyu Le Boudec, and Jean-Yves Le Boudec (EPFL)
   * *Conference:* ACM Conference on Emerging Networking Experiments and Technologies (CoNEXT 2012), Nice, France
   * *DOI:* [10.1145/2413176.2413178](https://doi.org/10.1145/2413176.2413178)
   * *Significance:* First demonstrated that earlier coupled algorithms (such as LIA) can degrade total throughput below single-path levels. Introduced the capacity shifting parameter $\epsilon$ to guarantee Pareto-optimality and responsiveness.

2. **Full Mathematical Model & Proofs (IEEE/ACM ToN 2013):**
   * *Title:* Opportunistic Linked-Increases Congestion Control Algorithm for MPTCP
   * *Authors:* Ramin Khalili, Nicolas Gast, Miroslav Popovic, and Jean-Yves Le Boudec
   * *Journal:* IEEE/ACM Transactions on Networking (ToN), Volume 21, Issue 5, pp. 1653–1665 (October 2013)
   * *DOI:* [10.1109/TNET.2013.2274462](https://doi.org/10.1109/TNET.2013.2274462)
   * *Significance:* Provides formal proofs for fluid model stability, non-flapping equilibria, and optimal traffic shifting under heterogeneous delay conditions.

3. **IETF Specification Draft:**
   * *Title:* Opportunistic Linked-Increases Congestion Control Algorithm for MPTCP
   * *Document ID:* `draft-khalili-mptcp-congestion-control-05`
   * *Authors:* R. Khalili, N. Gast, M. Popovic, J.-Y. Le Boudec
   * *Organization:* Internet Engineering Task Force (IETF) MPTCP Working Group
   * *Link:* [datatracker.ietf.org/doc/draft-khalili-mptcp-congestion-control/](https://datatracker.ietf.org/doc/draft-khalili-mptcp-congestion-control/)
   * *Significance:* Defines the canonical discrete equations for window increments and the rules for sets $B$ (best quality) and $M$ (max window).

4. **Multipath QUIC Translation (ACM CoNEXT 2017):**
   * *Title:* Multipath QUIC: Design and Evaluation
   * *Authors:* Quentin De Coninck and Olivier Bonaventure (UCLouvain)
   * *Conference:* ACM CoNEXT 2017
   * *DOI:* [10.1145/3143361.3143370](https://doi.org/10.1145/3143361.3143370)
   * *Significance:* Translated multipath congestion control from kernel MPTCP into user-space QUIC, inspiring the packet-number decoupled and per-path window architecture used in this implementation.

