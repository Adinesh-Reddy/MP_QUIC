# Step 1 — Ubuntu Desktop host (KVM/QEMU, virt-manager)

Everything in this file is typed on the **host** (your Ubuntu Desktop), not in a VM.

## 1.1 The four bridges exist, are up, and carry no host address

```bash
ip -br link show type bridge          # mpqa1 mpqa2 mpqb1 mpqb2 must be listed, state UP
ip -br -4 addr show | grep -E "mpq"   # nothing (or no IPv4) on them: they are isolated
bridge link | grep -E "mpqa1|mpqa2|mpqb1|mpqb2"   # with the VMs running: one vnetX per VM NIC
```

Each bridge must have exactly two VM ports:

| Bridge | Ports |
|---|---|
| `mpqa1` | MPQ-Client and MPQ-Router-A |
| `mpqa2` | MPQ-Router-A and MPQ-Server |
| `mpqb1` | MPQ-Client and MPQ-Router-B |
| `mpqb2` | MPQ-Router-B and MPQ-Server |

None of them should list a physical network card.

If a bridge is missing, create it, isolated and persistent, with NetworkManager:

```bash
for br in mpqa1 mpqa2 mpqb1 mpqb2; do
  sudo nmcli connection add type bridge ifname $br con-name $br \
       ipv4.method disabled ipv6.method disabled bridge.stp no
  sudo nmcli connection up $br
done
```

In virt-manager each VM NIC is then set to *Network source: Bridge device*, with the device
name `mpqa1`, … and *Device model: virtio*.

## 1.2 Which NIC of each VM is on which bridge (write the MAC addresses down)

```bash
for vm in MPQ-Client MPQ-Server MPQ-Router-A MPQ-Router-B; do
  echo "== $vm"; virsh -c qemu:///system domiflist $vm
done
```

The output lists, for every NIC, its *Source* (the bridge) and its *MAC*. Inside each VM the
same MAC tells you the interface name:

- Ubuntu: `ip -br link`.
- VyOS: `show interfaces ethernet` (the `hw-id`, or `show interfaces ethernet eth0` for one NIC).

Expected:

| VM | Bridge → name inside the VM |
|---|---|
| MPQ-Client | `mpqa1` → path A interface (often `enp1s0`), `mpqb1` → path B interface (often `enp2s0`) |
| MPQ-Server | `mpqa2` → path A interface, `mpqb2` → path B interface |
| MPQ-Router-A | `mpqa1` → `eth0`, `mpqa2` → `eth1` |
| MPQ-Router-B | `mpqb1` → `eth0`, `mpqb2` → `eth1` |

If a router has them the other way round (`eth0` on `mpqa2`), swap `eth0` and `eth1` in
every command of that router's file.

## 1.3 The host must not filter the bridged traffic

If the `br_netfilter` kernel module is loaded (Docker loads it), frames that cross a Linux
bridge go through the host's iptables `FORWARD` chain. Docker sets that chain's policy to
`DROP`, which silently cuts the paths.

```bash
lsmod | grep br_netfilter
sysctl net.bridge.bridge-nf-call-iptables 2>/dev/null   # 1 = bridged frames are filtered
sudo iptables -S FORWARD | head -3                       # "-P FORWARD DROP" = they are dropped
```

If the module is loaded **and** the policy is `DROP`, let the four bridges through. These
rules are not persistent, so repeat them after a reboot of the host:

```bash
for br in mpqa1 mpqa2 mpqb1 mpqb2; do
  sudo iptables -I FORWARD -i $br -o $br -j ACCEPT
done
```

## 1.4 (Optional) Internet or file copy for the Ubuntu VMs

The client and server only have their two experiment NICs. To copy the project into them,
or to install a missing package, you can temporarily add a third NIC on libvirt's
**default NAT network** (`192.168.122.0/24`). That range does not clash with the experiment's
`10.0.x.0/24` networks:

1. In virt-manager: *Add Hardware → Network → Network source: Virtual network 'default'*.
2. Inside the VM, the new interface gets an address by DHCP. Then from the host:
   `scp -r ~/MP_QUIC user@<that address>:`
3. You may keep or remove that NIC afterwards. Adding it does not rename the existing
   interfaces, but check again with `ip -br link`.
