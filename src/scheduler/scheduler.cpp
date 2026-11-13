#include <bits/stdc++.h>

#include "scheduler/scheduler.hpp"

using namespace std;

uint64_t LowestRttScheduler::get_quota(uint8_t path_id) const
{
    auto it = quotas.find(path_id);
    if (it == quotas.end())
    {
        return 0;
    }
    return it->second;
}

void LowestRttScheduler::on_packet_sent(Path *path)
{
    quotas[path->get_id()]++;
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

    // With a single path there is no choice, it is used even if it looks broken
    if (paths.size() == 1)
    {
        Path *path = paths.begin()->second;
        if (!has_retransmission && !path->sending_allowed())
        {
            return nullptr;
        }
        return path;
    }

    // Not in the reference implementation: when every path looks broken we must keep
    // trying on them, otherwise the connection could never recover
    bool all_paths_failed = true;
    for (const auto &entry : paths)
    {
        if (!entry.second->is_potentially_failed())
            all_paths_failed = false;
    }

    Path *selected_path = nullptr;
    uint64_t lower_rtt = 0; // smoothed RTT of selected_path, 0 = not known yet

    // The paths are visited in Path ID order, so when two paths are equal the lower ID wins
    for (const auto &entry : paths)
    {
        Path *path = entry.second;

        // Don't block path usage if we retransmit, even on another path
        if (!has_retransmission && !path->sending_allowed())
        {
            continue;
        }

        // If this path is potentially failed, do not consider it for sending
        if (path->is_potentially_failed() && !all_paths_failed)
        {
            continue;
        }

        uint64_t current_rtt = path->get_rtt_stats()->get_smoothed_rtt();

        if (selected_path != nullptr)
        {
            // A path with a known RTT is preferred to a path we know nothing about
            if (lower_rtt != 0 && current_rtt == 0)
            {
                continue;
            }

            // Several paths without RTT: take the one that sent the fewest packets
            if (lower_rtt == 0 && current_rtt == 0 &&
                get_quota(path->get_id()) >= get_quota(selected_path->get_id()))
            {
                continue;
            }

            // Both RTTs are known: keep the lowest one
            if (lower_rtt != 0 && current_rtt != 0 && current_rtt >= lower_rtt)
            {
                continue;
            }
        }

        lower_rtt = current_rtt;
        selected_path = path;
    }

    return selected_path;
}

Path *LowestRttScheduler::select_duplicate_path(const map<uint8_t, Path *> &paths, Path *sent_on)
{
    // Duplicate traffic only when it was sent on a path whose performance is unknown
    if (sent_on == nullptr || sent_on->get_rtt_stats()->get_smoothed_rtt() != 0)
    {
        return nullptr;
    }

    uint64_t current_quota = get_quota(sent_on->get_id());

    for (const auto &entry : paths)
    {
        Path *path = entry.second;
        if (path == sent_on || path->is_potentially_failed())
        {
            continue;
        }

        // A path that sent fewer packets did not carry this packet yet. The duplicate is
        // ordinary traffic for that path, so its congestion window must have room.
        if (get_quota(path->get_id()) < current_quota && path->sending_allowed())
        {
            return path;
        }
    }
    return nullptr;
}
