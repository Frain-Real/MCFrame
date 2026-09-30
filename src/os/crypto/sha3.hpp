#pragma once
#include "../utilities.hpp"
namespace embed {
// Define macros for bitwise rotations, optimized for fixed-width types
#define ROTL64(x, n) (((x) << (n)) | ((x) >> (64 - (n))))

// --- SHA3 Class Definition ---
class SHA3 {
public:
    // Constructor (optional, can be empty as state is initialized in init())
    SHA3();

    // Initialize the hash function for a specific output size
    // For SHA3-224, 256, 384, 512. For SHAKE, use the SHAKE functions directly.
    void init(unsigned int outputBits);

    // Update the hash with input data
    void update(const uint8_t* data, size_t len);

    // Finalize the hash computation and get the digest
    void final(uint8_t* digest);

    // Specific SHA3-fixed output functions
    void sha3_224(const uint8_t* data, size_t len, uint8_t* digest);
    void sha3_256(const uint8_t* data, size_t len, uint8_t* digest);
    void sha3_384(const uint8_t* data, size_t len, uint8_t* digest);
    void sha3_512(const uint8_t* data, size_t len, uint8_t* digest);

    // SHAKE functions (XOF - eXtendable Output Function)
    void shake128_init();
    void shake256_init();
    void shake_update(const uint8_t* data, size_t len);
    void shake_squeeze(uint8_t* output, size_t len);

private:
    // Keccak state: 5x5 array of 64-bit lanes (1600 bits total)
    uint64_t state[5][5];

    // Current block buffer
    uint8_t block[200]; // Max block size for Keccak-f[1600] is 200 bytes (1600 bits)

    size_t blockLen;       // Current bytes in block
    unsigned int rate;     // Rate in bits (R)
    unsigned int capacity; // Capacity in bits (C)
    unsigned int outputLen; // Output length in bits (for fixed SHA3 functions)
    uint8_t d;             // Padding byte for SHA-3 (0x06) or SHAKE (0x1F)

