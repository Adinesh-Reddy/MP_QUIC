#ifndef SHA256_HPP
#define SHA256_HPP

#include<bits/stdc++.h>

using namespace std;

// Streaming SHA-256 hash calculation for file integrity checks
class Sha256
{
public:
    Sha256();

    void update(const uint8_t *data, size_t length);

    // 64-char hex digest (call once at end)
    string hex_digest();

    // Hash whole buffer in one call
    static string hash(const vector<uint8_t> &data);

private:
    void transform(const uint8_t *block);

    uint32_t state[8];
    uint8_t buffer[64];
    size_t buffer_length=0;
    uint64_t total_length=0; // bytes
};

#endif
