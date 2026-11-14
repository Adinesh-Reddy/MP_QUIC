# Step 5 — MPQ-Client (Ubuntu Server 24.04)

Type everything in the client VM as your normal user, in `~/MP_QUIC`.

## 5.1 Packages, build and tests (once)

The same as 4.1 of [server.md](server.md):

```bash
cd ~/MP_QUIC
dpkg -s build-essential iproute2 ethtool iperf3 mptcpize python3 >/dev/null && echo "packages OK"
make -j2
make test                     # must end with: === ALL TEST PROGRAMS PASSED ===
sysctl net.mptcp.enabled      # must print 1
printf 'net.core.rmem_max=4194304\nnet.core.wmem_max=4194304\n' | sudo tee /etc/sysctl.d/90-mpquic.conf
sudo sysctl --system >/dev/null
```

## 5.2 Interface names

```bash
ip -br link
```

Match the MAC addresses with `virsh domiflist MPQ-Client` on the host:

- the NIC on **`mpqa1`** is the **path A** interface (often `enp1s0`);
- the NIC on **`mpqb1`** is the **path B** interface (often `enp2s0`).

**Type this in every new terminal** (the next blocks use `$IF_A` and `$IF_B`):

```bash
IF_A=enp1s0   # NIC on mpqa1
IF_B=enp2s0   # NIC on mpqb1
```

## 5.3 Addresses and routes (kept after a reboot)

```bash
sudo tee /etc/netplan/60-mpquic.yaml >/dev/null <<EOF
# The two paths of the MP-QUIC test bed. Remove this file and run "sudo netplan apply" to undo.
network:
  version: 2
  ethernets:
    $IF_A:
      dhcp4: false
      dhcp6: false
      addresses: [10.0.0.1/24]
      routes:
        - to: 10.0.2.0/24
          via: 10.0.0.254
    $IF_B:
      dhcp4: false
      dhcp6: false
      addresses: [10.0.1.1/24]
      routes:
        - to: 10.0.3.0/24
          via: 10.0.1.254
EOF
sudo chmod 600 /etc/netplan/60-mpquic.yaml
sudo netplan apply
```

Check:

```bash
ip -br -4 addr show dev $IF_A    # 10.0.0.1/24
ip -br -4 addr show dev $IF_B    # 10.0.1.1/24
ip route | grep via              # 10.0.2.0/24 via 10.0.0.254 and 10.0.3.0/24 via 10.0.1.254
```

<details>
<summary>Only for a quick test: the same without netplan (lost at a reboot)</summary>

```bash
sudo ip addr replace 10.0.0.1/24 dev $IF_A ; sudo ip link set $IF_A up
sudo ip addr replace 10.0.1.1/24 dev $IF_B ; sudo ip link set $IF_B up
sudo ip route replace 10.0.2.0/24 via 10.0.0.254 dev $IF_A
sudo ip route replace 10.0.3.0/24 via 10.0.1.254 dev $IF_B
```
</details>

## 5.4 Offloads off (after every reboot)

```bash
sudo ethtool -K $IF_A tso off gso off gro off
sudo ethtool -K $IF_B tso off gso off gro off
```

## 5.5 Multipath TCP limits (after every reboot)

The client may open a second subflow towards the address that the server announces:

```bash
sudo sysctl -w net.mptcp.enabled=1
sudo ip mptcp limits set subflows 2 add_addr_accepted 2
```

## 5.6 Check both paths

```bash
ip -4 route get 10.0.2.2 from 10.0.0.1    # must say: via 10.0.0.254 dev <IF_A>
ip -4 route get 10.0.3.2 from 10.0.1.1    # must say: via 10.0.1.254 dev <IF_B>
ping -c 5 -I 10.0.0.1 10.0.2.2            # path A: rtt avg about 20 ms
ping -c 5 -I 10.0.1.1 10.0.3.2            # path B: rtt avg about 50 ms
```

The ping times are about 20 and 50 ms only once both routers are shaped
([router-a-vyos.md](router-a-vyos.md) 2.4, [router-b-vyos.md](router-b-vyos.md) 3.4).

## 5.7 Run the comparisons

Start the servers first ([server.md](server.md), 4.7), then follow
[benchmarks.md](benchmarks.md).
