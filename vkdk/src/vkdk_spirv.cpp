// vkdk: SPIR-V shader modules, converted on the console (Dekopon-32).
//
// bo2-360-nx gives vkdk GLSL blobs made at build time; an emulator makes its shaders while it runs (Dekopon's PICA
// shaders, one per register configuration), so here vkCreateShaderModule takes plain SPIR-V and converts it the way
// NaGaa95's NetherSX2_nx / DrasticDS_nx deko3d back ends do: SPIRV-Cross to GLSL 4.60, with every resource rebound to
// deko3d's per-stage slots and the "// vkdk" line naming the Vulkan (set, binding, element) each slot comes from
// (the format tools/deko3d/bo2_dk_glsl.py writes; ShaderMeta::Parse reads it). uam compiles the text later, on vkdk's
// worker thread, when a pipeline needs the variant (and the DKSH cache keeps it).
//
// Slots: uniform buffers 0-15 (5 is the push constants' block, vkdk_internal.h kPushUboSlot), storage buffers 0-15,
// textures 0-31 (a combined image sampler, a texel buffer, or an image read with texelFetch: one handle each),
// storage images 0-7.
//
// Copyright 2026 aks796. GPL-3.0, like the rest of vkdk.

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <spirv_cross/spirv_glsl.hpp>
#include <sys/stat.h>
#include <switch.h>

#include "vkdk_internal.h"

