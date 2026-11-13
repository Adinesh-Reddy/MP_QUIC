#ifndef SHA256_HPP
#define SHA256_HPP

#include <bits/stdc++.h>

using namespace std;

// SHA-256 (FIPS 180-4), used by the programs to check that a transferred file is identical
// to the original. It is a test of the transfer, not a security feature of the protocol.
// The data can be given in several pieces, so a large file never has to be in memory twice.
class Sha256
{
public:
    Sha256();

    void update(const uint8_t *data, size_t length);

    // The 64 hexadecimal characters of the digest (same text as the sha256sum command).
    // Call it once, when all the data was given.
    string hex_digest();

    // Digest of a whole buffer in one call
    static string hash(const vector<uint8_t> &data);

private:
    void transform(const uint8_t *block);

    uint32_t state[8];
    uint8_t buffer[64];
    size_t buffer_length = 0;
    uint64_t total_length = 0; // in bytes
};

#endif
