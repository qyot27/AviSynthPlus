// Avisynth v2.5.  Copyright 2002 Ben Rudiak-Gould et al.
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

// Overlay (c) 2003, 2004 by Klaus Post

#include "overlayfunctions.h"

#include <stdint.h>
#include <type_traits>
#ifdef INTEL_INTRINSICS
#include "intel/OF_multiply_sse.h"
#include "intel/OF_multiply_avx2.h"
#else
#include "simde/OF_multiply_sse.h"
#include "simde/OF_multiply_avx2.h"
#endif
void OL_MultiplyImage::DoBlendImageMask(ImageOverlayInternal* base, ImageOverlayInternal* overlay, ImageOverlayInternal* mask) {
  if (bits_per_pixel == 8)
    BlendImageMask<uint8_t>(base, overlay, mask);
  else if(bits_per_pixel <= 16)
    BlendImageMask<uint16_t>(base, overlay, mask);
  //else if(bits_per_pixel == 32)
  //  BlendImageMask<float>(base, overlay, mask);

}

void OL_MultiplyImage::DoBlendImage(ImageOverlayInternal* base, ImageOverlayInternal* overlay) {
  if (bits_per_pixel == 8)
    BlendImage<uint8_t>(base, overlay);
  else if(bits_per_pixel <= 16)
    BlendImage<uint16_t>(base, overlay);
  //else if(bits_per_pixel == 32)
  //  BlendImage<float>(base, overlay);
}

// mode multiply: Darkens the image in proportion to overlay lightness.

template<typename pixel_t, bool opacity_is_full, bool has_mask>
static void of_multiply_c(
  int bits_per_pixel,
  const float opacity_f,
  const int opacity,
  int width, int height,
  const pixel_t* ovY,
  int overlaypitch,
  pixel_t* baseY, pixel_t* baseU, pixel_t* baseV,
  int basepitch,
  const pixel_t* maskY, const pixel_t* maskU, const pixel_t* maskV,
  int maskpitch
  )
{
  const int max_pixel_value = (sizeof(pixel_t) == 1) ? 255 : (1 << bits_per_pixel) - 1;
  const float factor = 1.0f / max_pixel_value;
  const int half_i = 1 << (bits_per_pixel - 1);
  const float half_f = (float)half_i;

  float factor_mul_opacity;
  if constexpr (opacity_is_full)
    factor_mul_opacity = factor * 1.0f;
  else
    factor_mul_opacity = factor * opacity_f;

  // start processing
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int Y, U, V;

      // generic, 8-16 bits
      // This part re-appears in SSE code (non mod4 end-of-line fragment in C)
      // Unlike the old integer version here is proper rounding
      const float overlay_opacity_minus1 = ovY[x] * factor - 1.0f;
      if constexpr (has_mask) {
        float final_opacity;

        final_opacity = maskY[x] * factor_mul_opacity;
        auto Yfactor = 1.0f + overlay_opacity_minus1 * final_opacity;
        Y = (int)(baseY[x] * Yfactor + 0.5f);

        final_opacity = maskU[x] * factor_mul_opacity;
        auto Ufactor = 1.0f + overlay_opacity_minus1 * final_opacity;
        U = (int)(((float)baseU[x] - half_f) * Ufactor + half_f + 0.5f);

        final_opacity = maskV[x] * factor_mul_opacity;
        auto Vfactor = 1.0f + overlay_opacity_minus1 * final_opacity;
        V = (int)(((float)baseV[x] - half_f) * Vfactor + half_f + 0.5f);
      }
      else {
        const float common_factor = 1.0f + overlay_opacity_minus1 * opacity_f;

        auto Yfactor = common_factor;
        Y = (int)((float)baseY[x] * Yfactor + 0.5f);

        auto Ufactor = common_factor;
        U = (int)(((float)baseU[x] - half_f) * Ufactor + half_f + 0.5f);

        auto Vfactor = common_factor;
        V = (int)(((float)baseV[x] - half_f) * Vfactor + half_f + 0.5f);

      }

      baseU[x] = (pixel_t)U;
      baseV[x] = (pixel_t)V;
      baseY[x] = (pixel_t)Y;
    }

    if constexpr (has_mask) {
      maskY += maskpitch;
      maskU += maskpitch;
      maskV += maskpitch;
    }

    baseY += basepitch;
    baseU += basepitch;
    baseV += basepitch;

    ovY += overlaypitch;

  }
}

