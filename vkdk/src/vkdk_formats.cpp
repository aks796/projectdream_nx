// vkdk: Vulkan formats and enums to deko3d's. Tables follow renut-nx's plume deko3d back end (MIT), with Vulkan's
// names. Copyright 2026 aks796. GPL-3.0.
#include "vkdk_internal.h"

#include <cstdio>
#include <unordered_map>
#include <string>
#include <mutex>

namespace vkdk {

DkImageFormat ToDkFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R8_UNORM: return DkImageFormat_R8_Unorm;
    case VK_FORMAT_R8_SNORM: return DkImageFormat_R8_Snorm;
    case VK_FORMAT_R8_UINT: return DkImageFormat_R8_Uint;
    case VK_FORMAT_R8_SINT: return DkImageFormat_R8_Sint;
    case VK_FORMAT_R16_SFLOAT: return DkImageFormat_R16_Float;
    case VK_FORMAT_R16_UNORM: return DkImageFormat_R16_Unorm;
    case VK_FORMAT_R16_SNORM: return DkImageFormat_R16_Snorm;
    case VK_FORMAT_R16_UINT: return DkImageFormat_R16_Uint;
    case VK_FORMAT_R16_SINT: return DkImageFormat_R16_Sint;
    case VK_FORMAT_R32_SFLOAT: return DkImageFormat_R32_Float;
    case VK_FORMAT_R32_UINT: return DkImageFormat_R32_Uint;
    case VK_FORMAT_R32_SINT: return DkImageFormat_R32_Sint;
    case VK_FORMAT_R8G8_UNORM: return DkImageFormat_RG8_Unorm;
    case VK_FORMAT_R8G8_SNORM: return DkImageFormat_RG8_Snorm;
    case VK_FORMAT_R8G8_UINT: return DkImageFormat_RG8_Uint;
    case VK_FORMAT_R8G8_SINT: return DkImageFormat_RG8_Sint;
    case VK_FORMAT_R16G16_SFLOAT: return DkImageFormat_RG16_Float;
    case VK_FORMAT_R16G16_UNORM: return DkImageFormat_RG16_Unorm;
    case VK_FORMAT_R16G16_SNORM: return DkImageFormat_RG16_Snorm;
    case VK_FORMAT_R16G16_UINT: return DkImageFormat_RG16_Uint;
    case VK_FORMAT_R16G16_SINT: return DkImageFormat_RG16_Sint;
    case VK_FORMAT_R32G32_SFLOAT: return DkImageFormat_RG32_Float;
    case VK_FORMAT_R32G32_UINT: return DkImageFormat_RG32_Uint;
    case VK_FORMAT_R32G32_SINT: return DkImageFormat_RG32_Sint;
    case VK_FORMAT_R32G32B32_SFLOAT: return DkImageFormat_RGB32_Float;
    case VK_FORMAT_R32G32B32_UINT: return DkImageFormat_RGB32_Uint;
    case VK_FORMAT_R32G32B32_SINT: return DkImageFormat_RGB32_Sint;
    case VK_FORMAT_R8G8B8A8_UNORM: return DkImageFormat_RGBA8_Unorm;
    case VK_FORMAT_R8G8B8A8_SNORM: return DkImageFormat_RGBA8_Snorm;
    case VK_FORMAT_R8G8B8A8_UINT: return DkImageFormat_RGBA8_Uint;
    case VK_FORMAT_R8G8B8A8_SINT: return DkImageFormat_RGBA8_Sint;
    case VK_FORMAT_R8G8B8A8_SRGB: return DkImageFormat_RGBA8_Unorm_sRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return DkImageFormat_BGRA8_Unorm;
    case VK_FORMAT_B8G8R8A8_SRGB: return DkImageFormat_BGRA8_Unorm_sRGB;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return DkImageFormat_RGBA16_Float;
    case VK_FORMAT_R16G16B16A16_UNORM: return DkImageFormat_RGBA16_Unorm;
    case VK_FORMAT_R16G16B16A16_SNORM: return DkImageFormat_RGBA16_Snorm;
    case VK_FORMAT_R16G16B16A16_UINT: return DkImageFormat_RGBA16_Uint;
    case VK_FORMAT_R16G16B16A16_SINT: return DkImageFormat_RGBA16_Sint;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return DkImageFormat_RGBA32_Float;
    case VK_FORMAT_R32G32B32A32_UINT: return DkImageFormat_RGBA32_Uint;
    case VK_FORMAT_R32G32B32A32_SINT: return DkImageFormat_RGBA32_Sint;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DkImageFormat_RGB10A2_Unorm;
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return DkImageFormat_RGB10A2_Uint;
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return DkImageFormat_RG11B10_Float;
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: return DkImageFormat_E5BGR9_Float;
    // Packed 16-bit formats: Vulkan names them from the top bit down, deko3d from bit 0 up (its RGBA4 has red in bits
    // 0-3, the 3DS's and Vulkan's R4G4B4A4 have it in bits 12-15). Dekopon-32 round h: R5G6B5 went to RGB565
    // (red and blue swapped, and not a render target on deko3d) and R4G4B4A4 to RGBA4 (every channel reversed:
    // transparent white sampled as opaque cyan in NES Remix's dialogs).
    case VK_FORMAT_R5G6B5_UNORM_PACK16: return DkImageFormat_BGR565_Unorm;
    case VK_FORMAT_B5G6R5_UNORM_PACK16: return DkImageFormat_RGB565_Unorm;
    case VK_FORMAT_A1R5G5B5_UNORM_PACK16: return DkImageFormat_BGR5A1_Unorm;
    case VK_FORMAT_R5G5B5A1_UNORM_PACK16: return DkImageFormat_A5BGR5_Unorm;  // sampled only
    case VK_FORMAT_A4B4G4R4_UNORM_PACK16: return DkImageFormat_RGBA4_Unorm;   // sampled only
    case VK_FORMAT_R4G4B4A4_UNORM_PACK16: return DkImageFormat_RGBA4_Unorm;   // sampled only, swizzled (FormatSwizzle)
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: return DkImageFormat_RGB_BC1;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return DkImageFormat_RGBA_BC1;
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return DkImageFormat_RGBA_BC1_sRGB;
    case VK_FORMAT_BC2_UNORM_BLOCK: return DkImageFormat_RGBA_BC2;
    case VK_FORMAT_BC2_SRGB_BLOCK: return DkImageFormat_RGBA_BC2_sRGB;
    case VK_FORMAT_BC3_UNORM_BLOCK: return DkImageFormat_RGBA_BC3;
    case VK_FORMAT_BC3_SRGB_BLOCK: return DkImageFormat_RGBA_BC3_sRGB;
    case VK_FORMAT_BC4_UNORM_BLOCK: return DkImageFormat_R_BC4_Unorm;
    case VK_FORMAT_BC4_SNORM_BLOCK: return DkImageFormat_R_BC4_Snorm;
    case VK_FORMAT_BC5_UNORM_BLOCK: return DkImageFormat_RG_BC5_Unorm;
    case VK_FORMAT_BC5_SNORM_BLOCK: return DkImageFormat_RG_BC5_Snorm;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: return DkImageFormat_RGBA_BC6H_UF16_Float;
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: return DkImageFormat_RGBA_BC6H_SF16_Float;
    case VK_FORMAT_BC7_UNORM_BLOCK: return DkImageFormat_RGBA_BC7_Unorm;
    case VK_FORMAT_BC7_SRGB_BLOCK: return DkImageFormat_RGBA_BC7_Unorm_sRGB;
    // Depth: the console's own 24-bit depth for D24S8 (renut-nx: 4 bytes a pixel, not ZF32_X24S8's 8).
    case VK_FORMAT_D16_UNORM: return DkImageFormat_Z16;
    case VK_FORMAT_X8_D24_UNORM_PACK32: return DkImageFormat_Z24X8;
    case VK_FORMAT_D24_UNORM_S8_UINT: return DkImageFormat_Z24S8;
    case VK_FORMAT_D32_SFLOAT: return DkImageFormat_ZF32;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return DkImageFormat_ZF32_X24S8;
    case VK_FORMAT_S8_UINT: return DkImageFormat_S8;
    default: return DkImageFormat_None;
  }
}

