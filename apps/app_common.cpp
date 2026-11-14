#include <bits/stdc++.h>
#include <arpa/inet.h>

#include "app_common.hpp"

using namespace std;

Arguments::Arguments(int argc, char **argv, const set<string> &flags)
{
    for (int i = 1; i < argc; i++)
    {
        string arg = argv[i];
        if (arg.size() < 3 || arg.substr(0, 2) != "--")
        {
            error_message = "\"" + arg + "\" is not an option (every option starts with --, see the list below)";
            return;
        }
        string name = arg.substr(2);

        if (flags.count(name))
        {
            values[name].push_back("true");
            continue;
        }
        if (i + 1 >= argc)
        {
            error_message = "the option --" + name + " needs a value after it";
            return;
        }
        values[name].push_back(argv[++i]);
    }
}

bool Arguments::has(const string &name) const
{
    return values.count(name) > 0;
}

string Arguments::get(const string &name, const string &default_value) const
{
    auto it = values.find(name);
    if (it == values.end())
    {
        return default_value;
    }
    return it->second.back();
}

vector<string> Arguments::get_all(const string &name) const
{
    auto it = values.find(name);
    if (it == values.end())
    {
        return {};
    }
    return it->second;
}

long Arguments::get_number(const string &name, long default_value) const
{
    if (!has(name))
    {
        return default_value;
    }
    return atol(get(name).c_str());
}

bool Arguments::ok() const
{
    return error_message.empty();
}

string Arguments::error() const
{
    return error_message;
}

bool parse_address(const string &text, NetworkAddress &out)
{
    size_t colon = text.rfind(':');
    if (colon == string::npos || colon == 0 || colon + 1 >= text.size())
    {
        return false;
    }

    string ip = text.substr(0, colon);
    long port = atol(text.substr(colon + 1).c_str());

    in_addr ipv4;
    if (inet_pton(AF_INET, ip.c_str(), &ipv4) != 1 || port <= 0 || port > 65535)
    {
        return false;
    }

    out.ip = ip;
    out.port = static_cast<uint16_t>(port);
    return true;
}

bool read_file(const string &path, vector<uint8_t> &out)
{
    ifstream file(path, ios::binary | ios::ate); // opened at the end, so tellg() is the size
    if (!file)
    {
        return false;
    }
    streamoff size = file.tellg();
    if (size < 0)
    {
        return false;
    }
    // One read of the whole size (reading character by character is about 10 times slower)
    out.resize(static_cast<size_t>(size));
    file.seekg(0);
    return size == 0 || file.read(reinterpret_cast<char *>(out.data()), size).good();
}

void print_path_statistics(Connection &connection)
{
    for (const auto &[path_id, path] : connection.get_paths())
    {
        SentPacketHandler *sent = path->get_sent_packet_handler();
        ReceivedPacketHandler *received = path->get_received_packet_handler();
        OliaSender *olia = path->get_congestion();

        cout << "PATH id=" << (int)path->get_id()
             << " local=" << path->get_local_ip() << ":" << path->get_local_port()
             << " remote=" << path->get_remote_ip() << ":" << path->get_remote_port()
             << " sent=" << sent->get_packets_sent()
             << " lost=" << sent->get_losses()
             << " retransmitted=" << sent->get_retransmissions()
             << " received=" << received->get_packets_received()
             << " srtt_us=" << path->get_rtt_stats()->get_smoothed_rtt()
             << " cwnd_packets=" << olia->get_congestion_window_packets()
             << " potentially_failed=" << (path->is_potentially_failed() ? 1 : 0) << "\n";
    }
}

bool StatsWriter::open(const string &path, uint64_t interval_ms)
{
    file.open(path);
    if (!file)
    {
        return false;
    }
    interval_us = interval_ms * 1000;
    start_us = 0; // the clock of the file starts with the first sample
    next_sample_us = 0;

    file << "time_ms,path_id,packets_sent,packets_lost,packets_retransmitted,packets_received,"
         << "cwnd_packets,ssthresh_packets,slow_start,srtt_us,bytes_in_flight,potentially_failed,"
         << "bytes_transferred\n";
    return true;
}

bool StatsWriter::is_open() const
{
    return file.is_open();
}

void StatsWriter::maybe_sample(Connection &connection, uint64_t bytes_transferred)
{
    uint64_t now_us = Packet::current_time_us();
    if (!file.is_open() || now_us < next_sample_us)
    {
        return;
    }
    sample(connection, bytes_transferred);
    next_sample_us = now_us + interval_us;
}

void StatsWriter::sample(Connection &connection, uint64_t bytes_transferred)
{
    if (!file.is_open())
    {
        return;
    }

    if (start_us == 0)
    {
        start_us = Packet::current_time_us();
    }
    uint64_t time_ms = (Packet::current_time_us() - start_us) / 1000;
    for (const auto &[path_id, path] : connection.get_paths())
    {
        SentPacketHandler *sent = path->get_sent_packet_handler();
        OliaSender *olia = path->get_congestion();

        file << time_ms << "," << (int)path->get_id()
             << "," << sent->get_packets_sent()
             << "," << sent->get_losses()
             << "," << sent->get_retransmissions()
             << "," << path->get_received_packet_handler()->get_packets_received()
             << "," << olia->get_congestion_window_packets()
             << "," << olia->get_slow_start_threshold_packets()
             << "," << (olia->in_slow_start() ? 1 : 0)
             << "," << path->get_rtt_stats()->get_smoothed_rtt()
             << "," << sent->get_bytes_in_flight()
             << "," << (path->is_potentially_failed() ? 1 : 0)
             << "," << bytes_transferred << "\n";
    }
    file.flush();
}
