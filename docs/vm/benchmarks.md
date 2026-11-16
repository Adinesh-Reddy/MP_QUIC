# Step 6 — Comparing MP-QUIC with TCP, Multipath TCP and UDP

Type everything here on **MPQ-Client** in `~/MP_QUIC`. Before you start:

- both routers are shaped ([router-a-vyos.md](router-a-vyos.md) 2.4, [router-b-vyos.md](router-b-vyos.md) 3.4);
- the three servers run on MPQ-Server ([server.md](server.md) 4.7);
- the client checks of [client.md](client.md) 5.4–5.6 are done.

## How the comparison works

The comparison is a download: the server sends and the client receives. All protocols use
the same two shaped paths and the same amount of data (20 MB). The number compared is the
**goodput**: bytes received × 8 ÷ time, as measured by the receiver.

| Protocol | Tool | Path A only | Path B only | Both, A first | Both, B first |
|---|---|---|---|---|---|
| UDP | `iperf3 -u -b 20M` | ✓ | ✓ | – | – |
| TCP | `iperf3` | ✓ | ✓ | – | – |
| Multipath TCP | `mptcpize run iperf3` (Linux kernel MPTCP) | – | – | ✓ | ✓ |
| MP-QUIC | `mpq_client` | ✓ | ✓ | ✓ | ✓ |

- **UDP is the capacity line.** It has no congestion control and no retransmission, and it
  sends faster than the link, so its receiver goodput is what the path can carry. It is a
  reference, not a competitor.
- **TCP against single-path MP-QUIC:** is our protocol as efficient as TCP on one path?
- **Multipath TCP against MP-QUIC on two paths:** this is the paper's comparison. Each
  multipath test is run twice, starting on the fast path and starting on the slow path.
- **Repetitions:** run every test 3 times and keep the middle value (the median), as in the paper.
- Wait a second or two between two tests, so the queues of the routers are empty.

## 6.1 MP-QUIC (four tests)

The client prints a last line like
`RESULT status=ok file=test20M.bin bytes=20971520 time_ms=15020 goodput_kbps=11169 sha256=… paths=2`.
Write down **`goodput_kbps`** (kbit/s) and check `paths=` and `sha256=`.

```bash
# Both paths, the connection starts on path A (paths=2)
./bin/mpq_client --server 10.0.2.2:4433 --local 10.0.0.1 --local 10.0.1.1 --get test20M.bin --out /tmp/o.bin

# Both paths, the connection starts on path B (paths=2)
./bin/mpq_client --server 10.0.3.2:4433 --local 10.0.1.1 --local 10.0.0.1 --get test20M.bin --out /tmp/o.bin

# Path A only (paths=1)
./bin/mpq_client --server 10.0.2.2:4433 --local 10.0.0.1 --get test20M.bin --out /tmp/o.bin

# Path B only (paths=1)
./bin/mpq_client --server 10.0.3.2:4433 --local 10.0.1.1 --get test20M.bin --out /tmp/o.bin
```

- **File intact:** the `sha256=` of the `RESULT` line, and `sha256sum /tmp/o.bin`, must equal
  the value printed by `sha256sum test20M.bin` on the server (4.6).
- **Only the `RESULT` lines**, for three runs in a row:
  `for i in 1 2 3; do ./bin/mpq_client … --out /tmp/o.bin | grep ^RESULT; sleep 2; done`
- **Path state:** the `PATH …` lines before `RESULT` show, for each path, the packets sent,
  lost and retransmitted, and the RTT.
- **Window and RTT over time:** add `--stats /tmp/paths.csv` (one line per path every 100 ms).

## 6.2 TCP and UDP (one path at a time)

iperf3 prints a summary at the end with a `sender` and a `receiver` line. Write down the
**`Bitrate` of the `receiver` line**.

```bash
# TCP, path A, then path B
iperf3 -c 10.0.2.2 -B 10.0.0.1 -p 5201 -R -n 20M -i 0.1
iperf3 -c 10.0.3.2 -B 10.0.1.1 -p 5201 -R -n 20M -i 0.1

# UDP (capacity), path A, then path B
iperf3 -c 10.0.2.2 -B 10.0.0.1 -p 5201 -R -u -b 20M -t 10
iperf3 -c 10.0.3.2 -B 10.0.1.1 -p 5201 -R -u -b 20M -t 10
```

- **`-R`:** the server sends, the same direction as the MP-QUIC download.
- **`-B`:** chooses the local address, and so the path.
- **`-n 20M`:** the same 20 MB as the MP-QUIC file.
- **`-i 0.1`:** without it iperf3 rounds the test time up to a whole second.
- **UDP:** the receiver line also shows `Lost/Total`. Many lost packets are normal: the sender
  sends 20 Mbit/s into a slower link. The received bitrate is the capacity of the path.

## 6.3 Multipath TCP (both paths)

```bash
# First subflow on path A
mptcpize run iperf3 -c 10.0.2.2 -B 10.0.0.1 -p 5202 -R -n 20M -i 0.1
# First subflow on path B
mptcpize run iperf3 -c 10.0.3.2 -B 10.0.1.1 -p 5202 -R -n 20M -i 0.1
```

Write down the `Bitrate` of the `receiver` line. Check that both subflows were really used:

```bash
nstat -az | grep -i MPJoin      # MPTcpExtMPJoinSynAckRx must not be 0
```

## 6.4 Proof that both paths carry data

Read the bytes received on each client interface before and after a test (type the
`IF_A=… IF_B=…` line of [client.md](client.md) 5.2 first):