// old, integer-inside C version.
// compared to the float-inside version, here is no proper rounding
// 8 bit processing is quicker though
template<typename pixel_t, bool opacity_is_full, bool has_mask>
static void of_multiply_c_old(
  int bits_per_pixel,
  const float opacity_f,
  const int opacity,
  int width, int height,
  const pixel_t* ovY,
  int overlaypitch,
  pixel_t* baseY, pixel_t* baseU, pixel_t* baseV,
  int basepitch,
  const pixel_t* maskY, const pixel_t* maskU, const pixel_t* maskV,
  int maskpitch
)
{
  const int max_pixel_value = (sizeof(pixel_t) == 1) ? 255 : (1 << bits_per_pixel) - 1;
  const int chroma_half_corr = (sizeof(pixel_t) == 1) ? 128 : (1 << (bits_per_pixel - 1));
  const int pixel_range = max_pixel_value + 1;
  const int MASK_CORR_SHIFT = (sizeof(pixel_t) == 1) ? 8 : bits_per_pixel;
  const int inv_opacity = 256 - opacity;

  // start processing
  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      int Y, U, V;

      // for 8 bit, which is a bit quicker with integer arithmetics
      // avoid "uint16*uint16 can't get into int32" overflows
      typedef typename std::conditional < sizeof(pixel_t) == 1, int, typename std::conditional < sizeof(pixel_t) == 2, int64_t, float>::type >::type result_t;

      if constexpr (has_mask) {
        result_t overlay_Y = ovY[x];

        int final_opacity;
        result_t inverse_final_opacity;

        if constexpr (opacity_is_full)
          final_opacity = maskY[x];
        else
          final_opacity = (maskY[x] * opacity) >> 8;
        inverse_final_opacity = pixel_range - final_opacity;
        Y = (int)((baseY[x] * (pixel_range * inverse_final_opacity + (overlay_Y * final_opacity))) >> (MASK_CORR_SHIFT * 2));

        if constexpr (opacity_is_full)
          final_opacity = maskU[x];
        else
          final_opacity = (maskU[x] * opacity) >> 8;
        inverse_final_opacity = pixel_range - final_opacity;
        U = (int)(((baseU[x] * inverse_final_opacity * pixel_range) + (final_opacity * (baseU[x] * overlay_Y + chroma_half_corr * (pixel_range - overlay_Y)))) >> (MASK_CORR_SHIFT * 2));

        if constexpr (opacity_is_full)
          final_opacity = maskV[x];
        else
          final_opacity = (maskV[x] * opacity) >> 8;
        inverse_final_opacity = pixel_range - final_opacity;
        V = (int)(((baseV[x] * inverse_final_opacity * pixel_range) + (final_opacity * (baseV[x] * overlay_Y + chroma_half_corr * (pixel_range - overlay_Y)))) >> (MASK_CORR_SHIFT * 2));
      }
      else {
        // no mask clip
        if constexpr (opacity_is_full) {
          result_t ovYx = ovY[x];
          Y = (int)((baseY[x] * ovYx) >> MASK_CORR_SHIFT);
          U = (int)((baseU[x] * ovYx + chroma_half_corr * (pixel_range - ovYx)) >> MASK_CORR_SHIFT);
          V = (int)((baseV[x] * ovYx + chroma_half_corr * (pixel_range - ovYx)) >> MASK_CORR_SHIFT);
        }
        else {
          result_t ovYx = ovY[x];
          result_t baseYx = baseY[x];
          Y = (int)((baseYx * (pixel_range * inv_opacity + (ovYx * opacity))) >> (MASK_CORR_SHIFT + 8));
          result_t baseUx = baseU[x];
          U = (int)(((baseUx * inv_opacity * pixel_range) + (opacity * (baseUx * ovYx + chroma_half_corr * (pixel_range - ovYx)))) >> (MASK_CORR_SHIFT + 8));
          result_t baseVx = baseV[x];
          V = (int)(((baseVx * inv_opacity * pixel_range) + (opacity * (baseVx * ovYx + chroma_half_corr * (pixel_range - ovYx)))) >> (MASK_CORR_SHIFT + 8));
        }
      }

      baseU[x] = (pixel_t)U;
      baseV[x] = (pixel_t)V;
      baseY[x] = (pixel_t)Y;
    }

    if constexpr (has_mask) {
      maskY += maskpitch;
      maskU += maskpitch;
      maskV += maskpitch;
    }

    baseY += basepitch;
    baseU += basepitch;
    baseV += basepitch;

    ovY += overlaypitch;

  }
}

