#include <bits/stdc++.h>

#include "scheduler/scheduler.hpp"

using namespace std;

uint64_t LowestRttScheduler::packets_placed_on(uint8_t path_id) const
{
    auto it = packets_placed.find(path_id);
    return (it == packets_placed.end()) ? 0 : it->second;
}

void LowestRttScheduler::on_packet_sent(Path *path)
{
    packets_placed[path->get_id()]++;
}

string LowestRttScheduler::get_name() const
{
    return "lowest-rtt";
}

Path *LowestRttScheduler::select_path(const map<uint8_t, Path *> &paths, bool has_retransmission)
{
    if (paths.empty())
    {
        return nullptr;
    }

    // A single path is the only choice: it is used even if it looks broken
    if (paths.size() == 1)
    {
        Path *only = paths.begin()->second;
        if (!has_retransmission && !only->sending_allowed())
        {
            return nullptr;
        }
        return only;
    }

    // If every path looks broken, keep trying them all: skipping them would leave the
    // connection with no way to find out that one of them works again
    bool every_path_failed = all_of(paths.begin(), paths.end(), [](const auto &entry) {
        return entry.second->is_potentially_failed();
    });

    Path *chosen = nullptr;
    uint64_t chosen_rtt = 0; // smoothed RTT of the chosen path, 0 = unknown

    // Paths are visited in Path ID order, so on a tie the lower ID is kept
    for (const auto &[key, path] : paths)
    {
        // A full window blocks new data, but never retransmissions
        if (!has_retransmission && !path->sending_allowed())
        {
            continue;
        }

        if (path->is_potentially_failed() && !every_path_failed)
        {
            continue;
        }

        uint64_t rtt = path->get_rtt_stats()->get_smoothed_rtt();

        if (chosen != nullptr)
        {
            // A measured path beats one we know nothing about
            if (chosen_rtt != 0 && rtt == 0)
            {
                continue;
            }

            // Neither is measured: prefer the one that has carried fewer packets
            if (chosen_rtt == 0 && rtt == 0 && packets_placed_on(path->get_id()) >= packets_placed_on(chosen->get_id()))
            {
                continue;
            }

            // Both measured: the smaller RTT wins
            if (chosen_rtt != 0 && rtt != 0 && rtt >= chosen_rtt)
            {
                continue;
            }
        }

        chosen_rtt = rtt;
        chosen = path;
    }

    return chosen;
}

Path *LowestRttScheduler::select_duplicate_path(const map<uint8_t, Path *> &paths, Path *sent_on)
{
    // Copies are only made for data sent on a path whose RTT is still unknown
    if (sent_on == nullptr || sent_on->get_rtt_stats()->get_smoothed_rtt() != 0)
    {
        return nullptr;
    }

    uint64_t placed_on_original = packets_placed_on(sent_on->get_id());

    auto candidate = find_if(paths.begin(), paths.end(), [&](const auto &entry) {
        Path *path = entry.second;
        if (path == sent_on || path->is_potentially_failed())
        {
            return false;
        }
        // A path that has carried fewer packets has not received this data yet. For that
        // path the copy is ordinary traffic, so its congestion window must have room.
        return packets_placed_on(path->get_id()) < placed_on_original && path->sending_allowed();
    });

    return (candidate == paths.end()) ? nullptr : candidate->second;
}