bool IsDepthFormat(VkFormat f) {
  switch (f) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
    case VK_FORMAT_S8_UINT:
      return true;
    default:
      return false;
  }
}

bool HasStencil(VkFormat f) {
  return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT;
}

bool IsCompressed(DkImageFormat f) {
  return f >= DkImageFormat_RGB_BC1 && f <= DkImageFormat_RGBA_ASTC_12x12_sRGB;
}

bool CanUse2DEngine(DkImageFormat f) {
  // Depth formats too (deko3d's format table allows them: Z24S8 goes through as 32-bit colour, Z16 as R16), for depth
  // blits (round j). S8 has no 2D format.
  return f != DkImageFormat_S8 && !IsCompressed(f) && f != DkImageFormat_RGB32_Float &&
         f != DkImageFormat_RGB32_Uint && f != DkImageFormat_RGB32_Sint;
}

// Bytes of one texel, or of one block for block-compressed formats (with its size).
uint32_t FormatBlockBytes(VkFormat f, uint32_t& bw, uint32_t& bh) {
  bw = bh = 1;
  switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_SNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT:
    case VK_FORMAT_S8_UINT:
      return 1;
    case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SNORM: case VK_FORMAT_R16_UINT:
    case VK_FORMAT_R16_SINT: case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R8G8_SNORM: case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R8G8_SINT: case VK_FORMAT_D16_UNORM: case VK_FORMAT_R5G6B5_UNORM_PACK16:
    case VK_FORMAT_B5G6R5_UNORM_PACK16: case VK_FORMAT_A1R5G5B5_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
      return 2;
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R16G16_SFLOAT:
    case VK_FORMAT_R16G16_UNORM: case VK_FORMAT_R16G16_SNORM: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT:
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SNORM: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_A2B10G10R10_UNORM_PACK32: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT:
      return 4;
    case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R16G16B16A16_SNORM:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return 8;
    case VK_FORMAT_R32G32B32_SFLOAT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT:
      return 12;
    case VK_FORMAT_R32G32B32A32_SFLOAT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
      return 16;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK: case VK_FORMAT_BC4_SNORM_BLOCK:
      bw = bh = 4;
      return 8;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC5_UNORM_BLOCK: case VK_FORMAT_BC5_SNORM_BLOCK:
    case VK_FORMAT_BC6H_UFLOAT_BLOCK: case VK_FORMAT_BC6H_SFLOAT_BLOCK: case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK:
      bw = bh = 4;
      return 16;
    default:
      return 0;
  }
}

