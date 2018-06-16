#include "avx2_gemm.h"

#include <cassert>
#include <emmintrin.h>
#include <immintrin.h>
#include <tmmintrin.h>
#include <xmmintrin.h>
#include <cstdint>

namespace intgemm {
#ifdef __AVX2__

// PREPARE A: just quantization in the same memory order.

namespace {
// Read a vector of floats, multiply them, and cast to 32-bit integer.
inline __m256i QuantizerGrab(const float *input, const __m256 quant_mult_reg) {
  return _mm256_cvtps_epi32(_mm256_mul_ps(_mm256_load_ps(input), quant_mult_reg));
}

inline __m256i QuantizeTile16(const float *input0, const float *input1, __m256 quant_mult_reg) {
  __m256i g0 = QuantizerGrab(input0, quant_mult_reg);
  __m256i g1 = QuantizerGrab(input1, quant_mult_reg);
  __m256i packed = _mm256_packs_epi32(g0, g1);
  // Reorder the packed values because Intel does 0 1 2 3 8 9 10 11 4 5 6 7 12 13 14 15.
  // Technically this could be removed if the PrepareB did the same reordering internally.
  return _mm256_permute4x64_epi64(packed, 0xd8 /* 0, 2, 1, 3 */);
}
} // namespace

// Just quantize everything in order.
void AVX2_16bit::Quantize(const float *input, int16_t *output, float quant_mult, int size) {
  assert(size % 16 == 0);
  assert(reinterpret_cast<uintptr_t>(input) % 32 == 0);
  const __m256 quant_mult_reg = _mm256_set1_ps(quant_mult);
  const float *end = input + size;
  for (; input != end; input += 16, output += 16) {
    *reinterpret_cast<__m256i*>(output) = QuantizeTile16(input, input + 8, quant_mult_reg);
  }
}

namespace {
/* Read 8 floats at a time from input0, input1, input2, and input3.  Quantize
 * them to 8-bit by multiplying with quant_mult_reg then rounding. Concatenate
 * the result into one register and return it.
 */
inline __m256i QuantizeTile8(const float *input0, const float *input1, const float *input2, const float *input3, __m256 quant_mult_reg) {
  // Looking at the assembly, gcc has pulled this outside the loop in Quantize8.
  const __m256i neg127 = _mm256_set1_epi8(-127);
  const __m256i shuffle_param = _mm256_set_epi32(7, 3, 6, 2, 5, 1, 4, 0);
  // Grab 4 registers at a time in 32-bit format.
  __m256i g0 = QuantizerGrab(input0, quant_mult_reg);
  __m256i g1 = QuantizerGrab(input1, quant_mult_reg);
  __m256i g2 = QuantizerGrab(input2, quant_mult_reg);
  __m256i g3 = QuantizerGrab(input3, quant_mult_reg);
  // Pack 32-bit to 16-bit.
  __m256i packed0 = _mm256_packs_epi32(g0, g1);
  __m256i packed1 = _mm256_packs_epi32(g2, g3);
  // Pack 16-bit to 8-bit.
  __m256i packed = _mm256_packs_epi16(packed0, packed1);
  // Ban -128.
  packed = _mm256_max_epi8(packed, neg127);
  // Currently in 0 1 2 3 8 9 10 11 16 17 18 19 24 25 26 27 4 5 6 7 12 13 14 15 20 21 22 23 28 29 30 31
  // Or as 32-bit integers 0 2 4 6 1 3 5 7
  // Technically this could be removed so long as the rows are bigger than 16
  // and the values are only used for GEMM.
  return _mm256_permutevar8x32_epi32(packed, shuffle_param);
}
} // namespace

// Just quantize everything in order.
void AVX2_8bit::Quantize(const float *input, int8_t *output, float quant_mult, int size) {
  assert(size % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(input) % 32 == 0);
  const __m256 quant_mult_reg = _mm256_set1_ps(quant_mult);
  const float *end = input + size;
  for (; input != end; input += 32, output += 32) {
    *reinterpret_cast<__m256i*>(output) = QuantizeTile8(input, input + 8, input + 16, input + 24, quant_mult_reg);
  }
}

// PREPARE B: quantize and rearrange.  B is presumed to be constantparameters
// so we can take our time rearranging it in order to save during the multiply.
//
// We presume B starts in row-major order.
//
// A register holds 32 8-bit values or 16 16-bit values and we want that many
// values from the same column in the register.
//
// The multiplier reads 8 rows at a time and we want these reads to be
// contiguous.
//
// Each 8x32 (for 8-bit) or 8x16 (for 16-bit) tile of B is transposed.
// The tiles are stored in column major order.
//
// This matrix shows what index each value of B will be stored at:
//   0  16 ... 240
//   1  17 ... 241
//   2  18 ... 242
//   3  19 ... 243
//   4  20 ... 244
//   5  21 ... 245
//   6  22 ... 246
//   7  23 ... 247
//   8  24 ... 248
//   9  25 ... 249
//  10  26 ... 250
//  11  27 ... 251
//  12  28 ... 252
//  13  29 ... 253
//  14  30 ... 254
//  15  31 ... 255
// 256 272
// 257 273
// ... ...
namespace {

// Input: 8-bit integers
// first  f0 f1 f2 f3 f4 f5 f6 f7 f8 f9 f10 f11 f12 f13 f14 f15 f16 f17 f18 f19 f20 f21 f22 f23 f24 f25 f26 f27 f28 f29 f30 f31
// second s0 s1 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 s12 s13 s14 s15 s16 s17 s18 s19 s20 s21 s22 s23 s24 s25 s26 s27 s28 s29 s30 s31
// Output:
// first  [f0 s0 f1 s1 f2 s2 f3 s3 f4 s4 f5 s5 f6 s6 f7 s7] [f16 s16 f17 s17 f18 s18 f19 s19 f20 s20 f21 s21 f22 s22 f23 s23]
// second [f8 s8 f9 s9 f10 s10 f11 s11 f12 s12 f13 s13 f14 s14 f15 s15] [f24 s24 f25 s25 f26 s26 f27 s27 f28 s28 f29 s29 f30 s30 f31 s31]
inline void Interleave8(__m256i &first, __m256i &second) {
  __m256i temp = _mm256_unpacklo_epi8(first, second);
  second = _mm256_unpackhi_epi8(first, second);
  first = temp;
}
// Same but move 16-bit integers.
inline void Interleave16(__m256i &first, __m256i &second) {
  __m256i temp = _mm256_unpacklo_epi16(first, second);
  second = _mm256_unpackhi_epi16(first, second);
  first = temp;
}
// Same but move 32-bit integers.
inline void Interleave32(__m256i &first, __m256i &second) {
  __m256i temp = _mm256_unpacklo_epi32(first, second);
  second = _mm256_unpackhi_epi32(first, second);
  first = temp;
}
// Same but move 64-bit integers.
inline void Interleave64(__m256i &first, __m256i &second) {
  __m256i temp = _mm256_unpacklo_epi64(first, second);
  second = _mm256_unpackhi_epi64(first, second);
  first = temp;
}

inline void ReshapeToFours16(const float *input, __m256 quant_mult_reg, int cols,  __m256i &out0, __m256i &out1) {
  out0 = QuantizeTile16(input,            input + 8 * cols, quant_mult_reg);
  out1 = QuantizeTile16(input + 1 * cols, input + 9 * cols, quant_mult_reg);
  Interleave16(out0, out1);
  // out0:
  // [0,0,1,1,2,2,3,3] [0,0,1,1,2,2,3,3]
  // out1:
  // [4,4,5,5,6,6,7,7] [4,4,5,5,6,6,7,7]
}

inline void ReshapeToEights16(const float *input, __m256 quant_mult_reg, int cols, __m256i &out0, __m256i &out1, __m256i &out2, __m256i &out3) {
  ReshapeToFours16(input, quant_mult_reg, cols, out0, out2);
  ReshapeToFours16(input + 2 * cols, quant_mult_reg, cols, out1, out3);
  Interleave32(out0, out1);
  Interleave32(out2, out3);
  // out0: 64-bit [0,1] from rows 0-3 [0,1] from rows 8-11
  // out1: 64-bit [2,3] from rows 0-3 [2,3] from rows 8-11
  // out2: 64-bit [5,6] from rows 0-3 [5,6] from rows 8-11
  // out3: 64-bit [7,8] from rows 0-3 [7,8] from rows 8-11
}

} // namespace

void AVX2_16bit::PrepareB(const float *input, int16_t *output_shadow, float quant_mult, int rows, int cols) {
  assert(rows % 16 == 0);
  assert(cols % 8 == 0);
  assert(reinterpret_cast<uintptr_t>(input) % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(output_shadow) % 32 == 0);
  __m256i *output = reinterpret_cast<__m256i*>(output_shadow);
  const __m256 quant_mult_reg = _mm256_set1_ps(quant_mult);
  for (int c = 0; c < cols; c += 8) {
    for (int r = 0; r < rows; r += 16, output += 8) {
      ReshapeToEights16(input + r * cols + c,       quant_mult_reg, cols, output[0], output[2], output[4], output[6]);
      ReshapeToEights16(input + (r + 4) * cols + c, quant_mult_reg, cols, output[1], output[3], output[5], output[7]);
      Interleave64(output[0], output[1]);
      Interleave64(output[2], output[3]);
      Interleave64(output[4], output[5]);
      Interleave64(output[6], output[7]);
    }
  }
}

namespace {

inline void ReshapeToFours8(const float *input, __m256 quant_mult_reg, int cols, __m256i &out0, __m256i &out1) {
  // This generates bytes [0,1,2,3,4,6,7] [0,1,2,3,4,6,7] [0,1,2,3,4,6,7] [0,1,2,3,4,6,7] where each is from a different row.
  // We keep higher rows in the second half of the register to avoid a shuffle later.
  out0 = QuantizeTile8(input,            input + 2 * cols, input + 16 * cols, input + 18 * cols, quant_mult_reg);
  out1 = QuantizeTile8(input + 1 * cols, input + 3 * cols, input + 17 * cols, input + 19 * cols, quant_mult_reg);
  // This does [0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7] [0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7] where consecutive values are from consecutive rows
  Interleave8(out0, out1);
  Interleave16(out0, out1);
  // out0:
  // [0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3] [0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3]
  // Rows 0, 1, 2, and 3 in first 128; rows 16, 17, 18, and 19 in last 128
  // out1:
  // [4,4,4,4,5,5,5,5,6,6,6,6,7,7,7,7] [4,4,4,4,5,5,5,5,6,6,6,6,7,7,7,7]
  // Or as 32-bit blocks: [4,5,6,7] [4,5,6,7]
}

inline void ReshapeToEights8(const float *input, __m256 quant_mult_reg, int cols, __m256i &out0, __m256i &out1, __m256i &out2, __m256i &out3) {
  // Get 32-bit blocks:
  ReshapeToFours8(input, quant_mult_reg, cols, out0, out2);
  // out0: [0,1,2,3] from rows 0-3 [0,1,2,3] from rows 16-19
  // out2: [5,6,7,8] from rows 0-3 [5,6,7,8] from rows 16-19
  ReshapeToFours8(input + 4 * cols, quant_mult_reg, cols, out1, out3);
  // out1: [0,1,2,3] from rows 4-7 [0,1,2,3] from rows 20-23
  // out3: [5,6,7,8] from rows 4-7 [5,6,7,8] from rows 20-23
  Interleave32(out0, out1);
  Interleave32(out2, out3);
  // out0: 64-bit [0,1] from rows 0-7 [0,1] from rows 16-23
  // out1: 64-bit [2,3] from rows 0-7 [2,3] from rows 16-23
  // out2: 64-bit [5,6] from rows 0-7 [5,6] from rows 16-23
  // out3: 64-bit [7,8] from rows 0-7 [7,8] from rows 16-23
}

} // namespace

void AVX2_8bit::PrepareB(const float *input, int8_t *output_shadow, float quant_mult, int rows, int cols) {
  assert(rows % 32 == 0);
  assert(cols % 8 == 0);
  assert(reinterpret_cast<uintptr_t>(input) % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(output_shadow) % 32 == 0);
  __m256i *output = reinterpret_cast<__m256i*>(output_shadow);
  const __m256 quant_mult_reg = _mm256_set1_ps(quant_mult);
  for (int c = 0; c < cols; c += 8) {
    for (int r = 0; r < rows; r += 32, output += 8) {
      ReshapeToEights8(input + r * cols + c,       quant_mult_reg, cols, output[0], output[2], output[4], output[6]);
      ReshapeToEights8(input + (r + 8) * cols + c, quant_mult_reg, cols, output[1], output[3], output[5], output[7]);
      Interleave64(output[0], output[1]);
      Interleave64(output[2], output[3]);
      Interleave64(output[4], output[5]);
      Interleave64(output[6], output[7]);
    }
  }
}

namespace {
/* Again just a shorter version of AVX512.  TODO: test shift and friends.  Or _mm256_hadds_epi16 */
inline void Convert32Sum(__m256i &sum) {
  sum = _mm256_madd_epi16(sum, _mm256_set1_epi16(1) /* Empirically gcc is smart enough to pull this out */);
}

/* Take 4 registers with 32-bit values to be horizontally added.  Reduce them
 * to one register with 32-bit values in the pattern 1 2 3 4 1 2 3 4, leaving
 * the final addition (which crosses 128-bit lanes) to the caller. */
inline __m256i Pack1234(__m256i sum1, __m256i sum2, __m256i sum3, __m256i sum4) {
  // 1 2 1 2 1 2 1 2
  __m256i pack12 = _mm256_add_epi32(_mm256_unpackhi_epi32(sum1, sum2), _mm256_unpacklo_epi32(sum1, sum2));
  // 3 4 3 4 3 4 3 4
  __m256i pack34 = _mm256_add_epi32(_mm256_unpackhi_epi32(sum3, sum4), _mm256_unpacklo_epi32(sum3, sum4));
  // 1 2 3 4 1 2 3 4
  return _mm256_add_epi32(_mm256_unpackhi_epi64(pack12, pack34), _mm256_unpacklo_epi64(pack12, pack34));
}

// Assuming sum1, sum2, sum3, sum4, sum5, sum6, and sum7 are arrays 32-bit
// signed integers, reduce within each.
// Returns [sum(sum1), sum(sum2), sum(sum3), sum(sum4), sum(sum5), sum(sum6), sum(sum7), sum(sum8)]
// TODO: consider doing in 64-bit, allowing more bits of quantization?
inline __m256i Reduce32(__m256i sum1, __m256i sum2, __m256i sum3, __m256i sum4, __m256i sum5, __m256i sum6, __m256i sum7, __m256i sum8) {
  __m256i pack1234 = Pack1234(sum1, sum2, sum3, sum4);
  __m256i pack5678 = Pack1234(sum5, sum6, sum7, sum8);
  // Introducing "f" for first half and "s" for second half, we have this order:
  // pack1234 = 1f 2f 3f 4f 1s 2s 3s 4s
  // pack5678 = 5f 6f 7f 8f 5s 6s 7s 8s
  // This instruction generates 1s 2s 3s 4s 5f 6f 7f 8f
  __m256i rev = _mm256_permute2f128_si256(pack1234, pack5678, 0x21);
  // This instruction generates 1f 2f 3f 4f 5s 6s 7s 8s
  __m256i blended = _mm256_blend_epi32(pack1234, pack5678, 0xf0);
  return _mm256_add_epi32(rev, blended);
}

inline __m256i Reduce16to32(__m256i sum1, __m256i sum2, __m256i sum3, __m256i sum4, __m256i sum5, __m256i sum6, __m256i sum7, __m256i sum8) {
  Convert32Sum(sum1);
  Convert32Sum(sum2);
  Convert32Sum(sum3);
  Convert32Sum(sum4);
  Convert32Sum(sum5);
  Convert32Sum(sum6);
  Convert32Sum(sum7);
  Convert32Sum(sum8);
  return Reduce32(sum1, sum2, sum3, sum4, sum5, sum6, sum7, sum8);
}

} // namespace

// This is an AVX2 implementation of int16_t multiply based on Jacob
// Devlin's SSE code.  The original SSE code was:

// Copyright (c) 2017 Microsoft Corporation

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// A is a row-major quantized matrix with 32-byte alignment (see PrepareA)
// B is a rearranged quantized matrix with 32-byte alignment (see PrepareB)
// C is output in row-major form.
// C = A * B * unquant_mult
void AVX2_16bit::Multiply(const int16_t *A, const int16_t *B, float *C, float unquant_mult, int A_rows, int width, int B_cols) {
  assert(width % 16 == 0);
  assert(B_cols % 8 == 0);
  assert(reinterpret_cast<uintptr_t>(A) % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(B) % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(C) % 32 == 0);
  const int simd_width = width / 16;
  const __m256 unquant_reg = _mm256_set1_ps(unquant_mult);
  const __m256i *B0_col = reinterpret_cast<const __m256i *>(B);
  for (int B0_colidx = 0; B0_colidx < B_cols; B0_col += 8 * simd_width, B0_colidx += 8) {
    // Process one row of A at a time.  Doesn't seem to be faster to do multiple rows of A at once.
    for (int A_rowidx = 0; A_rowidx < A_rows; ++A_rowidx) {
      const __m256i *A_row = reinterpret_cast<const __m256i*>(A + A_rowidx * width);
      // These will be packed 32-bit integers containing sums for each row of B multiplied by the row of A.
      __m256i sum0 = _mm256_setzero_si256();
      __m256i sum1 = _mm256_setzero_si256();
      __m256i sum2 = _mm256_setzero_si256();
      __m256i sum3 = _mm256_setzero_si256();
      __m256i sum4 = _mm256_setzero_si256();
      __m256i sum5 = _mm256_setzero_si256();
      __m256i sum6 = _mm256_setzero_si256();
      __m256i sum7 = _mm256_setzero_si256();
      // Iterate over shared (inner) dimension.
      for (int k = 0; k < simd_width; ++k) {
        __m256i a = *(A_row + k);
        // Multiply 16-bit, horizontally add to packed 32-bit integers.
        __m256i mult0 = _mm256_madd_epi16(a, *(B0_col + k * 8));
        __m256i mult1 = _mm256_madd_epi16(a, *(B0_col + k * 8 + 1));
        __m256i mult2 = _mm256_madd_epi16(a, *(B0_col + k * 8 + 2));
        __m256i mult3 = _mm256_madd_epi16(a, *(B0_col + k * 8 + 3));
        __m256i mult4 = _mm256_madd_epi16(a, *(B0_col + k * 8 + 4));
        __m256i mult5 = _mm256_madd_epi16(a, *(B0_col + k * 8 + 5));
        __m256i mult6 = _mm256_madd_epi16(a, *(B0_col + k * 8 + 6));
        __m256i mult7 = _mm256_madd_epi16(a, *(B0_col + k * 8 + 7));
        // Sum packed 32-bit integers with danger of overflow.  TODO: accumulate in 64-bit every so often.
        sum0 = _mm256_add_epi32(sum0, mult0);
        sum1 = _mm256_add_epi32(sum1, mult1);
        sum2 = _mm256_add_epi32(sum2, mult2);
        sum3 = _mm256_add_epi32(sum3, mult3);
        sum4 = _mm256_add_epi32(sum4, mult4);
        sum5 = _mm256_add_epi32(sum5, mult5);
        sum6 = _mm256_add_epi32(sum6, mult6);
        sum7 = _mm256_add_epi32(sum7, mult7);
      }
      // Write to C.
      __m256i combined = Reduce32(sum0, sum1, sum2, sum3, sum4, sum5, sum6, sum7);
      *reinterpret_cast<__m256*>(C + A_rowidx * B_cols + B0_colidx) = _mm256_mul_ps(_mm256_cvtepi32_ps(combined), unquant_reg);
    }
  }
}

/* This is the C version of the below */
/*
void AVX2_8Bit::Multiply(const __m256i *A, const __m256i *B, float *C, float unquant_mult, int A_rows, int width, int B_cols) {
  assert(width % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(A) % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(B) % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(C) % 32 == 0);
  assert(num_B_rows % 8 == 0);
  __m256 unquant_reg = _mm256_set1_ps(unquant_mult);
  const int simd_width = width / 32;
  int B0_rowidx = 0;
  // Go over 8 rows of B at a time.  TODO: rearrange B so that these accesses are adjacent (it's faster).
  for (const __m256i *B0_row = B; B0_rowidx != num_B_rows; B0_row += 8 * simd_width, B0_rowidx += 8) {
    // Process one row of A at a time.  Doesn't seem to be faster to do multiple rows of A at once.
    for (int A_rowidx = 0; A_rowidx < A_rows; ++A_rowidx) {
      const __m256i *A_row = A + A_rowidx * simd_width;
      // These will be packed 16-bit integers containing sums for each row of B multiplied by the row of A.
      __m256i sum0 = _mm256_setzero_si256();
      __m256i sum1 = _mm256_setzero_si256();
      __m256i sum2 = _mm256_setzero_si256();
      __m256i sum3 = _mm256_setzero_si256();
      __m256i sum4 = _mm256_setzero_si256();
      __m256i sum5 = _mm256_setzero_si256();
      __m256i sum6 = _mm256_setzero_si256();
      __m256i sum7 = _mm256_setzero_si256();
      // Iterate over shared (inner) dimension.
      for (int k = 0; k < simd_width; ++k) {
        // Read in 64 8-bit signed integers from A.
        __m256i a = *(A_row + k);
        // Negate 8-bit values in b if the corresponding a was negative.
        // Negation is implemented by subtraction from zero.
        __m256i b0 = _mm256_sign_epi8(*(B0_row + k * 8), a);
        __m256i b1 = _mm256_sign_epi8(*(B0_row + k * 8 + 1), a);
        __m256i b2 = _mm256_sign_epi8(*(B0_row + k * 8 + 2), a);
        __m256i b3 = _mm256_sign_epi8(*(B0_row + k * 8 + 3), a);
        __m256i b4 = _mm256_sign_epi8(*(B0_row + k * 8 + 4), a);
        __m256i b5 = _mm256_sign_epi8(*(B0_row + k * 8 + 5), a);
        __m256i b6 = _mm256_sign_epi8(*(B0_row + k * 8 + 6), a);
        __m256i b7 = _mm256_sign_epi8(*(B0_row + k * 8 + 7), a);
        __m256i a_positive = _mm256_abs_epi8(a);
        // Multiply 8-bit unsigned * signed, horizontally add to packed 16-bit integers.
        __m256i mult0 = _mm256_maddubs_epi16(a_positive, b0);
        __m256i mult1 = _mm256_maddubs_epi16(a_positive, b1);
        __m256i mult2 = _mm256_maddubs_epi16(a_positive, b2);
        __m256i mult3 = _mm256_maddubs_epi16(a_positive, b3);
        __m256i mult4 = _mm256_maddubs_epi16(a_positive, b4);
        __m256i mult5 = _mm256_maddubs_epi16(a_positive, b5);
        __m256i mult6 = _mm256_maddubs_epi16(a_positive, b6);
        __m256i mult7 = _mm256_maddubs_epi16(a_positive, b7);
        // Sum packed 16-bit integers with saturation.
        // With larger matrices there is a danger of saturating so TODO upcast to 32-bit every so often.
        sum0 = _mm256_adds_epi16(mult0, sum0);
        sum1 = _mm256_adds_epi16(mult1, sum1);
        sum2 = _mm256_adds_epi16(mult2, sum2);
        sum3 = _mm256_adds_epi16(mult3, sum3);
        sum4 = _mm256_adds_epi16(mult4, sum4);
        sum5 = _mm256_adds_epi16(mult5, sum5);
        sum6 = _mm256_adds_epi16(mult6, sum6);
        sum7 = _mm256_adds_epi16(mult7, sum7);
      }
      // Write to C.
      __m256i combined = Reduce16to32(sum0, sum1, sum2, sum3, sum4, sum5, sum6, sum7);
      *reinterpret_cast<__m256*>(C + A_rowidx * num_B_rows + B0_rowidx) = _mm256_mul_ps(_mm256_cvtepi32_ps(combined), unquant_reg);
    }
  }
}
*/

void AVX2_8bit::Multiply(const int8_t *A, const int8_t *B, float *C, float unquant_mult, int A_rows, int width, int B_cols) {
  assert(width % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(A) % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(B) % 32 == 0);
  assert(reinterpret_cast<uintptr_t>(C) % 32 == 0);
  assert(B_cols % 8 == 0);
  __m256 unquant_reg = _mm256_set1_ps(unquant_mult);
  const int simd_width = width / 32;
  int B0_colidx = 0;
  // Go over 8 columns of B at a time.
  for (const __m256i *B0_col = reinterpret_cast<const __m256i*>(B); B0_colidx != B_cols; B0_col += 8 * simd_width, B0_colidx += 8) {
    // Process one row of A at a time.  Doesn't seem to be faster to do multiple rows of A at once.
    for (int A_rowidx = 0; A_rowidx < A_rows; ++A_rowidx) {
      const __m256i *A_row = reinterpret_cast<const __m256i *>(A + A_rowidx * width);
      // These will be packed 16-bit integers containing sums for each column of B multiplied by the row of A.
      __m256i sum0 = _mm256_setzero_si256();
      __m256i sum1 = _mm256_setzero_si256();
      __m256i sum2 = _mm256_setzero_si256();
      __m256i sum3 = _mm256_setzero_si256();
      __m256i sum4 = _mm256_setzero_si256();
      __m256i sum5 = _mm256_setzero_si256();
      __m256i sum6 = _mm256_setzero_si256();
      __m256i sum7 = _mm256_setzero_si256();
      // Iterate over shared (inner) dimension.
      for (int k = 0; k < simd_width; ++k) {
        const __m256i *B_base = B0_col + k * 8;
        // Read in 64 8-bit signed integers from A.
        __m256i a = *(A_row + k);
        // The assembly will store the absolute value of a here.
        __m256i absa;
        // Annoyingly the only 8-bit multiply is signed * unsigned (maddubs).
        // So we take the sign bits off of a and apply them each b in a * b.
        //
        // We have only 16 YMM registers but we want to store:
        // 1 for a (or |a|)
        // 8 temporaries for applying sign to each column of B.
        // 8 sums.
        //
        // gcc's register allocator does:
        // 1 for a, do all the sign application, then overwrite with |a|
        // 8 temporaries
        // 7 sums in registers + 1 on the stack
        //
        // But it's possible to complete an operation early, freeing up its
        // temporary register for reuse.  But completing an operation early
        // requires us to have |a| for vpmaddubsw while completing the later
        // operation needs a again to apply sign.
        //
        // So we do two columns, 0 and 1, early.  This allows b0_b6 and b1_b7
        // to be reused by columns 6 and 7, respectively.  And there's enough
        // registers to store both a and |a|.
        //
        // These are the temporary variables used to process each column of b.
        // We let the compiler choose which register number is which, but force
        // it to allocate all registers.
        __m256i b0_b6, b1_b7, b2, b3, b4, b5;
        asm(
            // Copy the first 6 columns of b to registers.  We assume B has
            // been rearranged so that these 8 columns are consecutive.
            // vpsignb does not take a memory address as its second argument,
            // so this can't be inlined into vsignb.
            "vmovdqa    (%[B]), %[b0_b6];\n"
            "vmovdqa  32(%[B]), %[b1_b7];\n"
            "vmovdqa  64(%[B]), %[b2];\n"
            "vmovdqa  96(%[B]), %[b3];\n"
            "vmovdqa 128(%[B]), %[b4];\n"
            "vmovdqa 160(%[B]), %[b5];\n"
            // Store the absolute value of a in absa.
            "vpabsb  %[a], %[absa];\n"
            // If a byte of a is negative, negate the corresponding byte in
            // b0_b6 etc.
            "vpsignb %[a], %[b0_b6], %[b0_b6];\n"
            "vpsignb %[a], %[b1_b7], %[b1_b7];\n"
            // Multiply signed * unsigned then horizontally add to form packed
            // 16-bit integers:
            // b0[0] * |a|[0] + b0[1] * |a|[1], b0[2] * |a|[2] + b0[3] * |a|[3], ...
            "vpmaddubsw %[b0_b6], %[absa], %[b0_b6];\n"
            "vpmaddubsw %[b1_b7], %[absa], %[b1_b7];\n"
            // vpmaddubsw has latency 5 so work on some other sign bits while
            // we're at it.
            "vpsignb %[a], %[b2], %[b2];\n"
            "vpsignb %[a], %[b3], %[b3];\n"
            "vpsignb %[a], %[b4], %[b4];\n"
            "vpsignb %[a], %[b5], %[b5];\n"
            // Perform a 16-bit add with saturation to accumlate sums.
            "vpaddsw %[b0_b6], %[sum0], %[sum0];\n"
            // Now we can reuse b0_b6 for b6
            "vmovdqa 192(%[B]), %[b0_b6];\n"
            "vpaddsw %[b1_b7], %[sum1], %[sum1];\n"
            // Now we can reuse b1_b7 for b7
            "vmovdqa 224(%[B]), %[b1_b7];\n"
            // More crunching while the load happens.
            "vpmaddubsw %[b2], %[absa], %[b2];\n"
            "vpmaddubsw %[b3], %[absa], %[b3];\n"
            "vpmaddubsw %[b4], %[absa], %[b4];\n"
            "vpsignb %[a], %[b0_b6], %[b0_b6];\n"
            "vpsignb %[a], %[b1_b7], %[b1_b7];\n"
            "vpmaddubsw %[b5], %[absa], %[b5];\n"
            "vpmaddubsw %[b0_b6], %[absa], %[b0_b6];\n"
            "vpmaddubsw %[b1_b7], %[absa], %[b1_b7];\n"
            "vpaddsw %[b2], %[sum2], %[sum2];\n"
            "vpaddsw %[b3], %[sum3], %[sum3];\n"
            "vpaddsw %[b4], %[sum4], %[sum4];\n"
            "vpaddsw %[b5], %[sum5], %[sum5];\n"
            "vpaddsw %[b0_b6], %[sum6], %[sum6];\n"
            "vpaddsw %[b1_b7], %[sum7], %[sum7];\n"
            : [sum0] "+x" (sum0),
              [sum1] "+x" (sum1),
              [sum2] "+x" (sum2),
              [sum3] "+x" (sum3),
              [sum4] "+x" (sum4),
              [sum5] "+x" (sum5),
              [sum6] "+x" (sum6),
              [sum7] "+x" (sum7),
              [b0_b6] "=&x" (b0_b6),
              [b1_b7] "=&x" (b1_b7),
              [b2] "=&x" (b2),
              [b3] "=&x" (b3),
              [b4] "=&x" (b4),
              [b5] "=&x" (b5),
              [absa] "=&x" (absa)
              // Tell gcc precisely what we are reading from RAM.
            : [B] "r" (*reinterpret_cast<const __m256i (*)[8]>(B_base)),
              [a] "x" (a)
           );
      }
      // Write to C.
      __m256i combined = Reduce16to32(sum0, sum1, sum2, sum3, sum4, sum5, sum6, sum7);
      *reinterpret_cast<__m256*>(C + A_rowidx * B_cols + B0_colidx) = _mm256_mul_ps(_mm256_cvtepi32_ps(combined), unquant_reg);
    }
  }
}


#endif // __AVX2__
} // namespace intgemm