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

// The scheduler of the paper (Section 3, Packet Scheduling), same rules as the
// reference implementation:
//   1. A path whose congestion window is full is not used (except for retransmissions).
//   2. A potentially failed path is not used.
//   3. Among the remaining paths the one with the lowest smoothed RTT is taken.
//   4. A path whose RTT is not known yet (no ACK received on it) loses against a path with
//      a known RTT. If no RTT is known at all, the path that sent the fewest packets is taken.
//   5. While the RTT of the chosen path is unknown, the data is duplicated on another path,
//      so a new path can be used at once without risking that its data arrives very late.
class LowestRttScheduler : public Scheduler
{
public:
    Path *select_path(const map<uint8_t, Path *> &paths, bool has_retransmission) override;
    Path *select_duplicate_path(const map<uint8_t, Path *> &paths, Path *sent_on) override;
    void on_packet_sent(Path *path) override;
    string get_name() const override;

    // Number of packets this scheduler placed on a path ("quota" in the reference implementation)
    uint64_t get_quota(uint8_t path_id) const;

private:
    map<uint8_t, uint64_t> quotas;
};

#endif
