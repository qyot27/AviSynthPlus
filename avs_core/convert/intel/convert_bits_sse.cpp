// Avisynth v2.5.  Copyright 2002-2009 Ben Rudiak-Gould et al.
// http://www.avisynth.org

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA, or visit
// http://www.gnu.org/copyleft/gpl.html .
//
// Linking Avisynth statically or dynamically with other modules is making a
// combined work based on Avisynth.  Thus, the terms and conditions of the GNU
// General Public License cover the whole combination.
//
// As a special exception, the copyright holders of Avisynth give you
// permission to link Avisynth with independent modules that communicate with
// Avisynth solely through the interfaces defined in avisynth.h, regardless of the license
// terms of these independent modules, and to copy and distribute the
// resulting combined work under terms of your choice, provided that
// every copy of the combined work is accompanied by a complete copy of
// the source code of Avisynth (the version of Avisynth used to produce the
// combined work), being distributed under the terms of the GNU General
// Public License plus this exception.  An independent module is a module
// which is not derived from or based on Avisynth, such as 3rd-party filters,
// import and export plugins, or graphical user interfaces.

#include "../convert_bits.h"
#include "convert_bits_sse.h"

#include <avs/alignment.h>
#include <avs/minmax.h>
#include <avs/config.h>
#include <tuple>
#include <map>
#include <algorithm>

#ifdef AVS_WINDOWS
#include <avs/win.h>
#else
#include <avs/posix.h>
#endif

#include <emmintrin.h>
#include <smmintrin.h> // SSE4.1




