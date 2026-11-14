#ifndef SCHEDULER_HPP
#define SCHEDULER_HPP

#include <bits/stdc++.h>
#include "path/path.hpp"

using namespace std;

// The packet scheduler decides on which path the next packet is sent.
// It is an interface so that other schedulers can be added later without touching the Connection.
class Scheduler
{
public:
    virtual ~Scheduler() {}

    // Path for the next packet, nullptr if no path can be used now.
    // has_retransmission = lost data was just collected: it must go out even if the
    // congestion window of the path is full.
    virtual Path *select_path(const map<uint8_t, Path *> &paths, bool has_retransmission) = 0;

    // Called after a packet with stream data was sent on sent_on. Returns another path on which
    // the same data must be sent again (a duplicate), or nullptr if no duplicate is needed.
    virtual Path *select_duplicate_path(const map<uint8_t, Path *> &paths, Path *sent_on) = 0;

    // Must be called for every packet the scheduler placed on a path
    virtual void on_packet_sent(Path *path) = 0;

    virtual string get_name() const = 0;
};

// Lowest-RTT scheduling. For every packet:
//   1. Skip a path whose congestion window is full (retransmissions may still use it).
//   2. Skip a path marked potentially failed.
//   3. Of the paths left, take the one with the smallest smoothed RTT.
//   4. A path with no RTT yet (nothing acknowledged on it) loses against any path with one.
//      When no path has an RTT, take the one that has carried the fewest packets.
//   5. While the chosen path has no RTT, its data is also copied onto another path. A fresh
//      path is then used right away, without the risk of its data arriving very late.
class LowestRttScheduler : public Scheduler
{
public:
    Path *select_path(const map<uint8_t, Path *> &paths, bool has_retransmission) override;
    Path *select_duplicate_path(const map<uint8_t, Path *> &paths, Path *sent_on) override;
    void on_packet_sent(Path *path) override;
    string get_name() const override;

    // Number of packets this scheduler has placed on a path
    uint64_t packets_placed_on(uint8_t path_id) const;

private:
    map<uint8_t, uint64_t> packets_placed;
};

#endif
