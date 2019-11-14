#include <stdint.h>
#include <immintrin.h>

// Faster UTF-8 SIMD validator
//
//   [[[ UTF-8 refresher: UTF-8 encodes text in sequences of "code points",
//   each one from 1-4 bytes. For each code point that is longer than one byte,
//   the code point begins with a unique prefix that specifies how many bytes
//   follow. All bytes in the code point after this first have a continuation
//   marker. All code points in UTF-8 will thus look like one of the following
//   binary sequences, with x meaning "don't care":
//      1 byte:  0xxxxxxx
//      2 bytes: 110xxxxx  10xxxxxx
//      3 bytes: 1110xxxx  10xxxxxx  10xxxxxx
//      4 bytes: 11110xxx  10xxxxxx  10xxxxxx  10xxxxxx
//   ]]]
//
// This validator works in two basic steps: checking continuation bytes, and
// handling special cases. Each step works on one vector's worth of input
// bytes at a time.
//
// The continuation bytes are handled in a fairly straightforward manner in
// the scalar domain. A mask is created from the input byte vector for each
// of the highest four bits of every byte. The first mask allows us to quickly
// skip pure ASCII input vectors, which have no bits set. The first and
// (inverted) second masks together give us every continuation byte (10xxxxxx).
// The other masks are used to find prefixes of multi-byte code points (110,
// 1110, 11110). For these, we keep a "required continuation" mask, by shifting
// these masks 1, 2, and 3 bits respectively forward in the byte stream. That
// is, we take a mask of all bytes that start with 11, and shift it left one
// bit forward to get the mask of all the first continuation bytes, then do the
// same for the second and third continuation bytes. Here's an example input
// sequence along with the corresponding masks:
//
//   bytes:        61 C3 80 62 E0 A0 80 63 F0 90 80 80 00
//   code points:  61|C3 80|62|E0 A0 80|63|F0 90 80 80|00
//   # of bytes:   1 |2  - |1 |3  -  - |1 |4  -  -  - |1
//   cont. mask 1: -  -  1  -  -  1  -  -  -  1  -  -  -
//   cont. mask 2: -  -  -  -  -  -  1  -  -  -  1  -  -
//   cont. mask 3: -  -  -  -  -  -  -  -  -  -  -  1  -
//   cont. mask *: 0  0  1  0  0  1  1  0  0  1  1  1  0
//
// The final required continuation mask is then compared with the mask of
// actual continuation bytes, and must match exactly in valid UTF-8. The only
// complication in this step is that the shifted masks can cross vector
// boundaries, so we need to keep a "carry" mask of the bits that were shifted
// past the boundary in the last loop iteration.
//
// Besides the basic prefix coding of UTF-8, there are several invalid byte
// sequences that need special handling. These are due to three factors:
// code points that could be described in fewer bytes, code points that are
// part of a surrogate pair (which are only valid in UTF-16), and code points
// that are past the highest valid code point U+10FFFF.
//
// All of the invalid sequences can be detected by independently observing
// the first three nibbles of each code point. Since AVX2 can do a 4-bit/16-byte
// lookup in parallel for all 32 bytes in a vector, we can create bit masks
// for all of these error conditions, look up the bit masks for the three
// nibbles for all input bytes, and AND them together to get a final error mask,
// that must be all zero for valid UTF-8. This is somewhat complicated by
// needing to shift the error masks from the first and second nibbles forward in
// the byte stream to line up with the third nibble.
//
// We have these possible values for valid UTF-8 sequences, broken down
// by the first three nibbles:
//
//   1st   2nd   3rd   comment
//   0..7  0..F        ASCII
//   8..B  0..F        continuation bytes
//   C     2..F  8..B  C0 xx and C1 xx can be encoded in 1 byte
//   D     0..F  8..B  D0..DF are valid with a continuation byte
//   E     0     A..B  E0 8x and E0 9x can be encoded with 2 bytes
//         1..C  8..B  E1..EC are valid with continuation bytes
//         D     8..9  ED Ax and ED Bx correspond to surrogate pairs
//         E..F  8..B  EE..EF are valid with continuation bytes
//   F     0..3  8..B  F0..F3 are valid with continuation bytes
//         4     8     F4 8F BF BF is the maximum valid code point
//
// That leaves us with these invalid sequences, which would otherwise fit
// into UTF-8's prefix encoding. Each of these invalid sequences needs to
// be detected separately, with their own bits in the error mask.
//
//   1st   2nd   3rd   error bit
//   C     0..1  0..F  0x01
//   E     0     8..9  0x02
//         D     A..B  0x04
//   F     4     9..F  0x08
//   F     5..F  0..F  0x10
//
// For every possible value of the first, second, and third nibbles, we keep
// a lookup table that contains the bitwise OR of all errors that that nibble
// value can cause. For example, the first nibble has zeroes in every entry
// except for C, E, and F, and the third nibble lookup has the 0x11 bits in
// every entry, since those errors don't depend on the third nibble. After
// doing a parallel lookup of the first/second/third nibble values for all
// bytes, we AND them together. Only when all three have an error bit in common
// do we fail validation.

// Right now, this code is only for AVX-2
typedef __m256i vec_t;

// Simple macro to make a vector lookup table for use with vpshufb. Since
// AVX2 is two 16-byte halves, we duplicate the input values.