bool VertexFormat(VkFormat f, DkVtxAttribSize& size, DkVtxAttribType& type, bool& bgra) {
  bgra = false;
  auto set = [&](DkVtxAttribSize s, DkVtxAttribType t) {
    size = s;
    type = t;
    return true;
  };
  switch (f) {
    // Dekopon-32: every 8- and 16-bit layout the PICA vertex loader can ask for (Vulkan renderer attrib table).
    case VK_FORMAT_R8_SNORM: return set(DkVtxAttribSize_1x8, DkVtxAttribType_Snorm);
    case VK_FORMAT_R8_USCALED: return set(DkVtxAttribSize_1x8, DkVtxAttribType_Uscaled);
    case VK_FORMAT_R8_SSCALED: return set(DkVtxAttribSize_1x8, DkVtxAttribType_Sscaled);
    case VK_FORMAT_R8_UINT: return set(DkVtxAttribSize_1x8, DkVtxAttribType_Uint);
    case VK_FORMAT_R8_SINT: return set(DkVtxAttribSize_1x8, DkVtxAttribType_Sint);
    case VK_FORMAT_R8G8_USCALED: return set(DkVtxAttribSize_2x8, DkVtxAttribType_Uscaled);
    case VK_FORMAT_R8G8_SSCALED: return set(DkVtxAttribSize_2x8, DkVtxAttribType_Sscaled);
    case VK_FORMAT_R8G8_UINT: return set(DkVtxAttribSize_2x8, DkVtxAttribType_Uint);
    case VK_FORMAT_R8G8_SINT: return set(DkVtxAttribSize_2x8, DkVtxAttribType_Sint);
    case VK_FORMAT_R8G8B8_UNORM: return set(DkVtxAttribSize_3x8, DkVtxAttribType_Unorm);
    case VK_FORMAT_R8G8B8_SNORM: return set(DkVtxAttribSize_3x8, DkVtxAttribType_Snorm);
    case VK_FORMAT_R8G8B8_USCALED: return set(DkVtxAttribSize_3x8, DkVtxAttribType_Uscaled);
    case VK_FORMAT_R8G8B8_SSCALED: return set(DkVtxAttribSize_3x8, DkVtxAttribType_Sscaled);
    case VK_FORMAT_R8G8B8_UINT: return set(DkVtxAttribSize_3x8, DkVtxAttribType_Uint);
    case VK_FORMAT_R8G8B8_SINT: return set(DkVtxAttribSize_3x8, DkVtxAttribType_Sint);
    case VK_FORMAT_R16_SNORM: return set(DkVtxAttribSize_1x16, DkVtxAttribType_Snorm);
    case VK_FORMAT_R16_USCALED: return set(DkVtxAttribSize_1x16, DkVtxAttribType_Uscaled);
    case VK_FORMAT_R16_SSCALED: return set(DkVtxAttribSize_1x16, DkVtxAttribType_Sscaled);
    case VK_FORMAT_R16_UINT: return set(DkVtxAttribSize_1x16, DkVtxAttribType_Uint);
    case VK_FORMAT_R16_SINT: return set(DkVtxAttribSize_1x16, DkVtxAttribType_Sint);
    case VK_FORMAT_R16G16B16_UNORM: return set(DkVtxAttribSize_3x16, DkVtxAttribType_Unorm);
    case VK_FORMAT_R16G16B16_SNORM: return set(DkVtxAttribSize_3x16, DkVtxAttribType_Snorm);
    case VK_FORMAT_R16G16B16_USCALED: return set(DkVtxAttribSize_3x16, DkVtxAttribType_Uscaled);
    case VK_FORMAT_R16G16B16_SSCALED: return set(DkVtxAttribSize_3x16, DkVtxAttribType_Sscaled);
    case VK_FORMAT_R16G16B16_UINT: return set(DkVtxAttribSize_3x16, DkVtxAttribType_Uint);
    case VK_FORMAT_R16G16B16_SINT: return set(DkVtxAttribSize_3x16, DkVtxAttribType_Sint);
    case VK_FORMAT_R16G16B16_SFLOAT: return set(DkVtxAttribSize_3x16, DkVtxAttribType_Float);
    case VK_FORMAT_R32G32B32A32_SFLOAT: return set(DkVtxAttribSize_4x32, DkVtxAttribType_Float);
    case VK_FORMAT_R32G32B32A32_UINT: return set(DkVtxAttribSize_4x32, DkVtxAttribType_Uint);
    case VK_FORMAT_R32G32B32A32_SINT: return set(DkVtxAttribSize_4x32, DkVtxAttribType_Sint);
    case VK_FORMAT_R32G32B32_SFLOAT: return set(DkVtxAttribSize_3x32, DkVtxAttribType_Float);
    case VK_FORMAT_R32G32B32_UINT: return set(DkVtxAttribSize_3x32, DkVtxAttribType_Uint);
    case VK_FORMAT_R32G32B32_SINT: return set(DkVtxAttribSize_3x32, DkVtxAttribType_Sint);
    case VK_FORMAT_R32G32_SFLOAT: return set(DkVtxAttribSize_2x32, DkVtxAttribType_Float);
    case VK_FORMAT_R32G32_UINT: return set(DkVtxAttribSize_2x32, DkVtxAttribType_Uint);
    case VK_FORMAT_R32G32_SINT: return set(DkVtxAttribSize_2x32, DkVtxAttribType_Sint);
    case VK_FORMAT_R32_SFLOAT: return set(DkVtxAttribSize_1x32, DkVtxAttribType_Float);
    case VK_FORMAT_R32_UINT: return set(DkVtxAttribSize_1x32, DkVtxAttribType_Uint);
    case VK_FORMAT_R32_SINT: return set(DkVtxAttribSize_1x32, DkVtxAttribType_Sint);
    case VK_FORMAT_R16G16B16A16_SFLOAT: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Float);
    case VK_FORMAT_R16G16B16A16_UNORM: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Unorm);
    case VK_FORMAT_R16G16B16A16_SNORM: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Snorm);
    case VK_FORMAT_R16G16B16A16_UINT: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Uint);
    case VK_FORMAT_R16G16B16A16_SINT: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Sint);
    case VK_FORMAT_R16G16B16A16_USCALED: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Uscaled);
    case VK_FORMAT_R16G16B16A16_SSCALED: return set(DkVtxAttribSize_4x16, DkVtxAttribType_Sscaled);
    case VK_FORMAT_R16G16_SFLOAT: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Float);
    case VK_FORMAT_R16G16_UNORM: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Unorm);
    case VK_FORMAT_R16G16_SNORM: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Snorm);
    case VK_FORMAT_R16G16_UINT: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Uint);
    case VK_FORMAT_R16G16_SINT: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Sint);
    case VK_FORMAT_R16G16_USCALED: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Uscaled);
    case VK_FORMAT_R16G16_SSCALED: return set(DkVtxAttribSize_2x16, DkVtxAttribType_Sscaled);
    case VK_FORMAT_R16_SFLOAT: return set(DkVtxAttribSize_1x16, DkVtxAttribType_Float);
    case VK_FORMAT_R16_UNORM: return set(DkVtxAttribSize_1x16, DkVtxAttribType_Unorm);
    case VK_FORMAT_R8G8B8A8_UNORM: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Unorm);
    case VK_FORMAT_R8G8B8A8_SNORM: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Snorm);
    case VK_FORMAT_R8G8B8A8_UINT: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Uint);
    case VK_FORMAT_R8G8B8A8_SINT: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Sint);
    case VK_FORMAT_R8G8B8A8_USCALED: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Uscaled);
    case VK_FORMAT_R8G8B8A8_SSCALED: return set(DkVtxAttribSize_4x8, DkVtxAttribType_Sscaled);
    case VK_FORMAT_B8G8R8A8_UNORM: bgra = true; return set(DkVtxAttribSize_4x8, DkVtxAttribType_Unorm);
    case VK_FORMAT_B8G8R8A8_SNORM: bgra = true; return set(DkVtxAttribSize_4x8, DkVtxAttribType_Snorm);
    case VK_FORMAT_B8G8R8A8_UINT: bgra = true; return set(DkVtxAttribSize_4x8, DkVtxAttribType_Uint);
    case VK_FORMAT_B8G8R8A8_SINT: bgra = true; return set(DkVtxAttribSize_4x8, DkVtxAttribType_Sint);
    case VK_FORMAT_B8G8R8A8_USCALED: bgra = true; return set(DkVtxAttribSize_4x8, DkVtxAttribType_Uscaled);
    case VK_FORMAT_B8G8R8A8_SSCALED: bgra = true; return set(DkVtxAttribSize_4x8, DkVtxAttribType_Sscaled);
    case VK_FORMAT_R8G8_UNORM: return set(DkVtxAttribSize_2x8, DkVtxAttribType_Unorm);
    case VK_FORMAT_R8G8_SNORM: return set(DkVtxAttribSize_2x8, DkVtxAttribType_Snorm);
    case VK_FORMAT_R8_UNORM: return set(DkVtxAttribSize_1x8, DkVtxAttribType_Unorm);
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Unorm);
    case VK_FORMAT_A2B10G10R10_SNORM_PACK32: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Snorm);
    case VK_FORMAT_A2B10G10R10_UINT_PACK32: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Uint);
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Sint);
    case VK_FORMAT_A2B10G10R10_USCALED_PACK32: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Uscaled);
    case VK_FORMAT_A2B10G10R10_SSCALED_PACK32: return set(DkVtxAttribSize_10_10_10_2, DkVtxAttribType_Sscaled);
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return set(DkVtxAttribSize_11_11_10, DkVtxAttribType_Float);
    default: return false;
  }
}

