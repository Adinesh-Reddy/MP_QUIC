# Step 4 — MPQ-Server (Ubuntu Server 24.04)

Type everything in the server VM as your normal user, in the project folder `~/MP_QUIC`.
Each block can be copied as it is.

## 4.1 Packages, build and tests (once)

Copy the project into the VM if it is not there yet ([host-kvm.md](host-kvm.md), 1.4), then:

```bash
cd ~/MP_QUIC
dpkg -s build-essential iproute2 ethtool iperf3 mptcpize python3 >/dev/null && echo "packages OK"
make -j2
make test                     # about 8 s, must end with: === ALL TEST PROGRAMS PASSED ===
sysctl net.mptcp.enabled      # must print 1 (Multipath TCP is needed for the comparison)
```

If `dpkg -s` complains, install what is missing with `sudo apt install -y <package>` (needs
Internet: 1.4 of [host-kvm.md](host-kvm.md)).

Larger socket buffers, so the programs get the 4 MB they ask for (kept after a reboot):

```bash
printf 'net.core.rmem_max=4194304\nnet.core.wmem_max=4194304\n' | sudo tee /etc/sysctl.d/90-mpquic.conf
sudo sysctl --system >/dev/null
```

## 4.2 Interface names

```bash
ip -br link
```

Match the MAC addresses with `virsh domiflist MPQ-Server` on the host:

- the NIC on **`mpqa2`** is the **path A** interface (often `enp1s0`);
- the NIC on **`mpqb2`** is the **path B** interface (often `enp2s0`).

Put the two names in variables. **Type this line again in every new terminal**, because the
next blocks use `$IF_A` and `$IF_B`:

```bash
IF_A=enp1s0   # NIC on mpqa2
IF_B=enp2s0   # NIC on mpqb2
```

## 4.3 Addresses and routes (kept after a reboot)

Write a netplan file with the two addresses and the routes through the two routers, then
apply it:

```bash
sudo tee /etc/netplan/60-mpquic.yaml >/dev/null <<EOF
# The two paths of the MP-QUIC test bed. Remove this file and run "sudo netplan apply" to undo.
network:
  version: 2
  ethernets:
    $IF_A:
      dhcp4: false
      dhcp6: false
      addresses: [10.0.2.2/24]
      routes:
        - to: 10.0.0.0/24
          via: 10.0.2.254
    $IF_B:
      dhcp4: false
      dhcp6: false
      addresses: [10.0.3.2/24]
      routes:
        - to: 10.0.1.0/24
          via: 10.0.3.254
EOF
sudo chmod 600 /etc/netplan/60-mpquic.yaml
sudo netplan apply
```

Check:

```bash
ip -br -4 addr show dev $IF_A    # 10.0.2.2/24
ip -br -4 addr show dev $IF_B    # 10.0.3.2/24
ip route | grep via              # 10.0.0.0/24 via 10.0.2.254 and 10.0.1.0/24 via 10.0.3.254
```

<details>
<summary>Only for a quick test: the same without netplan (lost at a reboot)</summary>

```bash
sudo ip addr replace 10.0.2.2/24 dev $IF_A ; sudo ip link set $IF_A up
sudo ip addr replace 10.0.3.2/24 dev $IF_B ; sudo ip link set $IF_B up
sudo ip route replace 10.0.0.0/24 via 10.0.2.254 dev $IF_A
sudo ip route replace 10.0.1.0/24 via 10.0.3.254 dev $IF_B
```
</details>

## 4.4 Offloads off (after every reboot)

Without this the virtual cards hand very large packets to the kernel, and the TCP and
MPTCP numbers become wrong. This setting is lost at a reboot, so type it again after one.

```bash
sudo ethtool -K $IF_A tso off gso off gro off
sudo ethtool -K $IF_B tso off gso off gro off
```

## 4.5 Check both paths (after the two routers and the client are configured)

Path A must go through router A (10.0.2.254) and path B through router B (10.0.3.254):

```bash
ip -4 route get 10.0.0.1 from 10.0.2.2    # must say: via 10.0.2.254 dev <IF_A>
ip -4 route get 10.0.1.1 from 10.0.3.2    # must say: via 10.0.3.254 dev <IF_B>
ping -c 5 -I 10.0.2.2 10.0.0.1            # path A: about 20 ms once router A is shaped
ping -c 5 -I 10.0.3.2 10.0.1.1            # path B: about 50 ms once router B is shaped
```

Without shaping the ping times are below 1 ms. If a ping gets no answer, see
[troubleshooting.md](troubleshooting.md).

## 4.6 Test files (once)

A 20 MB file (the long transfer of the paper) and a 256 kB file (the short transfer), filled
with random bytes, and their SHA-256 to compare with what the client receives:

```bash
mkdir -p ~/MP_QUIC/evaluation/files && cd ~/MP_QUIC/evaluation/files
head -c 20971520 /dev/urandom > test20M.bin
head -c 262144   /dev/urandom > test256K.bin
sha256sum test20M.bin test256K.bin      # write these down: the client must get the same
cd ~/MP_QUIC
```

## 4.7 Start the servers

Open **three terminals** on the server (or use `tmux`). Leave them running during the tests.

**Terminal 1 — MP-QUIC server** on both addresses:

```bash
cd ~/MP_QUIC
./bin/mpq_server --listen 10.0.2.2:4433 --listen 10.0.3.2:4433 --dir evaluation/files
```

Each finished connection prints a `CONNECTION …` line and one `PATH …` line per path.

**Terminal 2 — iperf3 for TCP and UDP** (port 5201):

```bash
iperf3 -s -p 5201
```

**Terminal 3 — iperf3 for Multipath TCP** (port 5202). First the server announces both of
its addresses, so the client can open a second subflow over path B:

```bash
IF_A=enp1s0 ; IF_B=enp2s0                          # your names from 4.2
sudo sysctl -w net.mptcp.enabled=1
sudo ip mptcp limits set subflows 2 add_addr_accepted 2
sudo ip mptcp endpoint flush
sudo ip mptcp endpoint add 10.0.2.2 dev $IF_A signal
sudo ip mptcp endpoint add 10.0.3.2 dev $IF_B signal
ip mptcp endpoint show                             # two lines with "signal"
mptcpize run iperf3 -s -p 5202
```

A fresh Ubuntu Server has no active firewall. If `sudo ufw status` says *active*:

```bash
sudo ufw allow 4433/udp ; sudo ufw allow 5201 ; sudo ufw allow 5201/udp ; sudo ufw allow 5202
```

## 4.8 When finished

```bash
# Ctrl-C in the three terminals, then:
sudo ip mptcp endpoint flush
```

To remove the addresses for good:

```bash
sudo rm /etc/netplan/60-mpquic.yaml && sudo netplan apply
```
