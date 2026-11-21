// vkdk shader blobs: the deko3d shader library's entries (GLSL for uam, deflated), and the renderer's two SPIR-V rewrites
// done on that GLSL instead. Portable: the Switch build and the host packer (tools/xenosrecomp/bo2zm_pack.cpp) both use
// it. Copyright 2026 aks796. GPL-3.0.
#include <vkdk.h>

#include <zlib.h>

#include <cstring>
#include <regex>
#include <string>
#include <vector>

namespace vkdk {

std::string BlobText(const uint32_t* words, size_t count, uint32_t* stage) {
  if (!IsBlob(words, count)) {
    return {};
  }
  VkdkShaderBlobHeader h;
  memcpy(&h, words, sizeof(h));
  const size_t available = count * 4 - sizeof(h);
  if (h.version != VKDK_SHADER_BLOB_VERSION || h.stored_bytes > available || !h.glsl_bytes ||
      h.glsl_bytes > (64u << 20)) {
    return {};
  }
  const uint8_t* data = reinterpret_cast<const uint8_t*>(words) + sizeof(h);
  std::string text;
  if (h.flags & VKDK_SHADER_BLOB_ZLIB) {
    text.resize(h.glsl_bytes);
    uLongf size = h.glsl_bytes;
    if (uncompress(reinterpret_cast<Bytef*>(&text[0]), &size, data, h.stored_bytes) != Z_OK ||
        size != h.glsl_bytes) {
      return {};
    }
  } else {
    if (h.stored_bytes < h.glsl_bytes) {
      return {};
    }
    text.assign(reinterpret_cast<const char*>(data), h.glsl_bytes);
  }
  // Without the terminating NUL.
  text.resize(strnlen(text.data(), text.size()));
  if (stage) {
    *stage = h.stage;
  }
  return text;
}

std::vector<uint32_t> MakeBlob(const std::string& glsl, uint32_t stage, bool deflate) {
  VkdkShaderBlobHeader h{};
  h.magic = VKDK_SHADER_BLOB_MAGIC;
  h.version = VKDK_SHADER_BLOB_VERSION;
  h.stage = stage;
  h.glsl_bytes = uint32_t(glsl.size() + 1);
  std::vector<uint8_t> stored(glsl.begin(), glsl.end());
  stored.push_back(0);
  if (deflate) {
    uLongf size = compressBound(uLong(stored.size()));
    std::vector<uint8_t> packed(size);
    if (compress2(packed.data(), &size, stored.data(), uLong(stored.size()), 9) == Z_OK && size < stored.size()) {
      packed.resize(size);
      stored.swap(packed);
      h.flags |= VKDK_SHADER_BLOB_ZLIB;
    }
  }
  h.stored_bytes = uint32_t(stored.size());
  std::vector<uint32_t> out((sizeof(h) + stored.size() + 3) / 4, 0);
  memcpy(out.data(), &h, sizeof(h));
  memcpy(reinterpret_cast<uint8_t*>(out.data()) + sizeof(h), stored.data(), stored.size());
  return out;
}

std::vector<uint32_t> BlobPruneColorWrites(const std::vector<uint32_t>& blob, uint32_t& removed) {
  removed = 0;
  uint32_t stage = 0;
  std::string text = BlobText(blob.data(), blob.size(), &stage);
  if (text.empty() || stage != 4) {
    return {};
  }
  // The colour targets' outputs (locations 0-3, no index: gl_FragDepth and the second blend source stay) become plain
  // globals: their writes are dead and the compiler drops them, as the SPIR-V pass drops the OpStores.
  static const std::regex kOutput(R"(layout\(location = ([0-3])\) out (\w+) (\w+);)");
  std::string out;
  out.reserve(text.size());
  auto begin = text.cbegin();
  for (std::sregex_iterator it(text.begin(), text.end(), kOutput), end; it != end; ++it) {
    out.append(begin, (*it)[0].first);
    out += (*it)[2].str() + " " + (*it)[3].str() + ";";
    begin = (*it)[0].second;
    ++removed;
  }
  if (!removed) {
    return {};
  }
  out.append(begin, text.cend());
  return MakeBlob(out, stage);
}

std::vector<uint32_t> BlobWithTestsEarly(const std::vector<uint32_t>& blob, const char*& reason) {
  reason = "";
  uint32_t stage = 0;
  std::string text = BlobText(blob.data(), blob.size(), &stage);
  if (text.empty()) {
    reason = "not a vkdk blob";
    return {};
  }
  if (stage != 4) {
    reason = "no fragment entry point";
    return {};
  }
  if (text.find("early_fragment_tests") != std::string::npos) {
    reason = "already declared it";
    return {};
  }
  if (text.find("gl_FragDepth") != std::string::npos) {
    reason = "writes gl_FragDepth";
    return {};
  }
  // After the directives at the top (#version, #extension, #define of the bindless handle, comments).
  size_t at = 0;
  for (;;) {
    const size_t nl = text.find('\n', at);
    if (nl == std::string::npos) break;
    const char c = text[at];
    if (c == '#' || c == '/' || c == '\n') {
      at = nl + 1;
      continue;
    }
    break;
  }
  text.insert(at, "layout(early_fragment_tests) in;\n");
  return MakeBlob(text, stage);
}

bool BlobHasConstant(const std::vector<uint32_t>& blob, uint32_t value) {
  const std::string text = BlobText(blob.data(), blob.size());
  if (text.empty()) {
    return false;
  }
  // SPIRV-Cross writes 32-bit constants in decimal with a u suffix, or in hex.
  char dec[24], hex[24];
  snprintf(dec, sizeof(dec), "%uu", value);
  snprintf(hex, sizeof(hex), "0x%08xu", value);
  return text.find(dec) != std::string::npos || text.find(hex) != std::string::npos;
}

}  // namespace vkdk
