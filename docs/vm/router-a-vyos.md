# Step 2 — MPQ-Router-A (VyOS): path A

Router A joins `mpqa1` (client side, `eth0`) and `mpqa2` (server side, `eth1`). It forwards
packets and gives path A its capacity, delay, loss and queue.

Log in on the VM console (virt-manager window). The default user is `vyos`. VyOS must be
*installed* (`install image` from the live ISO), otherwise nothing is kept after a reboot.

> **Prompts.** `vyos@MPQ-Router-A:~$` is operational mode: `show …`, `sudo …`, and normal
> Linux commands work there. After `configure` the prompt ends in `#`: `set …` and `commit`
> work there, and `exit` leaves it.

## 2.1 Check the interface names

```
show interfaces ethernet
```

Compare the `hw-id` (MAC) of `eth0` and `eth1` with `virsh domiflist MPQ-Router-A` on the host
([host-kvm.md](host-kvm.md)). Expected: `eth0` = `mpqa1`, `eth1` = `mpqa2`. If they are the
other way round, swap `eth0` and `eth1` in every command of this file.

## 2.2 Addresses (kept after a reboot; once)

```
configure
set system host-name 'MPQ-Router-A'
delete interfaces ethernet eth0 address
delete interfaces ethernet eth1 address
set interfaces ethernet eth0 address '10.0.0.254/24'
set interfaces ethernet eth0 description 'mpqa1 client side path A'
set interfaces ethernet eth1 address '10.0.2.254/24'
set interfaces ethernet eth1 description 'mpqa2 server side path A'
commit
save
exit
```

- The two `delete` lines may answer "Nothing to delete"; that is fine.
- No static route is needed: both networks are directly connected to this router.

## 2.3 Check forwarding and firewall

```
show interfaces
sudo sysctl net.ipv4.ip_forward
show configuration commands | grep -E "firewall|qos|traffic-policy"
```

- **`show interfaces`:** eth0 shows 10.0.0.254/24 and eth1 shows 10.0.2.254/24, both `u/u` (up).
- **`ip_forward`:** must print 1. A router forwards by default; if it prints 0, check
  `show configuration commands | grep disable-forwarding` and delete that line in
  configuration mode.
- **The grep:** should print nothing. A firewall rule set could block the experiment, and an
  existing `qos` or `traffic-policy` would conflict with the shaping below.

## 2.4 Link shaping (path A = 8 Mbit/s, 10 ms one way, no loss, queue 50)

Type this at the **operational** prompt, not in configuration mode:

```
sudo ethtool -K eth0 tso off gso off gro off
sudo ethtool -K eth0 lro off
sudo ethtool -K eth1 tso off gso off gro off
sudo ethtool -K eth1 lro off
sudo tc qdisc replace dev eth0 root netem rate 8mbit delay 10ms loss 0% limit 50
sudo tc qdisc replace dev eth1 root netem rate 8mbit delay 10ms loss 0% limit 50
tc qdisc show dev eth0
tc qdisc show dev eth1
```

- **What it does:** netem shapes the traffic *leaving* an interface. Putting it on both
  interfaces gives both directions of path A the same rate, delay and loss, so the RTT of
  path A is 2 × 10 ms = 20 ms plus a little.
- **ethtool lines:** a line may print an error for a feature the virtual card does not
  have; that is fine.
- **Expected check output:** `qdisc netem … limit 50 delay 10ms rate 8Mbit` on both interfaces.
- **After a reboot:** the shaping is lost (the addresses of 2.2 are kept). Type 2.4 again,
  or use 2.6 instead.

## 2.5 Other network settings, show and remove

For another setting, type the two `tc qdisc replace` lines of 2.4 again with the new values
(`replace` changes the shaping that is there). The settings of [benchmarks.md](benchmarks.md) 6.2
for router A:

```
# similar: 10 Mbit/s, 10 ms
sudo tc qdisc replace dev eth0 root netem rate 10mbit delay 10ms loss 0% limit 50
sudo tc qdisc replace dev eth1 root netem rate 10mbit delay 10ms loss 0% limit 50
# lossy: 8 Mbit/s, 10 ms, 1 % loss
sudo tc qdisc replace dev eth0 root netem rate 8mbit delay 10ms loss 1% limit 50
sudo tc qdisc replace dev eth1 root netem rate 8mbit delay 10ms loss 1% limit 50
# highbdp: 20 Mbit/s, 100 ms, longer queue
sudo tc qdisc replace dev eth0 root netem rate 20mbit delay 100ms loss 0% limit 200
sudo tc qdisc replace dev eth1 root netem rate 20mbit delay 100ms loss 0% limit 200
# handover-paper: 8 Mbit/s, 7 ms
sudo tc qdisc replace dev eth0 root netem rate 8mbit delay 7ms loss 0% limit 50
sudo tc qdisc replace dev eth1 root netem rate 8mbit delay 7ms loss 0% limit 50
```

Show and remove:

```
tc -s qdisc show dev eth0          # counters: sent, dropped (queue full), overlimits
tc -s qdisc show dev eth1
sudo tc qdisc del dev eth0 root    # remove the shaping
sudo tc qdisc del dev eth1 root
```

## 2.6 (Alternative) VyOS's own network emulator, kept after a reboot

Use this **instead of** 2.4, never with it. First remove the tc shaping (the two `del` lines
of 2.5). It is configuration, so it survives reboots. The syntax follows the VyOS
documentation but was not tested here.

**VyOS 1.4 / 1.5:**

```
configure
set qos policy network-emulator MPQ-PATH-A bandwidth '8mbit'
set qos policy network-emulator MPQ-PATH-A delay '10ms'
set qos policy network-emulator MPQ-PATH-A queue-limit '50'
set qos interface eth0 egress 'MPQ-PATH-A'
set qos interface eth1 egress 'MPQ-PATH-A'
commit
save
exit
```

For loss, add `set qos policy network-emulator MPQ-PATH-A loss '1'` before `commit`.
To remove it:

```
configure
delete qos interface eth0
delete qos interface eth1
delete qos policy network-emulator MPQ-PATH-A
commit
save
exit
```

**VyOS 1.3:**

```
configure
set traffic-policy network-emulator MPQ-PATH-A bandwidth '8mbit'
set traffic-policy network-emulator MPQ-PATH-A network-delay '10ms'
set traffic-policy network-emulator MPQ-PATH-A packet-loss '0'
set traffic-policy network-emulator MPQ-PATH-A queue-limit '50'
set interfaces ethernet eth0 traffic-policy out 'MPQ-PATH-A'
set interfaces ethernet eth1 traffic-policy out 'MPQ-PATH-A'
commit
save
exit
```

To remove it:

```
configure
delete interfaces ethernet eth0 traffic-policy
delete interfaces ethernet eth1 traffic-policy
delete traffic-policy network-emulator MPQ-PATH-A
commit
save
exit
```

**Check:** `tc qdisc show dev eth0` shows what VyOS installed. It should be a netem with the
same rate and delay. If it is not, remove it and use 2.4.
