#ifndef APP_COMMON_HPP
#define APP_COMMON_HPP

#include<bits/stdc++.h>
#include "packet/frame.hpp"
#include "connection/connection.hpp"

using namespace std;

// Shared helpers for mpq_server, mpq_client, and mpq_reqres

// Parses "--name value" and "--flag" options (repeatable options allowed)
class Arguments
{
public:
    // flags = options with no value
    Arguments(int argc, char **argv, const set<string> &flags);

    bool has(const string &name) const;
    string get(const string &name, const string &default_value="") const;
    vector<string> get_all(const string &name) const;
    long get_number(const string &name, long default_value) const;

    // False if argument parsing failed (see error())
    bool ok() const;
    string error() const;

private:
    map<string, vector<string>> values;
    string error_message;
};

// Parse "IPv4:port" string
bool parse_address(const string &text, NetworkAddress &out);

// Read whole file into out; false on open/read error
bool read_file(const string &path, vector<uint8_t> &out);

// Print one stats line per path
void print_path_statistics(Connection &connection);

// Writes periodic path stats (cwnd, RTT, packets) to a CSV file
class StatsWriter
{
public:
    // Create CSV file; interval_ms = time between rows
    bool open(const string &path, uint64_t interval_ms=100);
    bool is_open() const;

    // Write a row if interval_ms has passed
    void maybe_sample(Connection &connection, uint64_t bytes_transferred);

    // Write one row right now
    void sample(Connection &connection, uint64_t bytes_transferred);

private:
    ofstream file;
    uint64_t interval_us=100000;
    uint64_t start_us=0;
    uint64_t next_sample_us=0;
};

#endif