// mode multiply: Darkens the image in proportion to overlay lightness.
template<typename pixel_t>
void OL_MultiplyImage::BlendImageMask(ImageOverlayInternal* base, ImageOverlayInternal* overlay, ImageOverlayInternal* mask) {
  pixel_t* baseY = reinterpret_cast<pixel_t*>(base->GetPtr(PLANAR_Y));
  pixel_t* baseU = reinterpret_cast<pixel_t*>(base->GetPtr(PLANAR_U));
  pixel_t* baseV = reinterpret_cast<pixel_t*>(base->GetPtr(PLANAR_V));

  pixel_t* ovY = reinterpret_cast<pixel_t*>(overlay->GetPtr(PLANAR_Y));

  pixel_t* maskY = reinterpret_cast<pixel_t*>(mask->GetPtr(PLANAR_Y));
  pixel_t* maskU = reinterpret_cast<pixel_t*>(mask->GetPtr(PLANAR_U));
  pixel_t* maskV = reinterpret_cast<pixel_t*>(mask->GetPtr(PLANAR_V));

  const int basepitch = (base->pitch) / sizeof(pixel_t);
  const int overlaypitch = (overlay->pitch) / sizeof(pixel_t);
  const int maskpitch = (mask->pitch) / sizeof(pixel_t);

  int w = base->w();
  int h = base->h();
#ifdef INTEL_INTRINSICS
  if (!!(env->GetCPUFlags() & CPUF_AVX2))
  {
    if (opacity == 256)
      of_multiply_avx2<pixel_t, true, true>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, maskY, maskU, maskV, maskpitch);
    else
      of_multiply_avx2<pixel_t, false, true>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, maskY, maskU, maskV, maskpitch);
  }
  else if (!!(env->GetCPUFlags() & CPUF_SSE4_1))
  {
    if (opacity == 256)
      of_multiply_sse41<pixel_t, true, true>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, maskY, maskU, maskV, maskpitch);
    else
      of_multiply_sse41<pixel_t, false, true>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, maskY, maskU, maskV, maskpitch);
  }
  else
#endif
  {
    // old integer C code: 8 bit is quicker than float-based
    if (sizeof(pixel_t) == 1) {
      if (opacity == 256)
        of_multiply_c_old<pixel_t, true, true>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, maskY, maskU, maskV, maskpitch);
      else
        of_multiply_c_old<pixel_t, false, true>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, maskY, maskU, maskV, maskpitch);
    }
    else {
      if (opacity == 256)
        of_multiply_c<pixel_t, true, true>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, maskY, maskU, maskV, maskpitch);
      else
        of_multiply_c<pixel_t, false, true>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, maskY, maskU, maskV, maskpitch);
    }
  }
}

// no mask involved
template<typename pixel_t>
void OL_MultiplyImage::BlendImage(ImageOverlayInternal* base, ImageOverlayInternal* overlay) {
  pixel_t* baseY = reinterpret_cast<pixel_t*>(base->GetPtr(PLANAR_Y));
  pixel_t* baseU = reinterpret_cast<pixel_t*>(base->GetPtr(PLANAR_U));
  pixel_t* baseV = reinterpret_cast<pixel_t*>(base->GetPtr(PLANAR_V));

  pixel_t* ovY = reinterpret_cast<pixel_t*>(overlay->GetPtr(PLANAR_Y));

  const int basepitch = (base->pitch) / sizeof(pixel_t);
  const int overlaypitch = (overlay->pitch) / sizeof(pixel_t);

  int w = base->w();
  int h = base->h();
#ifdef INTEL_INTRINSICS
  if (!!(env->GetCPUFlags() & CPUF_AVX2))
  {
    if (opacity == 256)
      of_multiply_avx2<pixel_t, true, false>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, nullptr, nullptr, nullptr, 0);
    else
      of_multiply_avx2<pixel_t, false, false>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, nullptr, nullptr, nullptr, 0);
  }
  else if (!!(env->GetCPUFlags() & CPUF_SSE4_1))
  {
    if (opacity == 256)
      of_multiply_sse41<pixel_t, true, false>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, nullptr, nullptr, nullptr, 0);
    else
      of_multiply_sse41<pixel_t, false, false>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, nullptr, nullptr, nullptr, 0);
  }
  else
#endif
  {
    if (sizeof(pixel_t) == 1) {
      // old integer C code: 8 bit is quicker than float-based
      if (opacity == 256)
        of_multiply_c_old<pixel_t, true, false>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, nullptr, nullptr, nullptr, 0);
      else
        of_multiply_c_old<pixel_t, false, false>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, nullptr, nullptr, nullptr, 0);
    }
    else {
      if (opacity == 256)
        of_multiply_c<pixel_t, true, false>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, nullptr, nullptr, nullptr, 0);
      else
        of_multiply_c<pixel_t, false, false>(bits_per_pixel, opacity_f, opacity, w, h, ovY, overlaypitch, baseY, baseU, baseV, basepitch, nullptr, nullptr, nullptr, 0);
    }

  }
}