#define LOOKUP16(...)   _mm256_setr_epi8(__VA_ARGS__, __VA_ARGS__)

// Move all the bytes in "top" to the left by one and fill in the first byte
// with the last byte in "bottom". Since AVX2 generally works on two separate
// 16-byte vectors glued together, this needs two steps. The permute2x128 takes
// the middle 32 bytes of the 64-byte concatenation bottom:top. The align then
// gives the final result in each half:
//      top half:    top_L:top_H -->    top_L[15]:top_H[0:14]
//   bottom half: bottom_H:top_L --> bottom_H[15]:top_L[0:14]
static inline vec_t v_shift_left(vec_t top, vec_t bottom) {
    vec_t shl_16 = _mm256_permute2x128_si256(top, bottom, 0x03);
    return _mm256_alignr_epi8(top, shl_16, 15);
}

int z_validate_utf8(const char *data, int64_t len) {
    // Error lookup tables for the first, second, and third nibbles
    vec_t error_1 = LOOKUP16(
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x01, 0x00, 0x06, 0x18
    );
    vec_t error_2 = LOOKUP16(
        0x03, 0x01, 0x00, 0x00,
        0x08, 0x10, 0x10, 0x10,
        0x10, 0x10, 0x10, 0x10,
        0x10, 0x14, 0x10, 0x10
    );
    vec_t error_3 = LOOKUP16(
        0x11, 0x11, 0x11, 0x11,
        0x11, 0x11, 0x11, 0x11,
        0x13, 0x1B, 0x1D, 0x1D,
        0x19, 0x19, 0x19, 0x19
    );

    // Mask for table lookup indices
    const vec_t mask_0F = _mm256_set1_epi8(0x0F);

    // Keep continuation bits from the previous iteration that carry over to
    // each input chunk vector
    uint32_t last_cont = 0;

    // We need a vector of the input byte stream shifted forward one byte. Since
    // we don't want to read the memory before the data pointer (which might not
    // even be mapped), so initialize the loop by using vector instructions to
    // shift the byte stream forward.
    vec_t next_bytes = v_shift_left(*(vec_t *)data, _mm256_set1_epi8(0));

    // Loop over input in 32-byte chunks
    for (; len > 0; len -= 32, data += 32) {
        vec_t bytes = *(vec_t *)data;

        // Quick skip for ascii-only input
        uint32_t high = _mm256_movemask_epi8(bytes);
        if (!(high | last_cont))
            continue;

        // Which bytes are required to be continuation bytes
        uint64_t req = last_cont;
        // A bitmask of the actual continuation bytes in the input
        uint32_t cont;

        // Compute the continuation byte mask by finding bytes that start with
        // 11x, 111x, and 1111. For each of these prefixes, we get a bitmask
        // and shift it forward by 1, 2, or 3. This loop should be unrolled by
        // the compiler, and the (n == 1) branch inside eliminated.
        uint64_t set = high;
        for (int n = 1; n <= 3; n++) {
            vec_t bit = _mm256_slli_epi16(bytes, n);
            set &= (uint32_t)_mm256_movemask_epi8(bit);
            // Mark continuation bytes: those that have the high bit set but
            // not the next one
            if (n == 1)
                cont = high ^ set;

            // We add the shifted mask here instead of ORing it, which would
            // be the more natural operation, so that this line can be done
            // with one lea. While adding could give a different result due
            // to carries, this will only happen for invalid UTF-8 sequences,
            // and in a way that won't cause it to pass validation. Reasoning:
            // Any bits for required continuation bytes come after the bits
            // for their leader bytes, and are all contiguous. For a carry to
            // happen, two of these bit sequences would have to overlap. If
            // this is the case, there is a leader byte before the second set
            // of required continuation bytes (and thus before the bit that
            // will be cleared by a carry). This leader byte will not be
            // in the continuation mask, despite being required. QEDish.
            req += set << n;
        }
        // Check that continuation bytes match.
        if (cont != (uint32_t)req)
            return 0;

        // Get a vector of the input bytes shifted forward one byte
        vec_t shifted_bytes = next_bytes;

        // Look up error masks for three consecutive nibbles. We need to
        // AND with 0x0F for each one, because vpshufb has the neat
        // "feature" that negative values in an index byte will result in 
        // a zero.
        vec_t m_1 = _mm256_and_si256(_mm256_srli_epi16(shifted_bytes, 4),
                mask_0F);
        vec_t e_1 = _mm256_shuffle_epi8(error_1, m_1);

        vec_t m_2 = _mm256_and_si256(shifted_bytes, mask_0F);
        vec_t e_2 = _mm256_shuffle_epi8(error_2, m_2);

        vec_t m_3 = _mm256_and_si256(_mm256_srli_epi16(bytes, 4), mask_0F);
        vec_t e_3 = _mm256_shuffle_epi8(error_3, m_3);

        // Check if any bits are set in all three error masks
        if (!_mm256_testz_si256(_mm256_and_si256(e_1, e_2), e_3))
            return 0;

        // Save continuation bits and input bytes for the next round
        last_cont = req >> 32;
        next_bytes = *(vec_t *)(data + 31);
    }

    // The input is valid if we don't have any more expected continuation bytes
    return last_cont == 0;
}