DkCompareOp ToDk(VkCompareOp op) {
  switch (op) {
    case VK_COMPARE_OP_NEVER: return DkCompareOp_Never;
    case VK_COMPARE_OP_LESS: return DkCompareOp_Less;
    case VK_COMPARE_OP_EQUAL: return DkCompareOp_Equal;
    case VK_COMPARE_OP_LESS_OR_EQUAL: return DkCompareOp_Lequal;
    case VK_COMPARE_OP_GREATER: return DkCompareOp_Greater;
    case VK_COMPARE_OP_NOT_EQUAL: return DkCompareOp_NotEqual;
    case VK_COMPARE_OP_GREATER_OR_EQUAL: return DkCompareOp_Gequal;
    default: return DkCompareOp_Always;
  }
}

DkStencilOp ToDk(VkStencilOp op) {
  switch (op) {
    case VK_STENCIL_OP_ZERO: return DkStencilOp_Zero;
    case VK_STENCIL_OP_REPLACE: return DkStencilOp_Replace;
    case VK_STENCIL_OP_INCREMENT_AND_CLAMP: return DkStencilOp_Incr;
    case VK_STENCIL_OP_DECREMENT_AND_CLAMP: return DkStencilOp_Decr;
    case VK_STENCIL_OP_INVERT: return DkStencilOp_Invert;
    case VK_STENCIL_OP_INCREMENT_AND_WRAP: return DkStencilOp_IncrWrap;
    case VK_STENCIL_OP_DECREMENT_AND_WRAP: return DkStencilOp_DecrWrap;
    default: return DkStencilOp_Keep;
  }
}

