#include<bits/stdc++.h>

#include "sha256.hpp"

using namespace std;

// SHA-256 tests using FIPS 180-4 vectors

static vector<uint8_t> bytes(const string &s)
{
    return vector<uint8_t>(s.begin(), s.end());
}

void test_known_digests()
{
    cout<<"[TEST] SHA-256 standard vectors\n";

    assert(Sha256::hash(bytes(""))==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    assert(Sha256::hash(bytes("abc"))==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // 56 bytes needs 2 padding blocks
    assert(Sha256::hash(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))==
           "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // 1 million 'a' bytes
    assert(Sha256::hash(vector<uint8_t>(1000000, 'a'))==
           "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

    cout<<"  -> PASS\n";
}

void test_data_given_in_pieces()
{
    cout<<"[TEST] SHA-256 chunked updates\n";

    vector<uint8_t> data(100000);
    for(size_t i=0; i<data.size(); i++)
        data[i]=static_cast<uint8_t>((i*7+3)%256);
    string whole=Sha256::hash(data);

    // Same data in chunks of 1, 63, 64, 65, 1316 bytes
    for(size_t piece : {1u, 63u, 64u, 65u, 1316u}){
        Sha256 sha;
        for(size_t pos=0; pos<data.size(); pos+=piece){
            size_t n=min(piece, data.size()-pos);
            sha.update(data.data()+pos, n);
        }
        assert(sha.hex_digest()==whole);
    }

    // 1 bit flip changes digest
    data[50000]^=1;
    assert(Sha256::hash(data)!=whole);

    cout<<"  -> PASS\n";
}

int main()
{
    cout<<"=== SHA-256 Tests ===\n";
    test_known_digests();
    test_data_given_in_pieces();
    cout<<"=== All SHA-256 Tests Passed ===\n";
    return 0;
}
