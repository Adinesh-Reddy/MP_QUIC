#include <iostream>
#include <cassert>
#include <vector>
#include <string>

#include "sha256.hpp"

// Tests of the SHA-256 used by the programs, with the official test vectors (FIPS 180-4)

static std::vector<uint8_t> bytes(const std::string &s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

void test_known_digests()
{
    std::cout << "[TEST] SHA-256 of the standard test messages...\n";

    assert(Sha256::hash(bytes("")) ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(Sha256::hash(bytes("abc")) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // 56 bytes: the padding needs a second block
    assert(Sha256::hash(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // One million times the letter 'a'
    assert(Sha256::hash(std::vector<uint8_t>(1000000, 'a')) ==
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    std::cout << "  -> PASSED\n";
}

void test_data_given_in_pieces()
{
    std::cout << "[TEST] SHA-256 of data given in pieces of any size...\n";

    std::vector<uint8_t> data(100000);
    for (size_t i = 0; i < data.size(); i++)
        data[i] = static_cast<uint8_t>((i * 7 + 3) % 256);
    std::string whole = Sha256::hash(data);

    // The same data in pieces of 1, 63, 64, 65 and 1316 bytes (the size of a STREAM frame)
    for (size_t piece : {1u, 63u, 64u, 65u, 1316u})
    {
        Sha256 sha;
        for (size_t pos = 0; pos < data.size(); pos += piece)
        {
            size_t n = std::min(piece, data.size() - pos);
            sha.update(data.data() + pos, n);
        }
        assert(sha.hex_digest() == whole);
    }

    // One changed byte gives a different digest
    data[50000] ^= 1;
    assert(Sha256::hash(data) != whole);

    std::cout << "  -> PASSED\n";
}

int main()
{
    std::cout << "=== Running SHA-256 Tests ===\n";
    test_known_digests();
    test_data_given_in_pieces();
    std::cout << "=== All SHA-256 Tests Passed Successfully! ===\n";
    return 0;
}