DkBlendFactor ToDk(VkBlendFactor f) {
  switch (f) {
    case VK_BLEND_FACTOR_ZERO: return DkBlendFactor_Zero;
    case VK_BLEND_FACTOR_ONE: return DkBlendFactor_One;
    case VK_BLEND_FACTOR_SRC_COLOR: return DkBlendFactor_SrcColor;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return DkBlendFactor_InvSrcColor;
    case VK_BLEND_FACTOR_DST_COLOR: return DkBlendFactor_DstColor;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR: return DkBlendFactor_InvDstColor;
    case VK_BLEND_FACTOR_SRC_ALPHA: return DkBlendFactor_SrcAlpha;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return DkBlendFactor_InvSrcAlpha;
    case VK_BLEND_FACTOR_DST_ALPHA: return DkBlendFactor_DstAlpha;
    case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA: return DkBlendFactor_InvDstAlpha;
    case VK_BLEND_FACTOR_CONSTANT_COLOR: return DkBlendFactor_ConstColor;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR: return DkBlendFactor_InvConstColor;
    case VK_BLEND_FACTOR_CONSTANT_ALPHA: return DkBlendFactor_ConstAlpha;
    case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA: return DkBlendFactor_InvConstAlpha;
    case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE: return DkBlendFactor_SrcAlphaSaturate;
    case VK_BLEND_FACTOR_SRC1_COLOR: return DkBlendFactor_Src1Color;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR: return DkBlendFactor_InvSrc1Color;
    case VK_BLEND_FACTOR_SRC1_ALPHA: return DkBlendFactor_Src1Alpha;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA: return DkBlendFactor_InvSrc1Alpha;
    default: return DkBlendFactor_One;
  }
}