template<uint8_t sourcebits, int dither_mode, int TARGET_DITHER_BITDEPTH, int rgb_step>
void convert_rgb_uint16_to_8_sse2(const BYTE *srcp, BYTE *dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth)
{
  const uint16_t *srcp0 = reinterpret_cast<const uint16_t *>(srcp);
  src_pitch = src_pitch / sizeof(uint16_t);
  int src_width = src_rowsize / sizeof(uint16_t);

  int _y = 0; // for ordered dither

  const int TARGET_BITDEPTH = 8; // here is constant (uint8_t target)

  // for test, make it 2,4,6,8. sourcebits-TARGET_DITHER_BITDEPTH cannot exceed 8 bit
  // const int TARGET_DITHER_BITDEPTH = 2;

  const int max_pixel_value_dithered = (1 << TARGET_DITHER_BITDEPTH) - 1;
  // precheck ensures:
  // TARGET_BITDEPTH >= TARGET_DITHER_BITDEPTH
  // sourcebits - TARGET_DITHER_BITDEPTH <= 8
  // sourcebits - TARGET_DITHER_BITDEPTH is even (later we can use PRESHIFT)
  const int DITHER_BIT_DIFF = (sourcebits - TARGET_DITHER_BITDEPTH); // 2, 4, 6, 8
  const int PRESHIFT = DITHER_BIT_DIFF & 1;  // 0 or 1: correction for odd bit differences (not used here but generality)
  const int DITHER_ORDER = (DITHER_BIT_DIFF + PRESHIFT) / 2;
  const int DITHER_SIZE = 1 << DITHER_ORDER; // 9,10=2  11,12=4  13,14=8  15,16=16
  const int MASK = DITHER_SIZE - 1;
  // 10->8: 0x01 (2x2)
  // 11->8: 0x03 (4x4)
  // 12->8: 0x03 (4x4)
  // 14->8: 0x07 (8x8)
  // 16->8: 0x0F (16x16)
  const BYTE *matrix;
  switch (sourcebits - TARGET_DITHER_BITDEPTH) {
  case 2: matrix = reinterpret_cast<const BYTE *>(dither2x2.data); break;
  case 4: matrix = reinterpret_cast<const BYTE *>(dither4x4.data); break;
  case 6: matrix = reinterpret_cast<const BYTE *>(dither8x8.data); break;
  case 8: matrix = reinterpret_cast<const BYTE *>(dither16x16.data); break;
  default: return; // n/a
  }

  // 20171024: given up integer division, rounding problems
  const float mulfactor =
    sourcebits == 16 ? (1.0f / 257.0f) :
    sourcebits == 14 ? (255.0f / 16383.0f) :
    sourcebits == 12 ? (255.0f / 4095.0f) :
    (255.0f / 1023.0f); // 10 bits
  const __m128 mulfactor_simd = _mm_set1_ps(mulfactor);
  const __m128i zero = _mm_setzero_si128();

  for (int y = 0; y < src_height; y++)
  {
    if constexpr(dither_mode == 0)
      _y = (y & MASK) << DITHER_ORDER; // ordered dither
    for (int x = 0; x < src_width; x += 8) // 8 * uint16_t at a time
    {
      if constexpr(dither_mode < 0) // -1: no dither
      {

        // C: dstp[x] = (uint8_t)(srcp0[x] * mulfactor + 0.5f);
        // C cast truncates, use +0.5f rounder, which uses cvttss2si

        __m128i pixel_i = _mm_load_si128(reinterpret_cast<const __m128i *>(srcp0 + x)); // 16 bytes 8 pixels

        __m128 pixel_f_lo = _mm_cvtepi32_ps(_mm_unpacklo_epi16(pixel_i, zero)); // 4 floats
        __m128 mulled_lo = _mm_mul_ps(pixel_f_lo, mulfactor_simd);
        __m128i converted32_lo = _mm_cvtps_epi32(mulled_lo); // rounding ok, nearest. no +0.5 needed

        __m128 pixel_f_hi = _mm_cvtepi32_ps(_mm_unpackhi_epi16(pixel_i, zero)); // 4 floats
        __m128 mulled_hi = _mm_mul_ps(pixel_f_hi, mulfactor_simd);
        __m128i converted32_hi = _mm_cvtps_epi32(mulled_hi);

        __m128i converted_16 = _mm_packs_epi32(converted32_lo, converted32_hi);
        __m128i converted_8 = _mm_packus_epi16(converted_16, zero);
        _mm_storel_epi64(reinterpret_cast<__m128i *>(&dstp[x]), converted_8); // store 8 bytes
      }
      else { // dither_mode == 0 -> ordered dither
             //  const int corr = matrix[_y | ((x / rgb_step) & MASK)];
        __m128i corr_lo = _mm_set_epi32(
          matrix[_y | (((x + 3) / rgb_step) & MASK)],
          matrix[_y | (((x + 2) / rgb_step) & MASK)],
          matrix[_y | (((x + 1) / rgb_step) & MASK)],
          matrix[_y | (((x + 0) / rgb_step) & MASK)]
        );
        __m128i corr_hi = _mm_set_epi32(
          matrix[_y | (((x + 7) / rgb_step) & MASK)],
          matrix[_y | (((x + 6) / rgb_step) & MASK)],
          matrix[_y | (((x + 5) / rgb_step) & MASK)],
          matrix[_y | (((x + 4) / rgb_step) & MASK)]
        );
        // vvv for the non-fullscale version: int new_pixel = ((srcp0[x] + corr) >> DITHER_BIT_DIFF);

        // no integer division, rounding problems
        const float mulfactor_dith =
          DITHER_BIT_DIFF == 8 ? (1.0f / 257.0f) :
          DITHER_BIT_DIFF == 6 ? (255.0f / 16383.0f) :
          DITHER_BIT_DIFF == 4 ? (255.0f / 4095.0f) :
          DITHER_BIT_DIFF == 2 ? (255.0f / 1023.0f) :
          1.0f;
        __m128 mulfactor_dith_simd = _mm_set1_ps(mulfactor_dith);

        __m128i pixel_i = _mm_load_si128(reinterpret_cast<const __m128i *>(srcp0 + x)); // 16 bytes 8 pixels
        __m128i pixel_i_lo = _mm_add_epi32(_mm_unpacklo_epi16(pixel_i, zero), corr_lo);
        __m128i pixel_i_hi = _mm_add_epi32(_mm_unpackhi_epi16(pixel_i, zero), corr_hi);
        __m128i converted32_lo, converted32_hi;

        /* C:
        if (TARGET_DITHER_BITDEPTH <= 4)
        new_pixel = (uint16_t)((srcp0[x] + corr) * mulfactor); // rounding here makes brightness shift
        else if (DITHER_BIT_DIFF > 0)
        new_pixel = (uint16_t)((srcp0[x] + corr) * mulfactor + 0.5f);
        else
        new_pixel = (uint16_t)(srcp0[x] + corr);
        */
        if constexpr(TARGET_DITHER_BITDEPTH <= 4) {
          // round: truncate
          __m128 pixel_f_lo = _mm_cvtepi32_ps(pixel_i_lo); // 4 floats
          __m128 mulled_lo = _mm_mul_ps(pixel_f_lo, mulfactor_dith_simd);
          converted32_lo = _mm_cvttps_epi32(mulled_lo); // truncate! rounding here makes brightness shift

          __m128 pixel_f_hi = _mm_cvtepi32_ps(pixel_i_hi); // 4 floats
          __m128 mulled_hi = _mm_mul_ps(pixel_f_hi, mulfactor_dith_simd);
          converted32_hi = _mm_cvttps_epi32(mulled_hi); // truncate! rounding here makes brightness shift
        }
        else if constexpr(DITHER_BIT_DIFF > 0) {
          // round: nearest
          __m128 pixel_f_lo = _mm_cvtepi32_ps(pixel_i_lo); // 4 floats
          __m128 mulled_lo = _mm_mul_ps(pixel_f_lo, mulfactor_dith_simd);
          converted32_lo = _mm_cvtps_epi32(mulled_lo); // rounding ok, nearest. no +0.5 needed

          __m128 pixel_f_hi = _mm_cvtepi32_ps(pixel_i_hi); // 4 floats
          __m128 mulled_hi = _mm_mul_ps(pixel_f_hi, mulfactor_dith_simd);
          converted32_hi = _mm_cvtps_epi32(mulled_hi);
        }
        else {
          // new_pixel = (uint8_t)(srcp0[x] + corr);
          converted32_lo = pixel_i_lo;
          converted32_hi = pixel_i_hi;
        }

        __m128i converted_16 = _mm_packs_epi32(converted32_lo, converted32_hi);
        if constexpr(max_pixel_value_dithered <= 16384) // when <= 14 bits. otherwise packus_epi16 handles well. min_epi16 is sse2 only unlike min_epu16
          converted_16 = _mm_min_epi16(converted_16, _mm_set1_epi16(max_pixel_value_dithered)); // new_pixel = min(new_pixel, max_pixel_value_dithered); // clamp upper

        // scale back to the required bit depth
        // for generality. Now target == 8 bit, and dither_target is also 8 bit
        // for test: source:10 bit, target=8 bit, dither_target=4 bit
        const int BITDIFF_BETWEEN_DITHER_AND_TARGET = DITHER_BIT_DIFF - (sourcebits - TARGET_BITDEPTH);
        if constexpr(BITDIFF_BETWEEN_DITHER_AND_TARGET != 0)  // dither to 8, target to 8
          converted_16 = _mm_slli_epi16(converted_16, BITDIFF_BETWEEN_DITHER_AND_TARGET); // new_pixel << BITDIFF_BETWEEN_DITHER_AND_TARGET; // if implemented non-8bit dither target, this should be fullscale

        __m128i converted_8 = _mm_packus_epi16(converted_16, zero);

        _mm_storel_epi64(reinterpret_cast<__m128i *>(&dstp[x]), converted_8);
      }
    } // x
    dstp += dst_pitch;
    srcp0 += src_pitch;
  }
}