```bash
cat /sys/class/net/$IF_A/statistics/rx_bytes /sys/class/net/$IF_B/statistics/rx_bytes
# … run one test …
cat /sys/class/net/$IF_A/statistics/rx_bytes /sys/class/net/$IF_B/statistics/rx_bytes
```

Both numbers grow in a two-path test; only one grows in a one-path test. On a router,
`tc -s qdisc show dev eth0` shows the same thing (its `Sent … pkt` counter grows).

## 6.5 Results table and aggregation benefit (EBen)

Fill in the medians (kbit/s):

| Protocol | Path A only | Path B only | Both, A first | Both, B first |
|---|---|---|---|---|
| UDP (capacity) | | | – | – |
| TCP | | | – | – |
| Multipath TCP | – | – | | |
| MP-QUIC | | | | |

**EBen** (from the paper) tells how much of the second path a multipath protocol used. With
G_multi the two-path goodput, G_A and G_B the one-path goodputs of the same protocol family
(MP-QUIC with one-path MP-QUIC, Multipath TCP with TCP), and G_best the larger of G_A and G_B:

- if G_multi ≥ G_best: **EBen = (G_multi − G_best) / (G_A + G_B − G_best)**
- if G_multi < G_best: EBen = (G_multi − G_best) / G_best (negative: worse than one path)

1 means the sum of both paths was used; 0 means no better than the best single path.

Example: MP-QUIC 11,174 kbit/s on two paths, 7,521 on A, 3,757 on B:
(11174 − 7521) / (7521 + 3757 − 7521) = 3653 / 3757 = **0.97**.

## 6.6 Handover (the connection survives when path A breaks)

The client sends a request of 750 bytes every 400 ms and measures the time until the answer
is back (Section 4.3 and Figure 11 of the paper). After about 3 seconds path A is cut on the
client: every packet in and out of `IF_A` is dropped.

**Terminal 1** on the client:

```bash
./bin/mpq_reqres --server 10.0.2.2:4433 --local 10.0.0.1 --local 10.0.1.1 --out /tmp/handover.csv
```

**Terminal 2** on the client, about 3 seconds after starting terminal 1 (type the
`IF_A=…` line of 5.2 first):

```bash
sudo tc qdisc add dev $IF_A clsact
sudo tc filter add dev $IF_A egress  matchall action drop
sudo tc filter add dev $IF_A ingress matchall action drop
```

What to see in terminal 1:

- a line `[PATH] Path 0 may be broken (no reply before RTO) …` less than a second after the cut;
- the `REQUEST id=… delay_ms=…` lines go on: the delay goes from about 20 ms to about 50 ms
  (now over path B), with one larger delay at the cut;
- at the end `RESULT status=ok requests=35 answered=35 …`.

**Repair path A** when it is over (also if you stopped the test with Ctrl-C):

```bash
sudo tc qdisc del dev $IF_A clsact
```

The delays of every request are in `/tmp/handover.csv` (`request,sent_ms,delay_ms`;
`-1` = no answer): `column -s, -t < /tmp/handover.csv`.

**For contrast, one path only:** run terminal 1 with `--local 10.0.0.1` alone and cut path A
in the same way. The requests after the cut get no answer and the result is
`status=failed`. Repair path A afterwards.

Instead of the tc filter you can also cut path A by unplugging it in virt-manager (NIC on
`mpqa1` → *Link state: down*), or with `sudo tc qdisc replace dev eth0 root netem loss 100%`
on router A (type 2.4 again afterwards to repair it).

## 6.7 Other network settings and the short transfer

For each setting, shape **both routers** with the lines of 2.5 / 3.5, check the RTTs again
(5.6), and repeat 6.1–6.6.

| Label | Path A | Path B | Loss |
|---|---|---|---|
| `default` | 8 Mbit/s, 10 ms | 4 Mbit/s, 25 ms | 0 |
| `similar` | 10 Mbit/s, 10 ms | 10 Mbit/s, 10 ms | 0 |
| `lossy` | 8 Mbit/s, 10 ms | 4 Mbit/s, 25 ms | 1 % |
| `highbdp` | 20 Mbit/s, 100 ms | 5 Mbit/s, 150 ms | 0 |
| `handover-paper` | 8 Mbit/s, 7 ms | 4 Mbit/s, 12 ms | 0 (paper: RTTs ~15 and ~25 ms) |

**Short transfer (256 kB):** in 6.1 use `--get test256K.bin`, and in 6.2/6.3 use `-n 256K`
instead of `-n 20M`.

## 6.8 What to expect

These figures come from a local imitation of this topology with network namespaces, one run
each, not from the VMs. Treat them as a guide.

| Protocol | Path A only | Path B only | Both, A first | Both, B first |
|---|---|---|---|---|
| UDP (capacity) | 7.8 Mbit/s | 3.9 | – | – |
| TCP | 7.6 | 3.8 | – | – |
| Multipath TCP | – | – | 11.0 | 10.9 |
| MP-QUIC | 7.5 | 3.8 | 11.2 | 11.1 |

- **EBen:** MP-QUIC about 0.96–0.97, MPTCP about 0.86–0.89.
- **Handover with two paths:** every request is answered; the delay goes from about 22 ms to
  about 54 ms after the cut, with one request of about 350 ms at the cut.

## Differences from the paper (state them with the results)

- The kernel's Multipath TCP runs an independent congestion control on each subflow; the
  paper's used OLIA.
- Single-path MP-QUIC uses OLIA, which on one path behaves like Reno; the paper's QUIC used
  CUBIC.
- UDP is not in the paper. It is only the capacity line.