DkBlendOp ToDk(VkBlendOp op) {
  switch (op) {
    case VK_BLEND_OP_SUBTRACT: return DkBlendOp_Sub;
    case VK_BLEND_OP_REVERSE_SUBTRACT: return DkBlendOp_RevSub;
    case VK_BLEND_OP_MIN: return DkBlendOp_Min;
    case VK_BLEND_OP_MAX: return DkBlendOp_Max;
    default: return DkBlendOp_Add;
  }
}

DkWrapMode ToDk(VkSamplerAddressMode m) {
  switch (m) {
    case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT: return DkWrapMode_MirroredRepeat;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE: return DkWrapMode_ClampToEdge;
    case VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER: return DkWrapMode_ClampToBorder;
    case VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE: return DkWrapMode_MirrorClampToEdge;
    default: return DkWrapMode_Repeat;
  }
}

DkPrimitive ToDk(VkPrimitiveTopology t) {
  switch (t) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST: return DkPrimitive_Points;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST: return DkPrimitive_Lines;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP: return DkPrimitive_LineStrip;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP: return DkPrimitive_TriangleStrip;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN: return DkPrimitive_TriangleFan;
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY: return DkPrimitive_LinesAdjacency;
    case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY: return DkPrimitive_LineStripAdjacency;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY: return DkPrimitive_TrianglesAdjacency;
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY: return DkPrimitive_TriangleStripAdjacency;
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST: return DkPrimitive_Patches;
    default: return DkPrimitive_Triangles;
  }
}

DkImageSwizzle ToDk(VkComponentSwizzle s, DkImageSwizzle identity) {
  switch (s) {
    case VK_COMPONENT_SWIZZLE_ZERO: return DkImageSwizzle_Zero;
    case VK_COMPONENT_SWIZZLE_ONE: return DkImageSwizzle_One;
    case VK_COMPONENT_SWIZZLE_R: return DkImageSwizzle_Red;
    case VK_COMPONENT_SWIZZLE_G: return DkImageSwizzle_Green;
    case VK_COMPONENT_SWIZZLE_B: return DkImageSwizzle_Blue;
    case VK_COMPONENT_SWIZZLE_A: return DkImageSwizzle_Alpha;
    default: return identity;
  }
}