// Instantiate them
//<uint8_t sourcebits, int dither_mode, int TARGET_DITHER_BITDEPTH, int rgb_step>
// dither, src 10
template void convert_rgb_uint16_to_8_sse2<10, 0, 8, 1>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_rgb_uint16_to_8_sse2<10, 0, 6, 1>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_rgb_uint16_to_8_sse2<10, 0, 4, 1>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_rgb_uint16_to_8_sse2<10, 0, 2, 1>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
// dither, src 12
template void convert_rgb_uint16_to_8_sse2<12, 0, 8, 1>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_rgb_uint16_to_8_sse2<12, 0, 6, 1>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_rgb_uint16_to_8_sse2<12, 0, 4, 1>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
// dither, src 14
template void convert_rgb_uint16_to_8_sse2<14, 0, 8, 1>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_rgb_uint16_to_8_sse2<14, 0, 6, 1>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
// dither, src 16
template void convert_rgb_uint16_to_8_sse2<16, 0, 8, 1>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
// packed rgb src 16
template void convert_rgb_uint16_to_8_sse2<16, 0, 8, 3>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_rgb_uint16_to_8_sse2<16, 0, 8, 4>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);


#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4305 4309)
#endif

template<uint8_t sourcebits, uint8_t TARGET_DITHER_BITDEPTH>
void convert_uint16_to_8_dither_sse2(const BYTE *srcp8, BYTE *dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth)
{
  // Full general ordered dither from 10-16 bits to 2-8 bits, keeping the final 8 bit depth
  // Avisynth's ConvertBits parameter "dither_bits" default 8 goes to TARGET_DITHER_BITDEPTH
  // TARGET_BITDEPTH is always 8 bits, but the dither target can be less than 8.
  // The difference between source bitdepth and TARGET_DITHER_BITDEPTH cannot be more than 8
  // Basic usage: dither down to 8 bits from 10-16 bits.
  // Exotic usage: dither down to 2 bits from 10 bits

  const uint16_t *srcp = reinterpret_cast<const uint16_t *>(srcp8);
  src_pitch = src_pitch / sizeof(uint16_t);
  int src_width = src_rowsize / sizeof(uint16_t); // real width. We take 2x8 word pixels at a time
  int wmod16 = (src_width / 16) * 16;

  int _y_c = 0; // Bayer matrix shift for ordered dither

  const uint8_t TARGET_BITDEPTH = 8; // here is constant (uint8_t target)
  const int max_pixel_value_dithered = (1 << TARGET_DITHER_BITDEPTH) - 1; //may be less than 255, e.g. 15 for dither target 4 bits

  const __m128i max_pixel_value_dithered_epi8 = _mm_set1_epi8(max_pixel_value_dithered);
  // precheck ensures:
  // TARGET_BITDEPTH >= TARGET_DITHER_BITDEPTH
  // sourcebits - TARGET_DITHER_BITDEPTH <= 8
  // sourcebits - TARGET_DITHER_BITDEPTH is even (later we can use PRESHIFT)
  const uint8_t DITHER_BIT_DIFF = (sourcebits - TARGET_DITHER_BITDEPTH); // 2, 4, 6, 8
  const uint8_t PRESHIFT = DITHER_BIT_DIFF & 1;  // 0 or 1: correction for odd bit differences (not used here but for the sake of generality)
  const uint8_t DITHER_ORDER = (DITHER_BIT_DIFF + PRESHIFT) / 2;
  const uint8_t DITHER_SIZE = 1 << DITHER_ORDER; // 9,10=2  11,12=4  13,14=8  15,16=16
  const uint8_t MASK = DITHER_SIZE - 1;
  // 10->8: 0x01 (2x2)
  // 11->8: 0x03 (4x4)
  // 12->8: 0x03 (4x4)
  // 14->8: 0x07 (8x8)
  // 16->8: 0x0F (16x16)
  const BYTE *matrix;
  const BYTE *matrix_c;
  switch (sourcebits - TARGET_DITHER_BITDEPTH) {
  case 2: matrix = reinterpret_cast<const BYTE *>(dither2x2.data_sse2);
    matrix_c = reinterpret_cast<const BYTE *>(dither2x2.data);
    break;
  case 4: matrix = reinterpret_cast<const BYTE *>(dither4x4.data_sse2);
    matrix_c = reinterpret_cast<const BYTE *>(dither4x4.data);
    break;
  case 6:
    matrix = reinterpret_cast<const BYTE *>(dither8x8.data_sse2);
    matrix_c = reinterpret_cast<const BYTE *>(dither8x8.data);
    break;
  case 8:
    matrix = reinterpret_cast<const BYTE *>(dither16x16.data);
    matrix_c = matrix;
    break;
  default: return; // n/a
  }

  const BYTE *current_matrix_line;

  __m128i zero = _mm_setzero_si128();

  for (int y = 0; y < src_height; y++)
  {
    _y_c = (y & MASK) << DITHER_ORDER; // matrix lines stride for C
    current_matrix_line = matrix + ((y & MASK) << 4); // always 16 byte boundary

    __m128i corr = _mm_load_si128(reinterpret_cast<const __m128i*>(current_matrix_line)); // int corr = matrix[_y | (x & MASK)];
    __m128i corr_lo = _mm_unpacklo_epi8(corr, zero); // lower 8 byte->uint16_t
    __m128i corr_hi = _mm_unpackhi_epi8(corr, zero); // upper 8 byte->uint16_t

    for (int x = 0; x < src_width; x += 16)
    {
      __m128i src_lo = _mm_load_si128(reinterpret_cast<const __m128i*>(srcp + x)); // 8* uint16
      __m128i src_hi = _mm_load_si128(reinterpret_cast<const __m128i*>(srcp + x + 8));

      // int new_pixel = ((srcp0[x] + corr) >> DITHER_BIT_DIFF);

      __m128i new_pixel_lo, new_pixel_hi;

      if constexpr(sourcebits < 16) { // no overflow
        new_pixel_lo = _mm_srli_epi16(_mm_add_epi16(src_lo, corr_lo), DITHER_BIT_DIFF);
        new_pixel_hi = _mm_srli_epi16(_mm_add_epi16(src_hi, corr_hi), DITHER_BIT_DIFF);
        // scale down after adding dithering noise
      }
      else { // source bits: 16. Overflow can happen when 0xFFFF it dithered up. Go 32 bits
        // lower
        __m128i src_lo_lo = _mm_unpacklo_epi16(src_lo, zero);
        __m128i corr_lo_lo = _mm_unpacklo_epi16(corr_lo, zero);
        __m128i new_pixel_lo_lo = _mm_srli_epi32(_mm_add_epi32(src_lo_lo, corr_lo_lo), DITHER_BIT_DIFF);

        __m128i src_lo_hi = _mm_unpackhi_epi16(src_lo, zero);
        __m128i corr_lo_hi = _mm_unpackhi_epi16(corr_lo, zero);
        __m128i new_pixel_lo_hi = _mm_srli_epi32(_mm_add_epi32(src_lo_hi, corr_lo_hi), DITHER_BIT_DIFF);

        new_pixel_lo = _mm_packs_epi32(new_pixel_lo_lo, new_pixel_lo_hi); // packs is enough
        // upper
        __m128i src_hi_lo = _mm_unpacklo_epi16(src_hi, zero);
        __m128i corr_hi_lo = _mm_unpacklo_epi16(corr_hi, zero);
        __m128i new_pixel_hi_lo = _mm_srli_epi32(_mm_add_epi32(src_hi_lo, corr_hi_lo), DITHER_BIT_DIFF);

        __m128i src_hi_hi = _mm_unpackhi_epi16(src_hi, zero);
        __m128i corr_hi_hi = _mm_unpackhi_epi16(corr_hi, zero);
        __m128i new_pixel_hi_hi = _mm_srli_epi32(_mm_add_epi32(src_hi_hi, corr_hi_hi), DITHER_BIT_DIFF);

        new_pixel_hi = _mm_packs_epi32(new_pixel_hi_lo, new_pixel_hi_hi); // packs is enough
      }

      __m128i new_pixel = _mm_packus_epi16(new_pixel_lo, new_pixel_hi); // 2x8 x16 bit -> 16 byte. Clamp is automatic

      if constexpr(TARGET_DITHER_BITDEPTH < 8) { // generic (not used) fun option to dither 10->4 bits then back to 8 bit
        new_pixel = _mm_min_epu8(new_pixel, max_pixel_value_dithered_epi8);
      }

      const int BITDIFF_BETWEEN_DITHER_AND_TARGET = DITHER_BIT_DIFF - (sourcebits - TARGET_BITDEPTH);
      if constexpr(BITDIFF_BETWEEN_DITHER_AND_TARGET != 0) { //==0 when dither and target are both 8
        // scale back, when e.g. 10 bit data is dithered down to 4,6,8 bits but the target bit depth is still 8 bit.
        new_pixel = _mm_and_si128(_mm_set1_epi8((0xFF << BITDIFF_BETWEEN_DITHER_AND_TARGET) & 0xFF), _mm_slli_epi32(new_pixel, BITDIFF_BETWEEN_DITHER_AND_TARGET));
        // non-existant _mm_slli_epi8. closest in palette: simple shift
      }

      _mm_store_si128(reinterpret_cast<__m128i*>(dstp + x), new_pixel);
    }

    // rest, C
    for (int x = wmod16; x < src_width; x++)
    {
      int corr = matrix_c[_y_c | (x & MASK)];
      //BYTE new_pixel = (((srcp0[x] << PRESHIFT) >> (sourcebits - 8)) + corr) >> PRESHIFT; // >> (sourcebits - 8);
      int new_pixel = ((srcp[x] + corr) >> DITHER_BIT_DIFF);
      new_pixel = min(new_pixel, max_pixel_value_dithered); // clamp upper
      // scale back to the required bit depth
      // for generality. Now target == 8 bit, and dither_target is also 8 bit
      // for test: source:10 bit, target=8 bit, dither_target=4 bit
      const int BITDIFF_BETWEEN_DITHER_AND_TARGET = DITHER_BIT_DIFF - (sourcebits - TARGET_BITDEPTH);
      if constexpr(BITDIFF_BETWEEN_DITHER_AND_TARGET != 0)  // dither to 8, target to 8
        new_pixel = new_pixel << BITDIFF_BETWEEN_DITHER_AND_TARGET; // closest in palette: simple shift with
      dstp[x] = (BYTE)new_pixel;
    }

    dstp += dst_pitch;
    srcp += src_pitch;
  }
}