    // Private helper functions
    void keccak_absorb(const uint8_t* data, size_t len);
    void keccak_squeeze(uint8_t* output, size_t len);
    void keccak_permutation();
    void keccak_set_parameters(unsigned int outputBits, bool isSHAKE);
    void pad_and_absorb_last_block();
    void clear_state();
};

// --- SHA3 Class Implementation ---

SHA3::SHA3() {
    clear_state();
    blockLen = 0;
    rate = 0;
    capacity = 0;
    outputLen = 0;
    d = 0;
}

void SHA3::clear_state() {
    utilities::zero(state);
    utilities::zero(block);
}

// Keccak-f[1600] permutation round constants
static const uint64_t KeccakP1600RoundConstants[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

// Keccak-f[1600] permutation
void SHA3::keccak_permutation() {
    uint64_t A[5][5];
    uint64_t C[5], D[5];
    uint64_t B[5][5];

    // Copy current state to working A
    for (int x = 0; x < 5; ++x) {
        for (int y = 0; y < 5; ++y) {
            A[x][y] = state[x][y];
        }
    }

    for (int round = 0; round < 24; ++round) {
        // Theta step
        for (int x = 0; x < 5; ++x) {
            C[x] = A[x][0] ^ A[x][1] ^ A[x][2] ^ A[x][3] ^ A[x][4];
        }
        for (int x = 0; x < 5; ++x) {
            D[x] = C[(x + 4) % 5] ^ ROTL64(C[(x + 1) % 5], 1);
            for (int y = 0; y < 5; ++y) {
                A[x][y] ^= D[x];
            }
        }

        // Rho and Pi steps
        int x = 1, y = 0;
        uint64_t current = A[x][y];
        for (int t = 0; t < 24; ++t) {
            int newX = y;
            int newY = (2 * x + 3 * y) % 5;
            int rotVal = (t + 1) * (t + 2) / 2 % 64; // Rotation offset for Pi step
            B[newX][newY] = ROTL64(current, rotVal);
            current = A[newX][newY];
            x = newX;
            y = newY;
        }

        // Chi step
        for (int x = 0; x < 5; ++x) {
            for (int y = 0; y < 5; ++y) {
                A[x][y] = B[x][y] ^ ((~B[(x + 1) % 5][y]) & B[(x + 2) % 5][y]);
            }
        }

        // Iota step
        A[0][0] ^= KeccakP1600RoundConstants[round];
    }

    // Copy back to actual state
    for (int x = 0; x < 5; ++x) {
        for (int y = 0; y < 5; ++y) {
            state[x][y] = A[x][y];
        }
    }
}

// Converts bytes to 64-bit lanes (little-endian)
static uint64_t bytes_to_u64(const uint8_t* bytes) {
    uint64_t val = 0;
    for (int i = 0; i < 8; ++i) {
        val |= (uint64_t)bytes[i] << (i * 8);
    }
    return val;
}

// Converts 64-bit lanes to bytes (little-endian)
static void u64_to_bytes(uint64_t val, uint8_t* bytes) {
    for (int i = 0; i < 8; ++i) {
        bytes[i] = (uint8_t)(val >> (i * 8));
    }
}

void SHA3::keccak_set_parameters(unsigned int outputBits, bool isSHAKE) {
    clear_state();
    outputLen = outputBits;

    if (isSHAKE) {
        // SHAKE functions
        if (outputBits == 128) {
            rate = 1344; // 168 bytes
            capacity = 256;
            d = 0x1F; // SHAKE padding suffix
        } else if (outputBits == 256) {
            rate = 1088; // 136 bytes
            capacity = 512;
            d = 0x1F; // SHAKE padding suffix
        } else {
            // Should not happen if called correctly for SHAKE128/256
        }
    } else {
        // SHA3 fixed-output functions
        switch (outputBits) {
            case 224:
                rate = 1152; // 144 bytes
                capacity = 448;
                break;
            case 256:
                rate = 1088; // 136 bytes
                capacity = 512;
                break;
            case 384:
                rate = 832; // 104 bytes
                capacity = 768;
                break;
            case 512:
                rate = 576; // 72 bytes
                capacity = 1024;
                break;
            default:
                // Error handling or throw exception if desired
                return;
        }
        d = 0x06; // SHA-3 padding suffix
    }
    blockLen = 0; // Reset block buffer
}

// Absorb phase for Keccak
void SHA3::keccak_absorb(const uint8_t* data, size_t len) {
    size_t rate_bytes = rate / 8; // Rate in bytes

    while (len > 0) {
        size_t copy_len = rate_bytes - blockLen;
        if (copy_len > len) {
            copy_len = len;
        }

        utilities::fast_memcpy(&block[blockLen], data, copy_len);
        blockLen += copy_len;
        data += copy_len;
        len -= copy_len;

        if (blockLen == rate_bytes) {
            // Absorb the block into the state
            for (size_t i = 0; i < rate_bytes / 8; ++i) {
                state[i % 5][i / 5] ^= bytes_to_u64(&block[i * 8]);
            }
            keccak_permutation();
            blockLen = 0;
        }
    }
}

// Padding and absorb the last block (SHA3 specific: 0x06, SHAKE specific: 0x1F)
void SHA3::pad_and_absorb_last_block() {
    size_t rate_bytes = rate / 8;

    // Append 0x01 (or 0x1F for SHAKE)
    block[blockLen++] = d;

    // Pad with zeros
    while (blockLen < rate_bytes) {
        block[blockLen++] = 0x00;
    }

    // Append 0x80
    block[rate_bytes - 1] |= 0x80;

    // Absorb the last block
    for (size_t i = 0; i < rate_bytes / 8; ++i) {
        state[i % 5][i / 5] ^= bytes_to_u64(&block[i * 8]);
    }
    keccak_permutation();
}

// Squeeze phase for Keccak
void SHA3::keccak_squeeze(uint8_t* output, size_t len) {
    size_t rate_bytes = rate / 8;

    size_t output_bytes_count = 0;
    while (output_bytes_count < len) {
        size_t copy_len = len - output_bytes_count;
        if (copy_len > rate_bytes) {
            copy_len = rate_bytes;
        }

        for (size_t i = 0; i < copy_len / 8; ++i) {
            u64_to_bytes(state[i % 5][i / 5], &output[output_bytes_count + i * 8]);
        }
        
        // Handle remaining bytes if copy_len is not a multiple of 8
        if (copy_len % 8 != 0) {
             uint8_t temp_lane_bytes[8];
             u64_to_bytes(state[(copy_len / 8) % 5][(copy_len / 8) / 5], temp_lane_bytes);
             utilities::fast_memcpy(&output[output_bytes_count + (copy_len / 8) * 8], temp_lane_bytes, copy_len % 8);
        }

        output_bytes_count += copy_len;
        
        if (output_bytes_count < len) {
            keccak_permutation(); // Permute if more output is needed
        }
    }
}

// --- Public API for SHA3 fixed output ---

void SHA3::init(unsigned int outputBits) {
    keccak_set_parameters(outputBits, false);
}

void SHA3::update(const uint8_t* data, size_t len) {
    keccak_absorb(data, len);
}

void SHA3::final(uint8_t* digest) {
    pad_and_absorb_last_block();
    keccak_squeeze(digest, outputLen / 8);
    clear_state(); // Clear state after finalization
}

void SHA3::sha3_224(const uint8_t* data, size_t len, uint8_t* digest) {
    init(224);
    update(data, len);
    final(digest);
}

void SHA3::sha3_256(const uint8_t* data, size_t len, uint8_t* digest) {
    init(256);
    update(data, len);
    final(digest);
}

void SHA3::sha3_384(const uint8_t* data, size_t len, uint8_t* digest) {
    init(384);
    update(data, len);
    final(digest);
}

void SHA3::sha3_512(const uint8_t* data, size_t len, uint8_t* digest) {
    init(512);
    update(data, len);
    final(digest);
}

// --- Public API for SHAKE (XOF) ---

void SHA3::shake128_init() {
    keccak_set_parameters(128, true);
}

void SHA3::shake256_init() {
    keccak_set_parameters(256, true);
}

void SHA3::shake_update(const uint8_t* data, size_t len) {
    keccak_absorb(data, len);
}

void SHA3::shake_squeeze(uint8_t* output, size_t len) {
    if (blockLen > 0 || state[0][0] == 0) {
        pad_and_absorb_last_block();
    }
    keccak_squeeze(output, len);
}
}