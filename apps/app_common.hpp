#ifndef APP_COMMON_HPP
#define APP_COMMON_HPP

#include <bits/stdc++.h>
#include "packet/frame.hpp"
#include "connection/connection.hpp"

using namespace std;

// Code shared by the programs of apps/ (mpq_server, mpq_client, mpq_reqres)

// Command line of the form:  --name value --name value --flag
// An option can be given several times (--local 10.0.0.1 --local 10.0.1.1).
class Arguments
{
public:
    // flags = the options that take no value
    Arguments(int argc, char **argv, const set<string> &flags);

    bool has(const string &name) const;
    string get(const string &name, const string &default_value = "") const;
    vector<string> get_all(const string &name) const;
    long get_number(const string &name, long default_value) const;

    // False if the command line could not be understood (see error())
    bool ok() const;
    string error() const;

private:
    map<string, vector<string>> values;
    string error_message;
};

// "10.0.0.2:4433" -> address. Returns false if the text is not IPv4:port.
bool parse_address(const string &text, NetworkAddress &out);

// Reads a whole file. Returns false if it cannot be opened.
bool read_file(const string &path, vector<uint8_t> &out);

// One line per path with what the connection did on it (packets, losses, RTT, window)
void print_path_statistics(Connection &connection);

// Writes the state of every path to a CSV file at regular intervals, for the plots
// of the evaluation (window and RTT over time, share of each path).
class StatsWriter
{
public:
    // Returns false if the file cannot be created. interval_ms = time between two samples.
    bool open(const string &path, uint64_t interval_ms = 100);
    bool is_open() const;

    // Call it often (every tick): it writes a sample when the interval has passed.
    // bytes_transferred = application bytes sent or received so far.
    void maybe_sample(Connection &connection, uint64_t bytes_transferred);

    // Writes a last sample now
    void sample(Connection &connection, uint64_t bytes_transferred);

private:
    ofstream file;
    uint64_t interval_us = 100000;
    uint64_t start_us = 0;
    uint64_t next_sample_us = 0;
};

#endif