// instantiate them
//template<uint8_t sourcebits, uint8_t TARGET_DITHER_BITDEPTH>
template void convert_uint16_to_8_dither_sse2<10, 8>(const BYTE* srcp8, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_uint16_to_8_dither_sse2<12, 8>(const BYTE* srcp8, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_uint16_to_8_dither_sse2<14, 8>(const BYTE* srcp8, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_uint16_to_8_dither_sse2<16, 8>(const BYTE* srcp8, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_uint16_to_8_dither_sse2<10, 6>(const BYTE* srcp8, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_uint16_to_8_dither_sse2<12, 6>(const BYTE* srcp8, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_uint16_to_8_dither_sse2<14, 6>(const BYTE* srcp8, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_uint16_to_8_dither_sse2<10, 4>(const BYTE* srcp8, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_uint16_to_8_dither_sse2<12, 4>(const BYTE* srcp8, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);
template void convert_uint16_to_8_dither_sse2<10, 2>(const BYTE* srcp8, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);


// float to 8 bit, float to 10/12/14/16 bit

// sse4.1
template<typename pixel_t, bool chroma, bool fulls, bool fulld>
#if defined(GCC) || defined(CLANG)
__attribute__((__target__("sse4.1")))
#endif
void convert_32_to_uintN_sse41(const BYTE *srcp8, BYTE *dstp8, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth)
{
  const float *srcp = reinterpret_cast<const float *>(srcp8);
  pixel_t *dstp = reinterpret_cast<pixel_t *>(dstp8);

  src_pitch = src_pitch / sizeof(float);
  dst_pitch = dst_pitch / sizeof(pixel_t);

  int src_width = src_rowsize / sizeof(float);

  const int max_pixel_value = (1 << target_bitdepth) - 1;

  const int limit_lo_d = (fulld ? 0 : 16) << (target_bitdepth - 8);
  const int limit_hi_d = fulld ? ((1 << target_bitdepth) - 1) : ((chroma ? 240 : 235) << (target_bitdepth - 8));
  const float range_diff_d = (float)limit_hi_d - limit_lo_d;

  constexpr int limit_lo_s = fulls ? 0 : 16;
  constexpr int limit_hi_s = fulls ? 255 : (chroma ? 240 : 235);
  constexpr float range_diff_s = (limit_hi_s - limit_lo_s) / 255.0f;

  // fulls fulld luma             luma_new   chroma                          chroma_new
  // true  false 0..1              16-235     -0.5..0.5                      16-240       Y = Y * ((235-16) << (bpp-8)) + 16, Chroma= Chroma * ((240-16) << (bpp-8)) + 16
  // true  true  0..1               0-255     -0.5..0.5                      0-128-255
  // false false 16/255..235/255   16-235     (16-128)/255..(240-128)/255    16-240
  // false true  16/255..235/255    0..1      (16-128)/255..(240-128)/255    0-128-255
  const float factor = range_diff_d / range_diff_s;

  const float half_i = (float)(1 << (target_bitdepth - 1));
  const __m128 halfint_plus_rounder_ps = _mm_set1_ps(half_i + 0.5f);
  const __m128 limit_lo_s_ps = _mm_set1_ps(limit_lo_s / 255.0f);
  const __m128 limit_lo_plus_rounder_ps = _mm_set1_ps(limit_lo_d + 0.5f);
  const __m128 max_dst_pixelvalue = _mm_set1_ps((float)max_pixel_value); // 255, 1023, 4095, 16383, 65535.0
  const __m128 zero = _mm_setzero_ps();

  __m128 factor_ps = _mm_set1_ps(factor); // 0-1.0 -> 0..max_pixel_value

  for (int y = 0; y<src_height; y++)
  {
    for (int x = 0; x < src_width; x += 8) // 8 pixels at a time
    {
      __m128i result;
      __m128i result_0, result_1;
      __m128 src_0 = _mm_load_ps(reinterpret_cast<const float *>(srcp + x));
      __m128 src_1 = _mm_load_ps(reinterpret_cast<const float *>(srcp + x + 4));
      if constexpr(chroma) {
        src_0 = _mm_add_ps(_mm_mul_ps(src_0, factor_ps), halfint_plus_rounder_ps);
        src_1 = _mm_add_ps(_mm_mul_ps(src_1, factor_ps), halfint_plus_rounder_ps);
      }
      else {
        if constexpr(!fulls) {
          src_0 = _mm_sub_ps(src_0, limit_lo_s_ps);
          src_1 = _mm_sub_ps(src_1, limit_lo_s_ps);
        }
        src_0 = _mm_add_ps(_mm_mul_ps(src_0, factor_ps), limit_lo_plus_rounder_ps);
        src_1 = _mm_add_ps(_mm_mul_ps(src_1, factor_ps), limit_lo_plus_rounder_ps);
        //pixel = (srcp0[x] - limit_lo_s_ps) * factor + half + limit_lo + 0.5f;
      }

      src_0 = _mm_max_ps(_mm_min_ps(src_0, max_dst_pixelvalue), zero);
      src_1 = _mm_max_ps(_mm_min_ps(src_1, max_dst_pixelvalue), zero);
      result_0 = _mm_cvttps_epi32(src_0); // truncate
      result_1 = _mm_cvttps_epi32(src_1);
      if constexpr(sizeof(pixel_t) == 2) {
        result = _mm_packus_epi32(result_0, result_1); // sse41
        _mm_store_si128(reinterpret_cast<__m128i *>(dstp + x), result);
      }
      else {
        result = _mm_packs_epi32(result_0, result_1);
        result = _mm_packus_epi16(result, result); // lo 8 byte
        _mm_storel_epi64(reinterpret_cast<__m128i *>(dstp + x), result);
      }
    }
    dstp += dst_pitch;
    srcp += src_pitch;
  }
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

// instantiate them
//template<typename pixel_t, bool chroma, bool fulls, bool fulld>
#define DEF_convert_32_to_uintN_functions(uint_X_t) \
template void convert_32_to_uintN_sse41<uint_X_t, false, true, true>(const BYTE* srcp8, BYTE* dstp8, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_32_to_uintN_sse41<uint_X_t, true, true, true>(const BYTE* srcp8, BYTE* dstp8, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_32_to_uintN_sse41<uint_X_t, false, true, false>(const BYTE* srcp8, BYTE* dstp8, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_32_to_uintN_sse41<uint_X_t, true, true, false>(const BYTE* srcp8, BYTE* dstp8, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_32_to_uintN_sse41<uint_X_t, false, false, true>(const BYTE* srcp8, BYTE* dstp8, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_32_to_uintN_sse41<uint_X_t, true, false, true>(const BYTE* srcp8, BYTE* dstp8, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_32_to_uintN_sse41<uint_X_t, false, false, false>(const BYTE* srcp8, BYTE* dstp8, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_32_to_uintN_sse41<uint_X_t, true, false, false>(const BYTE* srcp8, BYTE* dstp8, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);

DEF_convert_32_to_uintN_functions(uint8_t)
DEF_convert_32_to_uintN_functions(uint16_t)

#undef DEF_convert_32_to_uintN_functions

// YUV: bit shift 8-16 <=> 8-16 bits
// shift right or left, depending on expandrange
template<typename pixel_t_s, typename pixel_t_d>
#if defined(GCC) || defined(CLANG)
__attribute__((__target__("sse4.1")))
#endif
static void convert_uint_limited_sse41(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth)
{
  const pixel_t_s* srcp0 = reinterpret_cast<const pixel_t_s*>(srcp);
  pixel_t_d* dstp0 = reinterpret_cast<pixel_t_d*>(dstp);

  src_pitch = src_pitch / sizeof(pixel_t_s);
  dst_pitch = dst_pitch / sizeof(pixel_t_d);

  const int src_width = src_rowsize / sizeof(pixel_t_s);
  int wmod = (src_width / 16) * 16;

  if (target_bitdepth > source_bitdepth) // expandrange. pixel_t_d is always uint16_t
  {
    const int shift_bits = target_bitdepth - source_bitdepth;
    __m128i shift = _mm_set_epi32(0, 0, 0, shift_bits);
    for (int y = 0; y < src_height; y++)
    {
      for (int x = 0; x < src_width; x += 16)
      {
        __m128i src_lo, src_hi;
        if constexpr (sizeof(pixel_t_s) == 1) {
          auto src = _mm_load_si128(reinterpret_cast<const __m128i*>(srcp0 + x)); // 16* uint8
          src_lo = _mm_cvtepu8_epi16(src);                       // 8* uint16
          src_hi = _mm_unpackhi_epi8(src, _mm_setzero_si128());  // 8* uint16
        }
        else {
          src_lo = _mm_load_si128(reinterpret_cast<const __m128i*>(srcp0 + x)); // 8* uint_16
          src_hi = _mm_load_si128(reinterpret_cast<const __m128i*>(srcp0 + x + 8)); // 8* uint_16
        }
        src_lo = _mm_sll_epi16(src_lo, shift);
        src_hi = _mm_sll_epi16(src_hi, shift);
        if constexpr (sizeof(pixel_t_d) == 1) {
          // upconvert always to 2 bytes
          assert(0);
        }
        else {
          _mm_store_si128(reinterpret_cast<__m128i*>(dstp0 + x), src_lo);
          _mm_store_si128(reinterpret_cast<__m128i*>(dstp0 + x + 8), src_hi);
        }
      }
      // rest
      for (int x = wmod; x < src_width; x++) {
        dstp0[x] = (pixel_t_d)(srcp0[x]) << shift_bits;  // expand range. No clamp before, source is assumed to have valid range
      }
      dstp0 += dst_pitch;
      srcp0 += src_pitch;
    }
  }
  else
  {
    // reduce range
    const int shift_bits = source_bitdepth - target_bitdepth;
    const int round = 1 << (shift_bits - 1);
    __m128i shift = _mm_set_epi32(0, 0, 0, shift_bits);
    const auto round_simd = _mm_set1_epi16(round);

    for (int y = 0; y < src_height; y++)
    {
      for (int x = 0; x < src_width; x += 16)
      {
        if constexpr (sizeof(pixel_t_s) == 1)
          assert(0);
        // downconvert always from 2 bytes
        auto src_lo = _mm_load_si128(reinterpret_cast<const __m128i*>(srcp0 + x)); // 8* uint_16
        auto src_hi = _mm_load_si128(reinterpret_cast<const __m128i*>(srcp0 + x + 8)); // 8* uint_16
        src_lo = _mm_srl_epi16(_mm_adds_epu16(src_lo, round_simd), shift);
        src_hi = _mm_srl_epi16(_mm_adds_epu16(src_hi, round_simd), shift);
        if constexpr (sizeof(pixel_t_d) == 1) {
          // to 8 bits
          auto dst = _mm_packus_epi16(src_lo, src_hi);
          _mm_store_si128(reinterpret_cast<__m128i*>(dstp0 + x), dst);
        }
        else {
          _mm_store_si128(reinterpret_cast<__m128i*>(dstp0 + x), src_lo);
          _mm_store_si128(reinterpret_cast<__m128i*>(dstp0 + x + 8), src_hi);
        }
      }
      // rest
      for (int x = wmod; x < src_width; x++) {
        dstp0[x] = (srcp0[x] + round) >> shift_bits;  // reduce range
      }
      dstp0 += dst_pitch;
      srcp0 += src_pitch;
    }
  }
}

template<typename pixel_t_s, typename pixel_t_d, bool chroma, bool fulls, bool fulld>
#if defined(GCC) || defined(CLANG)
__attribute__((__target__("sse4.1")))
#endif
void convert_uint_sse41(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth)
{
  // limited to limited is bitshift, see in other function
  if constexpr (!fulls && !fulld) {
    convert_uint_limited_sse41< pixel_t_s, pixel_t_d>(srcp, dstp, src_rowsize, src_height, src_pitch, dst_pitch, source_bitdepth, target_bitdepth);
    return;
  }

  const pixel_t_s* srcp0 = reinterpret_cast<const pixel_t_s*>(srcp);
  pixel_t_d* dstp0 = reinterpret_cast<pixel_t_d*>(dstp);

  src_pitch = src_pitch / sizeof(pixel_t_s);
  dst_pitch = dst_pitch / sizeof(pixel_t_d);

  int src_width = src_rowsize / sizeof(pixel_t_s);
  int wmod16 = (src_width / 16) * 16;

  if constexpr (sizeof(pixel_t_s) == 1 && sizeof(pixel_t_d) == 2) {
    if (fulls && fulld && !chroma && source_bitdepth == 8 && target_bitdepth == 16) {
      // special case 8->16 bit full scale: * 65535 / 255 = *257
      __m128i zero = _mm_setzero_si128();
      __m128i multiplier = _mm_set1_epi16(257);

      for (int y = 0; y < src_height; y++)
      {
        for (int x = 0; x < src_width; x += 16)
        {
          __m128i src = _mm_load_si128(reinterpret_cast<const __m128i*>(srcp0 + x)); // 16* uint8
          __m128i src_lo = _mm_unpacklo_epi8(src, zero);             // 8* uint16
          __m128i src_hi = _mm_unpackhi_epi8(src, zero);             // 8* uint16
            // *257 mullo is faster than x*257 = (x<<8 + x) add/or solution (i7)
          __m128i res_lo = _mm_mullo_epi16(src_lo, multiplier); // lower 16 bit of multiplication is enough
          __m128i res_hi = _mm_mullo_epi16(src_hi, multiplier);
          // dstp[x] = srcp0[x] * 257; // RGB: full range 0..255 <-> 0..65535 (257 = 65535 / 255)
          _mm_store_si128(reinterpret_cast<__m128i*>(dstp0 + x), res_lo);
          _mm_store_si128(reinterpret_cast<__m128i*>(dstp0 + x + 8), res_hi);
        } // for x
        // rest
        for (int x = wmod16; x < src_width; x++)
        {
          dstp0[x] = (pixel_t_d)(srcp0[x] * 257);
        }
        dstp0 += dst_pitch;
        srcp0 += src_pitch;
      } // for y
      return;
    }
  }

  const int target_max = (1 << target_bitdepth) - 1;

  float mul_factor;
  int src_offset, dst_offset;

  if constexpr (chroma) {
    // chroma: go into signed world, factor, then go back to biased range
    src_offset = 1 << (source_bitdepth - 1); // chroma center of source
    dst_offset = 1 << (target_bitdepth - 1); // chroma center of target
    mul_factor =
      fulls && fulld ? (float)(dst_offset - 1) / (src_offset - 1) :
      fulld ? (float)(dst_offset - 1) / (112 << (source_bitdepth - 8)) : // +-127 ==> +-112 (240-16)/2
      /*fulld*/ (float)(112 << (target_bitdepth - 8)) / (src_offset - 1); // +-112 (240-16)/2 ==> +-127
  }
  else {
    // luma/limited: subtract offset, convert, add offset
    src_offset = fulls ? 0 : 16 << (source_bitdepth - 8); // full/limited range low limit
    dst_offset = fulld ? 0 : 16 << (target_bitdepth - 8); // // full/limited range low limit
    const int source_max = (1 << source_bitdepth) - 1;
    mul_factor =
      fulls && fulld ? (float)target_max / source_max :
      fulld ? (float)target_max / (219 << (source_bitdepth - 8)) : // 0..255 ==> 16-235 (219)
      /*fulld*/ (float)(219 << (target_bitdepth - 8)) / source_max; // 16-235 ==> 0..255
  }

  auto vect_mul_factor = _mm_set1_ps(mul_factor);
  auto vect_src_offset = _mm_set1_epi32(src_offset);
  auto dst_offset_plus_round = dst_offset + 0.5f;
  auto vect_dst_offset_plus_round = _mm_set1_ps(dst_offset_plus_round);

  auto vect_target_max = _mm_set1_epi16(target_max);
  constexpr auto target_min = chroma && fulld ? 1 : 0;
  auto vect_target_min = _mm_set1_epi32(target_min); // chroma-only. 0 is not valid in full case. We leave limited as is.

  auto zero = _mm_setzero_si128();

  for (int y = 0; y < src_height; y++)
  {
    for (int x = 0; x < src_width; x += 16)
    {
      __m128i src_lo, src_hi;
      if constexpr (sizeof(pixel_t_s) == 1) {
        auto src = _mm_load_si128(reinterpret_cast<const __m128i*>(srcp0 + x)); // 16* uint8
        src_lo = _mm_cvtepu8_epi16(src);                   // 8* uint16
        src_hi = _mm_unpackhi_epi8(src, zero);             // 8* uint16
      }
      else {
        src_lo = _mm_load_si128(reinterpret_cast<const __m128i*>(srcp0 + x)); // 8* uint_16
        src_hi = _mm_load_si128(reinterpret_cast<const __m128i*>(srcp0 + x + 8)); // 8* uint_16
      }

      __m128i result1, result2;
      {
        // src: 8*uint16
        // convert to int32, bias, to float
        auto res_lo_i = _mm_cvtepu16_epi32(src_lo);
        auto res_hi_i = _mm_unpackhi_epi16(src_lo, zero);
        if constexpr (chroma || !fulls) {
          // src_offset is not zero
          res_lo_i = _mm_sub_epi32(res_lo_i, vect_src_offset);
          res_hi_i = _mm_sub_epi32(res_hi_i, vect_src_offset);
        }
        auto res_lo = _mm_cvtepi32_ps(res_lo_i);
        auto res_hi = _mm_cvtepi32_ps(res_hi_i);
        // multiply, bias back+round
        res_lo = _mm_add_ps(_mm_mul_ps(res_lo, vect_mul_factor), vect_dst_offset_plus_round);
        res_hi = _mm_add_ps(_mm_mul_ps(res_hi, vect_mul_factor), vect_dst_offset_plus_round);
        // convert back w/ truncate
        auto result_l = _mm_cvttps_epi32(res_lo); // no banker's rounding
        auto result_h = _mm_cvttps_epi32(res_hi);
        if constexpr (chroma) {
          result_l = _mm_max_epi32(result_l, vect_target_min);
          result_h = _mm_max_epi32(result_h, vect_target_min);
        }
        // back to 16 bit
        result1 = _mm_packus_epi32(result_l, result_h); // 8 * 16 bit pixels
        result1 = _mm_min_epu16(result1, vect_target_max);
      }

      // byte target: not yet
      if constexpr (sizeof(pixel_t_d) == 2)
        _mm_store_si128(reinterpret_cast<__m128i*>(dstp0 + x), result1);

      {
        // src: 8*uint16
        // convert to int32, bias, to float
        auto res_lo_i = _mm_cvtepu16_epi32(src_hi);
        auto res_hi_i = _mm_unpackhi_epi16(src_hi, zero);
        if constexpr (chroma || !fulls) {
          // src_offset is not zero
          res_lo_i = _mm_sub_epi32(res_lo_i, vect_src_offset);
          res_hi_i = _mm_sub_epi32(res_hi_i, vect_src_offset);
        }
        auto res_lo = _mm_cvtepi32_ps(res_lo_i);
        auto res_hi = _mm_cvtepi32_ps(res_hi_i);
        // multiply, bias back+round
        res_lo = _mm_add_ps(_mm_mul_ps(res_lo, vect_mul_factor), vect_dst_offset_plus_round);
        res_hi = _mm_add_ps(_mm_mul_ps(res_hi, vect_mul_factor), vect_dst_offset_plus_round);
        // convert back w/ truncate
        auto result_l = _mm_cvttps_epi32(res_lo);
        auto result_h = _mm_cvttps_epi32(res_hi);
        if constexpr (chroma) {
          result_l = _mm_max_epi32(result_l, vect_target_min);
          result_h = _mm_max_epi32(result_h, vect_target_min);
        }
        // back to 16 bit
        result2 = _mm_packus_epi32(result_l, result_h); // 8 * 16 bit pixels
        result2 = _mm_min_epu16(result2, vect_target_max);
      }

      if constexpr (sizeof(pixel_t_d) == 2)
        _mm_store_si128(reinterpret_cast<__m128i*>(dstp0 + x + 8), result2);
      else {
        // byte target: store both
        auto result12 = _mm_packus_epi16(result1, result2);
        _mm_store_si128(reinterpret_cast<__m128i*>(dstp0 + x), result12);
      }
    } // for x

    // rest
    for (int x = wmod16; x < src_width; x++)
    {
      const float val = (srcp0[x] - src_offset) * mul_factor + dst_offset_plus_round;
      dstp0[x] = clamp((int)val, target_min, target_max);
    }

    dstp0 += dst_pitch;
    srcp0 += src_pitch;
  }
}

// instantiate them all
#define convert_uint_sse4_functions(uint_X_t, uint_X_dest_t) \
template void convert_uint_sse41<uint_X_t, uint_X_dest_t, false, false, false>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_uint_sse41<uint_X_t, uint_X_dest_t, false, false, true>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_uint_sse41<uint_X_t, uint_X_dest_t, false, true, false>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_uint_sse41<uint_X_t, uint_X_dest_t, false, true, true>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_uint_sse41<uint_X_t, uint_X_dest_t, true, false, false>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_uint_sse41<uint_X_t, uint_X_dest_t, true, false, true>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_uint_sse41<uint_X_t, uint_X_dest_t, true, true, false>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth); \
template void convert_uint_sse41<uint_X_t, uint_X_dest_t, true, true, true>(const BYTE* srcp, BYTE* dstp, int src_rowsize, int src_height, int src_pitch, int dst_pitch, int source_bitdepth, int target_bitdepth);

convert_uint_sse4_functions(uint8_t, uint8_t)
convert_uint_sse4_functions(uint8_t, uint16_t)
convert_uint_sse4_functions(uint16_t, uint8_t)
convert_uint_sse4_functions(uint16_t, uint16_t)

#undef convert_uint_sse4_functions
