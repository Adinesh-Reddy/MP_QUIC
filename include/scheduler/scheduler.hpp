#ifndef SCHEDULER_HPP
#define SCHEDULER_HPP

#include<bits/stdc++.h>
#include "path/path.hpp"

using namespace std;

// Picks which path sends the next packet (interface so new schedulers can plug in)
class Scheduler
{
public:
    virtual ~Scheduler() {}

    /*
     Pick path for next packet, or nullptr if none can send.
     has_retransmission = true allows using paths with full congestion windows.
    */
    virtual Path *select_path(const map<uint8_t, Path *> &paths, bool has_retransmission)=0;

    // Pick another path to send a duplicate copy on (used when RTT is unknown), or nullptr
    virtual Path *select_duplicate_path(const map<uint8_t, Path *> &paths, Path *sent_on)=0;

    // Notify scheduler that a packet was placed on path
    virtual void on_packet_sent(Path *path)=0;

    virtual string get_name() const=0;
};

// Selects the healthy path with the lowest smoothed RTT
class LowestRttScheduler : public Scheduler
{
public:
    Path *select_path(const map<uint8_t, Path *> &paths, bool has_retransmission) override;
    Path *select_duplicate_path(const map<uint8_t, Path *> &paths, Path *sent_on) override;
    void on_packet_sent(Path *path) override;
    string get_name() const override;

    // Packets placed on this path so far
    uint64_t packets_placed_on(uint8_t path_id) const;

private:
    map<uint8_t, uint64_t> packets_placed;
};

#endif
