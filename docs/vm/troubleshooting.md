# Troubleshooting

Start with the path checks on the client ([client.md](client.md) 5.6) and on the server
([server.md](server.md) 4.5): `ip -4 route get …` says which router a path goes through, and
`ping -I …` says whether it answers.

## Network

| Symptom | Likely cause | What to do |
|---|---|---|
| `ip -br -4 addr` does not show 10.0.0.1 / 10.0.1.1 (client) or 10.0.2.2 / 10.0.3.2 (server) | The netplan file is missing or has wrong interface names | Check `cat /etc/netplan/60-mpquic.yaml`; write it again ([client.md](client.md) 5.3 / [server.md](server.md) 4.3) |
| `netplan apply`: error about an unknown interface | Wrong name in `IF_A`/`IF_B` | `ip -br link`; match MACs with `virsh domiflist` on the host |
| `ip route get`: path goes via the wrong gateway or interface | The two interface names are swapped | Swap `IF_A` and `IF_B` and write the netplan file again; IF_A must be the NIC on `mpqa1`/`mpqa2` |
| Path A route is right but the ping gets no answer | Router A not configured, or the server has no addresses | On router A: `show interfaces` (eth0 10.0.0.254, eth1 10.0.2.254); on the server: `ip -br -4 addr` |
| The router can ping both ends, but client and server cannot reach each other | Forwarding off on the router, or a VyOS firewall | On the router: `sudo sysctl net.ipv4.ip_forward` (must be 1); `show configuration commands \| grep firewall` |
| Nothing crosses a bridge at all (even client → router fails) | The host filters bridged frames (Docker + br_netfilter), or the NIC is on the wrong bridge | [host-kvm.md](host-kvm.md) 1.3; `bridge link` on the host |
| Router's eth0 is on `mpqa2` instead of `mpqa1` | NIC order differs | Swap `eth0` and `eth1` in every command of that router's file |
| Netplan prints warnings about file permissions | Another file in `/etc/netplan` is too open | `sudo chmod 600 /etc/netplan/*.yaml` |
| Router addresses gone after a reboot | `save` missing, or VyOS running from the live ISO | Repeat 2.2/3.2 with `commit` and `save`; install VyOS with `install image` |
| RTT not ~20 / ~50 ms | Shaping missing (lost at a reboot) or only on one interface | On the router: `tc qdisc show dev eth0` and `eth1`; type 2.4 / 3.4 again |
| VyOS: `tc` command not found / permission denied | Typed in configuration mode, or without `sudo` | `exit` configuration mode; use `sudo tc …` |
| VyOS: shaping disappears after a `commit` | A `qos`/`traffic-policy` configuration manages the same interfaces | Use either the tc commands or the VyOS `qos` policy, not both (remove lines in 2.6 of [router-a-vyos.md](router-a-vyos.md)) |

## Programs and benchmarks

| Symptom | Likely cause | What to do |
|---|---|---|
| `mpq_client` waits, then `error="timed out after … s"` | `mpq_server` not running, a firewall, or a path cut | Start it on the server ([server.md](server.md) 4.7); `sudo ufw status`; ping both paths |
| `RESULT status=failed … error="server error: file not found"` | The test files are not in `evaluation/files` on the server | [server.md](server.md) 4.6; `mpq_server` must run with `--dir evaluation/files` |
| `RESULT … paths=1` in a two-path run | Path B blocked, or the server does not listen on 10.0.3.2 | Ping path B; `mpq_server` needs both `--listen` options |
| Everything fails after a handover test | Path A still cut by the tc filter on the client | `sudo tc qdisc del dev $IF_A clsact` on the client; check the router shaping |
| MPTCP uses only one link | MPTCP endpoints or limits | Server: `ip mptcp endpoint show` (two `signal`); client: `ip mptcp limits` ([client.md](client.md) 5.5); `nstat -az \| grep MPJoin` |
| `mptcpize: command not found` | Package missing | `sudo apt install mptcpize` (needs Internet: [host-kvm.md](host-kvm.md) 1.4) |
| `iperf3: unable to connect` | The iperf3 servers are not running | Terminals 2 and 3 of [server.md](server.md) 4.7 (ports 5201 and 5202) |
| TCP/MPTCP well below the link rate | Offloads on; small queue | Router: the `ethtool` lines of 2.4/3.4; client/server: [client.md](client.md) 5.4 / [server.md](server.md) 4.4 |
| `mpq_server`: "[ERROR] Cannot bind … (Address already in use)" | An old server still runs | `pkill mpq_server` |
| `mpq_client`: "[ERROR] Cannot bind … (Cannot assign requested address)" | The `--local` address is not set on this VM | `ip -br -4 addr`; [client.md](client.md) 5.3 |

## Looking inside

- **Router queue counters:** `tc -s qdisc show dev eth1` on a router. `dropped` grows when the
  queue is full, which is congestion control at work.
- **Packets on a path:** `sudo tcpdump -ni eth0 udp port 4433` on router A.
- **Path state in MP-QUIC:** the `PATH …` lines printed at the end of each connection
  (`potentially_failed=1` on a cut path), and `--stats FILE` for the window and RTT every 100 ms.
