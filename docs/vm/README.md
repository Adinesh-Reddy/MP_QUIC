# MP-QUIC on four KVM virtual machines with VyOS routers

These files say what to type on each machine, in order, to run the MP-QUIC evaluation on this
test bed:

- **Host:** Ubuntu Desktop running KVM/QEMU and virt-manager.
- **`MPQ-Client`, `MPQ-Server`:** Ubuntu Server 24.04, 3 GB RAM, 25 GB disk.
- **`MPQ-Router-A`, `MPQ-Router-B`:** VyOS.
- **Networks:** four isolated Linux bridges, `mpqa1`, `mpqa2`, `mpqb1` and `mpqb2`.

```
                 mpqa1 10.0.0.0/24                      mpqa2 10.0.2.0/24
           .1 ----------------------- .254 [MPQ-Router-A] .254 ----------------------- .2
MPQ-Client                                 eth0       eth1                                 MPQ-Server
           .1 ----------------------- .254 [MPQ-Router-B] .254 ----------------------- .2
                 mpqb1 10.0.1.0/24                      mpqb2 10.0.3.0/24
```

Path A is client → Router A → server, path B is client → Router B → server. The two paths
share no network, so they stay independent.

## Addresses

| VM | NIC 1 | NIC 2 | Routes |
|---|---|---|---|
| MPQ-Client | `mpqa1`: 10.0.0.1/24 (path A) | `mpqb1`: 10.0.1.1/24 (path B) | 10.0.2.0/24 via 10.0.0.254; 10.0.3.0/24 via 10.0.1.254 |
| MPQ-Router-A (VyOS) | `mpqa1`: eth0 10.0.0.254/24 | `mpqa2`: eth1 10.0.2.254/24 | none (both networks directly connected) |
| MPQ-Router-B (VyOS) | `mpqb1`: eth0 10.0.1.254/24 | `mpqb2`: eth1 10.0.3.254/24 | none |
| MPQ-Server | `mpqa2`: 10.0.2.2/24 (path A) | `mpqb2`: 10.0.3.2/24 (path B) | 10.0.0.0/24 via 10.0.2.254; 10.0.1.0/24 via 10.0.3.254 |

## Order

| Step | Where | File |
|---|---|---|
| 1 | Ubuntu Desktop host: check the bridges and find which NIC is on which bridge | [host-kvm.md](host-kvm.md) |
| 2 | MPQ-Router-A: addresses, then link shaping | [router-a-vyos.md](router-a-vyos.md) |
| 3 | MPQ-Router-B: addresses, then link shaping | [router-b-vyos.md](router-b-vyos.md) |
| 4 | MPQ-Server: build, addresses, check the paths, start the servers | [server.md](server.md) |
| 5 | MPQ-Client: build, addresses, check the paths | [client.md](client.md) |
| 6 | MPQ-Client: run the comparisons (MP-QUIC, TCP, MPTCP, UDP) and read the results | [benchmarks.md](benchmarks.md) |
| — | When something does not work | [troubleshooting.md](troubleshooting.md) |

Steps 2–5 can be done in any order. Step 6 needs all four VMs ready and the server started.

## No scripts: every step is a command

Every file gives the commands to type on that machine, in order; there are no evaluation
scripts to run. The only script left in the project is
`tests/integration/test_file_transfer.sh`, which `make test` runs by itself.

| What | Where | Section |
|---|---|---|
| Addresses (netplan file), offloads off | client, server | [client.md](client.md) 5.3–5.4, [server.md](server.md) 4.3–4.4 |
| Addresses, shaping (`tc netem`) | routers | [router-a-vyos.md](router-a-vyos.md) 2.2–2.5, [router-b-vyos.md](router-b-vyos.md) 3.2–3.5 |
| Check that each path goes through its own router | client, server | [client.md](client.md) 5.6, [server.md](server.md) 4.5 |
| Test files, `mpq_server`, the two iperf3 servers | server | [server.md](server.md) 4.6–4.7 |
| MP-QUIC, TCP, UDP, Multipath TCP downloads | client | [benchmarks.md](benchmarks.md) 6.1–6.4 |
| Results table and EBen | by hand | [benchmarks.md](benchmarks.md) 6.5 |
| Handover (path A cut during the transfer) | client | [benchmarks.md](benchmarks.md) 6.6 |

## What was verified, and what was not

- **Tested** in a local imitation of this topology (four Linux network namespaces with the
  same addresses and `eth0`/`eth1` on the routers): the `tc netem` shaping commands, the
  address and route commands, the path checks, the netplan file (`netplan generate`) and all
  the MP-QUIC, iperf3 and handover commands.
- **Not tested here:** VyOS itself, KVM, virt-manager and the Linux bridges, because none is
  available on the development machine.
  - The VyOS configuration commands (`set interfaces …`, `set qos …`) follow the official
    VyOS documentation (docs.vyos.io, versions 1.3, 1.4 and rolling).
  - The shaping uses plain Linux `tc netem`, typed in the VyOS shell. VyOS is a Debian-based
    Linux, so this is the same command as on the tested Linux routers.