// deko3d formats the 3D engine cannot draw into (no colour target format in deko3d's format table): RGBA4, the
// 5-5-5 ones in RGB order, RGB565, E5BGR9, 96-bit RGB. Binding one as a target draws garbage, so a Vulkan user must be
// told (Azahar then keeps those surfaces in RGBA8).
bool DebugSwitch(const char* name, bool partOfSafe) {
  static std::mutex mutex;
  static std::unordered_map<std::string, bool> known;
  std::lock_guard<std::mutex> lock(mutex);
  auto it = known.find(name);
  if (it != known.end()) {
    return it->second;
  }
  auto exists = [](const std::string& file) {
    std::FILE* f = std::fopen(("sdmc:/switch/projectdream_nx/" + file).c_str(), "r");
    if (f) std::fclose(f);
    return f != nullptr;
  };
  const bool on = exists(name) || (partOfSafe && exists("vkdk_safe"));
  known.emplace(name, on);
  if (on) {
    Log("switches: %s on", name);
  }
  return on;
}

bool Renderable(DkImageFormat f) {
  // sdmc:/switch/projectdream_nx/vkdk_565_rgba8: BGR565 (Vulkan's R5G6B5) not offered as a target either, so Azahar keeps
  // 565 surfaces in RGBA8 (a fallback for an A/B check; in Ryubing a 565 target filled and display-transferred by a
  // test .3dsx gave the same picture both ways, and NVK uses the same hardware format pairing on the console).
  static const bool no565 = [] {
    std::FILE* file = std::fopen("sdmc:/switch/projectdream_nx/vkdk_565_rgba8", "r");
    if (file) {
      std::fclose(file);
      Log("formats: 565 surfaces kept in RGBA8 (vkdk_565_rgba8)");
    }
    return file != nullptr;
  }();
  if (no565 && f == DkImageFormat_BGR565_Unorm) {
    return false;
  }
  switch (f) {
    case DkImageFormat_RGBA4_Unorm: case DkImageFormat_RGB5_Unorm: case DkImageFormat_RGB5A1_Unorm:
    case DkImageFormat_RGB565_Unorm: case DkImageFormat_A5BGR5_Unorm: case DkImageFormat_E5BGR9_Float:
    case DkImageFormat_RGB32_Float: case DkImageFormat_RGB32_Uint: case DkImageFormat_RGB32_Sint:
      return false;
    default:
      return !IsCompressed(f);
  }
}

// The channel order a format's view needs on top of the caller's swizzle, for Vulkan formats deko3d only has in
// another bit order. R4G4B4A4 (red in bits 12-15) read through deko3d's RGBA4 (red in bits 0-3): red is deko3d's
// alpha, green its blue, blue its green, alpha its red.
bool FormatSwizzle(VkFormat format, DkImageSwizzle out[4]) {
  if (format == VK_FORMAT_R4G4B4A4_UNORM_PACK16) {
    out[0] = DkImageSwizzle_Alpha;
    out[1] = DkImageSwizzle_Blue;
    out[2] = DkImageSwizzle_Green;
    out[3] = DkImageSwizzle_Red;
    return true;
  }
  return false;
}

uint32_t FormatFeatures(VkFormat format, bool optimal) {
  const DkImageFormat dk = ToDkFormat(format);
  uint32_t features = 0;
  if (dk != DkImageFormat_None && optimal) {
    features |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if (IsDepthFormat(format)) {
      features |= VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
      // Depth blits as raw texels through the 2D engine (deko3d gives depth formats a same-size 2D format: Z24S8 as
      // 32-bit colour, Z16 as R16), nearest only (vkdk_cmd.cpp CmdBlitImage). Dekopon-32 round j: without it Azahar
      // could not scale a depth buffer to 2x ("Depth scale unsupported by hardware") and its draw-based fallback
      // needs stencil export, so a reloaded depth buffer kept stale contents at 2x.
      features |= VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    } else if (Renderable(dk)) {
      features |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                  VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
      if (CanUse2DEngine(dk)) {
        features |= VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
      }
    }
  }
  return features;
}

}  // namespace vkdk
