# Step 3 — MPQ-Router-B (VyOS): path B

The same procedure as [router-a-vyos.md](router-a-vyos.md), with router B's addresses and
path B's values. Router B joins `mpqb1` (client side, `eth0`) and `mpqb2` (server side,
`eth1`). Log in on the VM console as `vyos`.

## 3.1 Check the interface names

```
show interfaces ethernet
```

Compare with `virsh domiflist MPQ-Router-B` on the host. Expected: `eth0` = `mpqb1`,
`eth1` = `mpqb2`. If they are the other way round, swap `eth0` and `eth1` in every command.

## 3.2 Addresses (kept after a reboot; once)

```
configure
set system host-name 'MPQ-Router-B'
delete interfaces ethernet eth0 address
delete interfaces ethernet eth1 address
set interfaces ethernet eth0 address '10.0.1.254/24'
set interfaces ethernet eth0 description 'mpqb1 client side path B'
set interfaces ethernet eth1 address '10.0.3.254/24'
set interfaces ethernet eth1 description 'mpqb2 server side path B'
commit
save
exit
```

## 3.3 Check forwarding and firewall

```
show interfaces
sudo sysctl net.ipv4.ip_forward
show configuration commands | grep -E "firewall|qos|traffic-policy"
```

`ip_forward` must print 1, and the grep should print nothing (see 2.3 in
[router-a-vyos.md](router-a-vyos.md)).

## 3.4 Link shaping (path B = 4 Mbit/s, 25 ms one way, no loss, queue 50)

At the operational prompt:

```
sudo ethtool -K eth0 tso off gso off gro off
sudo ethtool -K eth0 lro off
sudo ethtool -K eth1 tso off gso off gro off
sudo ethtool -K eth1 lro off
sudo tc qdisc replace dev eth0 root netem rate 4mbit delay 25ms loss 0% limit 50
sudo tc qdisc replace dev eth1 root netem rate 4mbit delay 25ms loss 0% limit 50
tc qdisc show dev eth0
tc qdisc show dev eth1
```

The RTT of path B is 2 × 25 ms = 50 ms plus a little. The shaping is lost at a reboot; type
3.4 again.

## 3.5 Other network settings, show and remove

The settings of [benchmarks.md](benchmarks.md) 6.2 for router B:

```
# similar: 10 Mbit/s, 10 ms
sudo tc qdisc replace dev eth0 root netem rate 10mbit delay 10ms loss 0% limit 50
sudo tc qdisc replace dev eth1 root netem rate 10mbit delay 10ms loss 0% limit 50
# lossy: 4 Mbit/s, 25 ms, 1 % loss
sudo tc qdisc replace dev eth0 root netem rate 4mbit delay 25ms loss 1% limit 50
sudo tc qdisc replace dev eth1 root netem rate 4mbit delay 25ms loss 1% limit 50
# highbdp: 5 Mbit/s, 150 ms, longer queue
sudo tc qdisc replace dev eth0 root netem rate 5mbit delay 150ms loss 0% limit 200
sudo tc qdisc replace dev eth1 root netem rate 5mbit delay 150ms loss 0% limit 200
# handover-paper: 4 Mbit/s, 12 ms
sudo tc qdisc replace dev eth0 root netem rate 4mbit delay 12ms loss 0% limit 50
sudo tc qdisc replace dev eth1 root netem rate 4mbit delay 12ms loss 0% limit 50
```

Show and remove:

```
tc -s qdisc show dev eth0
tc -s qdisc show dev eth1
sudo tc qdisc del dev eth0 root
sudo tc qdisc del dev eth1 root
```

## 3.6 (Alternative) VyOS's own network emulator

As 2.6 of [router-a-vyos.md](router-a-vyos.md), with the name `MPQ-PATH-B` and path B's
values: `bandwidth '4mbit'`, `delay '25ms'` (VyOS 1.3: `network-delay '25ms'`),
`queue-limit '50'`.