namespace vkdk {

namespace {

constexpr uint32_t kMaxUbo = 16;
constexpr uint32_t kMaxSsbo = 16;
constexpr uint32_t kMaxTex = 32;
constexpr uint32_t kMaxImg = 8;

struct Ref {
  uint32_t set, binding, element;
};

std::string RefText(const Ref& r) {
  return std::to_string(r.set) + "." + std::to_string(r.binding) + "." + std::to_string(r.element);
}

}  // namespace

namespace {
std::shared_ptr<const ShaderSource> ConvertSpirvHere(const uint32_t* code, size_t bytes, std::string& error);
}  // namespace

// SPIRV-Cross recurses deeply on big shaders (Dekopon's PICA fragment shaders overflowed a renderer thread's
// stack in Ryubing): it runs on vkdk's 8 MB worker thread, which uam uses too.
static std::shared_ptr<const ShaderSource> ConvertSpirvOnThread(const uint32_t* code, size_t bytes,
                                                                std::string& error) {
  // No running it here instead: a renderer thread's 128 KB stack overflows in SPIRV-Cross.
  std::shared_ptr<const ShaderSource> result;
  if (!RunOnBigStack([&] { result = ConvertSpirvHere(code, bytes, error); })) {
    error = "vkdk's shader thread is not running";
    return nullptr;
  }
  return result;
}

// The conversions' cost on the console: a line per 64 shaders (an emulator makes them while games run, and again on
// every boot from its own shader cache).
std::shared_ptr<const ShaderSource> ConvertSpirv(const uint32_t* code, size_t bytes, std::string& error) {
  static uint32_t count = 0;
  static uint64_t totalNs = 0, maxNs = 0;
  const uint64_t t0 = armTicksToNs(armGetSystemTick());
  auto result = ConvertSpirvOnThread(code, bytes, error);
  const uint64_t ns = armTicksToNs(armGetSystemTick()) - t0;
  static std::mutex statsMutex;  // pipelines are made on several worker threads
  std::lock_guard<std::mutex> lock(statsMutex);
  totalNs += ns;
  maxNs = ns > maxNs ? ns : maxNs;
  if (++count % 64 == 0 || count == 8) {
    Log("spirv: %u shaders converted to GLSL, %.1f ms each on average, slowest %.1f ms", count,
        totalNs / 1e6 / count, maxNs / 1e6);
  }
  return result;
}

namespace {
std::shared_ptr<const ShaderSource> ConvertSpirvHere(const uint32_t* code, size_t bytes, std::string& error) {
  try {
    spirv_cross::CompilerGLSL c(code, bytes / 4);
    const auto entries = c.get_entry_points_and_stages();
    if (entries.empty()) {
      error = "no entry point";
      return nullptr;
    }
    DkStage stage;
    const char* tag;
    switch (entries[0].execution_model) {
      case spv::ExecutionModelVertex: stage = DkStage_Vertex; tag = "vs"; break;
      case spv::ExecutionModelFragment: stage = DkStage_Fragment; tag = "ps"; break;
      case spv::ExecutionModelGLCompute: stage = DkStage_Compute; tag = "cs"; break;
      default: error = "only vertex, fragment and compute shaders"; return nullptr;
    }
    c.set_entry_point(entries[0].name, entries[0].execution_model);

    spirv_cross::CompilerGLSL::Options o = c.get_common_options();
    o.version = 460;
    o.es = false;
    o.vulkan_semantics = false;
    o.separate_shader_objects = true;
    o.enable_420pack_extension = true;  // layout(binding = N)
    o.emit_push_constant_as_uniform_buffer = true;
    o.emit_uniform_buffer_as_plain_uniforms = false;
    o.vertex.fixup_clipspace = false;   // vkdk keeps Vulkan's clip space (flipped viewport)
    o.vertex.flip_vert_y = false;
    c.set_common_options(o);

    // One deko3d handle per texture slot: separate images and samplers are combined, and an image read without a
    // sampler (texelFetch) gets a dummy one, which the vkdk line marks "-".
    const spirv_cross::VariableID dummy = c.build_dummy_sampler_for_combined_images();
    c.build_combined_image_samplers();

    const spirv_cross::ShaderResources res = c.get_shader_resources();
    std::vector<std::string> ubo_f, ssbo_f, tex_f, img_f;
    bool push = false;

    const auto ref_of = [&](spirv_cross::ID id, uint32_t element) {
      return Ref{c.get_decoration(id, spv::DecorationDescriptorSet), c.get_decoration(id, spv::DecorationBinding),
                 element};
    };
    const auto rebind = [&](spirv_cross::ID id, uint32_t slot) {
      c.unset_decoration(id, spv::DecorationDescriptorSet);
      c.set_decoration(id, spv::DecorationBinding, slot);
    };
    const auto count_of = [&](spirv_cross::TypeID type_id) {
      const spirv_cross::SPIRType& t = c.get_type(type_id);
      uint32_t n = 1;
      for (size_t i = 0; i < t.array.size(); ++i) {
        n *= t.array[i] ? t.array[i] : 1;
      }
      return n;
    };

    uint32_t ubo_slot = 0;
    for (const auto& r : res.uniform_buffers) {
      if (ubo_slot == kPushUboSlot) {
        ++ubo_slot;
      }
      if (ubo_slot >= kMaxUbo) {
        error = "more than 15 uniform buffers";
        return nullptr;
      }
      const Ref ref = ref_of(r.id, 0);
      ubo_f.push_back(std::to_string(ubo_slot) + ":" + std::to_string(ref.set) + "." + std::to_string(ref.binding));
      rebind(r.id, ubo_slot++);
    }
    for (const auto& r : res.push_constant_buffers) {
      c.set_decoration(r.id, spv::DecorationBinding, kPushUboSlot);
      push = true;
    }
    uint32_t ssbo_slot = 0;
    for (const auto& r : res.storage_buffers) {
      if (ssbo_slot >= kMaxSsbo) {
        error = "more than 16 storage buffers";
        return nullptr;
      }
      const Ref ref = ref_of(r.id, 0);
      ssbo_f.push_back(std::to_string(ssbo_slot) + ":" + std::to_string(ref.set) + "." + std::to_string(ref.binding));
      rebind(r.id, ssbo_slot++);
    }
    uint32_t img_slot = 0;
    for (const auto& r : res.storage_images) {
      const uint32_t n = count_of(r.type_id);
      if (img_slot + n > kMaxImg) {
        error = "more than 8 storage images";
        return nullptr;
      }
      for (uint32_t e = 0; e < n; ++e) {
        img_f.push_back(std::to_string(img_slot + e) + ":" + RefText(ref_of(r.id, e)));
      }
      rebind(r.id, img_slot);
      img_slot += n;
    }
    uint32_t tex_slot = 0;
    // Already combined in the source (sampler2D, samplerCube, samplerBuffer ...).
    for (const auto& r : res.sampled_images) {
      const uint32_t n = count_of(r.type_id);
      if (tex_slot + n > kMaxTex) {
        error = "more than 32 textures";
        return nullptr;
      }
      const bool buffer = c.get_type(r.type_id).image.dim == spv::DimBuffer;
      for (uint32_t e = 0; e < n; ++e) {
        const std::string image = RefText(ref_of(r.id, e));
        tex_f.push_back(std::to_string(tex_slot + e) + ":" + image + "/" + (buffer ? "-" : image));
      }
      rebind(r.id, tex_slot);
      tex_slot += n;
    }
    // Combined by build_combined_image_samplers (separate texture2D + sampler, or an image read with texelFetch).
    for (const auto& comb : c.get_combined_image_samplers()) {
      const uint32_t n = count_of(c.get_type_from_variable(comb.combined_id).self);
      if (tex_slot + n > kMaxTex) {
        error = "more than 32 textures";
        return nullptr;
      }
      for (uint32_t e = 0; e < n; ++e) {
        const std::string image = RefText(ref_of(comb.image_id, e));
        const std::string sampler =
            comb.sampler_id == dummy ? std::string("-") : RefText(ref_of(comb.sampler_id, e));
        tex_f.push_back(std::to_string(tex_slot + e) + ":" + image + "/" + sampler);
      }
      c.set_decoration(comb.combined_id, spv::DecorationBinding, tex_slot);
      tex_slot += n;
    }

    // Texel buffers (samplerBuffer: Azahar's lighting, fog and proctex LUTs): SPIRV-Cross leaves buffer images out of
    // the combining above (GLSL reads them with texelFetch, no sampler), so they come as separate images.
    for (const auto& r : res.separate_images) {
      if (c.get_type(r.type_id).image.dim != spv::DimBuffer) {
        continue;  // combined above
      }
      const uint32_t n = count_of(r.type_id);
      if (tex_slot + n > kMaxTex) {
        error = "more than 32 textures";
        return nullptr;
      }
      for (uint32_t e = 0; e < n; ++e) {
        tex_f.push_back(std::to_string(tex_slot + e) + ":" + RefText(ref_of(r.id, e)) + "/-");
      }
      rebind(r.id, tex_slot);
      tex_slot += n;
    }

    // Specialization constants: SPIRV-Cross writes #ifndef SPIRV_CROSS_CONSTANT_ID_n defaults, vkdk defines a
    // variant's values (VariantText).
    std::vector<std::string> spec_f;
    for (const auto& sc : c.get_specialization_constants()) {
      const spirv_cross::SPIRConstant& k = c.get_constant(sc.id);
      const spirv_cross::SPIRType& t = c.get_type(k.constant_type);
      char type = 'u';
      switch (t.basetype) {
        case spirv_cross::SPIRType::Int: type = 'i'; break;
        case spirv_cross::SPIRType::Float: type = 'f'; break;
        case spirv_cross::SPIRType::Boolean: type = 'b'; break;
        default: type = 'u'; break;
      }
      spec_f.push_back(std::to_string(sc.constant_id) + type);
    }

    std::string glsl = c.compile();

    std::string meta = "// vkdk ";
    const auto add = [&](const char* key, const std::vector<std::string>& list) {
      if (list.empty()) {
        return;
      }
      meta += std::string(key) + "=";
      for (size_t i = 0; i < list.size(); ++i) {
        meta += (i ? "," : "") + list[i];
      }
      meta += " ";
    };
    add("ubo", ubo_f);
    add("ssbo", ssbo_f);
    add("tex", tex_f);
    add("img", img_f);
    if (push) {
      meta += "push=1 ";
    }
    add("spec", spec_f);

    // After the #version line.
    const size_t nl = glsl.find('\n');
    if (glsl.compare(0, 8, "#version") != 0 || nl == std::string::npos) {
      error = "SPIRV-Cross wrote no #version line";
      return nullptr;
    }
    glsl.insert(nl + 1, meta + "\n");

    auto src = std::make_shared<ShaderSource>();
    src->stage = stage;
    src->glsl = std::move(glsl);
    char name[48];
    snprintf(name, sizeof(name), "%s %016llx", tag, (unsigned long long)Hash64(code, bytes));
    src->name = name;
    // sdmc:/switch/projectdream_nx/vkdk_dump: every converted shader as text in shaders/vkdk/glsl (debugging).
    static const bool dump = [] {
      std::FILE* f = std::fopen("sdmc:/switch/projectdream_nx/vkdk_dump", "r");
      if (f) {
        std::fclose(f);
        mkdir("sdmc:/switch/projectdream_nx/shaders/vkdk/glsl", 0777);
      }
      return f != nullptr;
    }();
    if (dump) {
      char path[128];
      snprintf(path, sizeof(path), "sdmc:/switch/projectdream_nx/shaders/vkdk/glsl/%s_%016llx.glsl", tag,
               (unsigned long long)Hash64(code, bytes));
      if (std::FILE* f = std::fopen(path, "w")) {
        std::fwrite(src->glsl.data(), 1, src->glsl.size(), f);
        std::fclose(f);
      }
    }
    const size_t at = src->glsl.find("// vkdk ");
    if (at != std::string::npos) {
      if (!src->meta.Parse(src->glsl.c_str() + at + 8, error)) {
        return nullptr;
      }
    }
    return src;
  } catch (const std::exception& e) {
    error = std::string("SPIRV-Cross: ") + e.what();
    return nullptr;
  }
}
}  // namespace

}  // namespace vkdk
