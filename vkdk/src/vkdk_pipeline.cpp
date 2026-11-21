// vkdk: shader modules, the variant cache (uam on the console, a DKSH file the PC tool fills), pipeline layouts, render
// passes, framebuffers and pipelines. Copyright 2026 aks796. GPL-3.0.
//
// A shader is GLSL for uam, made at build time from the SPIR-V bo2 draws with (tools/deko3d/bo2_dk_glsl.py, from
// renut-nx's deko3d_glsl.py): SPIRV-Cross's Vulkan GLSL with the descriptor sets turned into deko3d's per-stage slots and
// specialization constants turned into #defines. A variant is that text with its constants defined first; its key is
// the text's FNV-1a 64.
#include "vkdk_internal.h"

#include <unistd.h>

#include <pthread.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>

#include <uam_api.h>
#if VKDK_ZSTD
#include <zstd.h>
#endif

namespace vkdk {

namespace {

const char* kMetaTag = "// vkdk ";

bool ParseRef(const char*& p, BindRef& ref) {
  // set.binding[.element]
  char* end;
  const unsigned long set = strtoul(p, &end, 10);
  if (end == p || *end != '.') return false;
  p = end + 1;
  const unsigned long binding = strtoul(p, &end, 10);
  if (end == p) return false;
  p = end;
  unsigned long element = 0;
  if (*p == '.') {
    ++p;
    element = strtoul(p, &end, 10);
    if (end == p) return false;
    p = end;
  }
  ref.set = uint8_t(set);
  ref.binding = uint8_t(binding);
  ref.element = uint16_t(element);
  return true;
}

}  // namespace

bool ShaderMeta::Parse(const char* line, std::string& error) {
  // ubo=SLOT:SET.BINDING,... ssbo=... tex=SLOT:SET.BINDING.ELEM/SET.BINDING.ELEM|-,... push=1 bindless=1
  // vin=L0,L1,... spec=IDt,...
  const char* p = line;
  while (*p && *p != '\n') {
    while (*p == ' ') ++p;
    const char* key = p;
    while (*p && *p != '=' && *p != ' ' && *p != '\n') ++p;
    if (*p != '=') {
      while (*p && *p != ' ' && *p != '\n') ++p;
      continue;
    }
    const std::string k(key, p - key);
    ++p;
    auto list = [&](auto&& item) {
      for (;;) {
        if (!item()) {
          error = "bad " + k + " entry in the vkdk line";
          return false;
        }
        if (*p != ',') return true;
        ++p;
      }
    };
    char* end;
    if (k == "ubo" || k == "ssbo" || k == "img") {
      auto& out = k == "ubo" ? ubos : k == "ssbo" ? ssbos : images;
      if (!list([&] {
            const unsigned long slot = strtoul(p, &end, 10);
            if (end == p || *end != ':') return false;
            p = end + 1;
            Ubo u{uint8_t(slot), {}};
            if (!ParseRef(p, u.ref)) return false;
            out.push_back(u);
            return true;
          })) {
        return false;
      }
    } else if (k == "tex") {
      if (!list([&] {
            const unsigned long slot = strtoul(p, &end, 10);
            if (end == p || *end != ':') return false;
            p = end + 1;
            Tex t{uint8_t(slot), {}, {}};
            if (!ParseRef(p, t.image)) return false;
            if (*p != '/') return false;
            ++p;
            if (*p == '-') {
              ++p;
            } else if (!ParseRef(p, t.sampler)) {
              return false;
            }
            textures.push_back(t);
            return true;
          })) {
        return false;
      }
    } else if (k == "specmask") {
      if (!list([&] {
            const unsigned long id = strtoul(p, &end, 10);
            if (end == p || *end != ':') return false;
            p = end + 1;
            const unsigned long mask = strtoul(p, &end, 16);
            if (end == p) return false;
            specMask.push_back({uint32_t(id), uint32_t(mask)});
            p = end;
            return true;
          })) {
        return false;
      }
    } else if (k == "push") {
      push = strtoul(p, &end, 10) != 0;
      p = end;
    } else if (k == "bindless") {
      bindless = strtoul(p, &end, 10) != 0;
      p = end;
    } else if (k == "vin") {
      if (!list([&] {
            const unsigned long l = strtoul(p, &end, 10);
            if (end == p || vinCount >= 16) return false;
            vin[vinCount++] = uint32_t(l);
            p = end;
            return true;
          })) {
        return false;
      }
    } else if (k == "spec") {
      if (!list([&] {
            const unsigned long id = strtoul(p, &end, 10);
            if (end == p || !*end || !strchr("uifb", *end)) return false;
            spec.push_back({uint32_t(id), *end});
            p = end + 1;
            return true;
          })) {
        return false;
      }
    } else {
      while (*p && *p != ' ' && *p != '\n') ++p;
    }
  }
  return true;
}

std::shared_ptr<const ShaderSource> ParseShaderBlob(const void* data, size_t size, std::string& error) {
  uint32_t stage = 0;
  std::string text = BlobText(static_cast<const uint32_t*>(data), size / 4, &stage);
  if (text.empty()) {
    error = "not a vkdk shader blob, or a damaged one";
    return nullptr;
  }
  auto src = std::make_shared<ShaderSource>();
  src->stage = DkStage(stage);
  src->glsl = std::move(text);
  char name[48];
  snprintf(name, sizeof(name), "%s %016llx", stage == DkStage_Vertex ? "vs" : stage == DkStage_Fragment ? "ps" : "cs",
           (unsigned long long)Hash64(src->glsl.data(), src->glsl.size(), stage));
  src->name = name;
  const size_t at = src->glsl.find(kMetaTag);
  if (at == std::string::npos) {
    error = "no vkdk line";
    return nullptr;
  }
  if (!src->meta.Parse(src->glsl.c_str() + at + strlen(kMetaTag), error)) {
    return nullptr;
  }
  return src;
}

// ---- the variant cache

namespace {

struct Builtin {
  uint64_t spirvHash;
  uint32_t stage;
  const char* glsl;
};

#include "vkdk_builtin_shaders.inc"

constexpr char kCacheMagic[8] = {'V', 'K', 'D', 'K', 'C', 'A', '1', '\0'};
constexpr unsigned kUamFlags = 0;  // uam::setBanjoFlags (WorkerMain)

// Round o diagnostics (sdmc:/switch/sdt_nx/vkdk_safesched): uam's control words at their safest (uam::setSafeSched
// 3: longest stall, every barrier waited on, no operand reuse). Such shaders get their own cache keys.
unsigned SafeSchedFlags() {
  static const unsigned flags = DebugSwitch("vkdk_safesched", false) ? 3u : 0u;
  return flags;
}


}  // namespace

// One worker thread with an 8 MB stack for the whole process, made on first use and never ended: uam is not
// reentrant and both it and SPIRV-Cross recurse deeply (a SPIR-V conversion overflowed a renderer thread's stack).
// A thread per device (or per conversion) failed in Dekopon-32 once a game had been left for another: the 8 MB stack
// could not be found again in the 32-bit heap ("shader compiler thread could not start").
namespace {
struct BigStackWorker {
  std::mutex mutex;
  std::condition_variable ready, done;
  std::deque<std::pair<const std::function<void()>*, bool*>> jobs;
  bool started = false;

  BigStackWorker() {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    pthread_t thread;
    started = pthread_create(&thread, &attr, Main, this) == 0;
    pthread_attr_destroy(&attr);
    if (started) {
      pthread_detach(thread);
    } else {
      Log("shader compiler thread could not start");
    }
  }

  static void* Main(void* arg) {
    BigStackWorker* self = static_cast<BigStackWorker*>(arg);
    for (;;) {
      std::pair<const std::function<void()>*, bool*> job;
      {
        std::unique_lock<std::mutex> lock(self->mutex);
        self->ready.wait(lock, [&] { return !self->jobs.empty(); });
        job = self->jobs.front();
        self->jobs.pop_front();
      }
      (*job.first)();
      {
        std::lock_guard<std::mutex> lock(self->mutex);
        *job.second = true;
      }
      self->done.notify_all();
    }
    return nullptr;
  }
};
}  // namespace

bool RunOnBigStack(const std::function<void()>& fn) {
  static BigStackWorker worker;
  if (!worker.started) {
    return false;
  }
  bool finished = false;
  std::unique_lock<std::mutex> lock(worker.mutex);
  worker.jobs.push_back({&fn, &finished});
  worker.ready.notify_one();
  worker.done.wait(lock, [&] { return finished; });
  return true;
}

}  // namespace vkdk

extern "C" void vkdkStartWorker(void) {
  vkdk::RunOnBigStack([] {});
}

namespace vkdk {

struct ShaderCache::Impl {
  VkDevice device;
  std::mutex mutex;  // variants, the file and its index
  std::unordered_map<uint64_t, std::unique_ptr<Variant>> variants;
  std::unordered_map<uint64_t, std::pair<long, uint32_t>> fileIndex;  // key -> (offset of the DKSH, size)
  std::string path;
  FILE* file = nullptr;
  long fileEnd = 0;
  std::unordered_map<uint64_t, std::shared_ptr<const ShaderSource>> builtins;
  std::mutex builtinMutex;


  std::atomic<uint64_t> compiled{0}, loaded{0}, compileNs{0};

  // The prebuilt cache (vkdkSetPrebuiltShaderCachePath): its index in memory, each frame read when asked for.
  std::string prebuiltPath;
  FILE* prebuilt = nullptr;
  std::vector<VkdkPrebuiltEntry> prebuiltIndex;
  uint64_t prebuiltHits = 0, prebuiltNs = 0;
  bool prebuiltPlain = false;  // a cache file (kCacheMagic records, uncompressed) used as the prebuilt set
#if VKDK_ZSTD
  ZSTD_DDict* prebuiltDict = nullptr;
  ZSTD_DCtx* prebuiltCtx = nullptr;
#endif

  bool Compile(DkStage stage, const std::string& text, std::vector<uint8_t>& dksh, std::string& log) {
    static const uam::Stage kStages[] = {uam::Stage::Vertex, uam::Stage::TessCtrl, uam::Stage::TessEval,
                                         uam::Stage::Geometry, uam::Stage::Fragment, uam::Stage::Compute};
    bool ok = false;
    const bool ran = RunOnBigStack([&] {
      static bool uamReady = false;
      if (!uamReady) {
        uam::init(true);
        // uam's own code generation, without banjo-nx's passes (its default, 1471): those were tuned and checked
        // on Banjo's shaders and its buffer layout (flag 1024), and flag 16 compiles a fragment shader up to four
        // times to keep the variant with fewer registers, which more than doubles a compile (79 against 36 ms a
        // shader on the PC, about ten times that on the console) for the same code size on average. renut-nx's
        // correctness patches (no dual issue, the stall before a new block) do not depend on the flags. A
        // PC-built DKSH cache must use the same flags (UAM_BANJO_FLAGS=0 for the uam tool).
        uam::setBanjoFlags(kUamFlags);
        uam::setSafeSched(SafeSchedFlags());
        uamReady = true;
      }
      uam::Result r = uam::compile(kStages[stage], text.c_str());
      ok = r.ok;
      dksh = std::move(r.dksh);
      log = std::move(r.log);
    });
    return ran && ok;
  }

  // The file: magic, then records {u64 key, u32 size, u32 0, DKSH, padding to 8}. A record cut short by a killed
  // process ends the walk; new records go after the last whole one (renut-nx's rule for its cache file).
  void OpenFile() {
    if (path.empty()) {
      return;
    }
    file = fopen(path.c_str(), "r+b");
    if (!file) {
      file = fopen(path.c_str(), "w+b");
      if (!file) {
        Log("shader cache: cannot open %s", path.c_str());
        return;
      }
      fwrite(kCacheMagic, 1, 8, file);
      fflush(file);
      fileEnd = 8;
      return;
    }
    char magic[8] = {};
    if (fread(magic, 1, 8, file) != 8 || memcmp(magic, kCacheMagic, 8)) {
      Log("shader cache: %s is not a vkdk cache, started again", path.c_str());
      fclose(file);
      file = fopen(path.c_str(), "w+b");
      if (file) {
        fwrite(kCacheMagic, 1, 8, file);
        fflush(file);
      }
      fileEnd = 8;
      return;
    }
    long at = 8;
    for (;;) {
      struct {
        uint64_t key;
        uint32_t size, zero;
      } rec;
      if (fseek(file, at, SEEK_SET) || fread(&rec, sizeof(rec), 1, file) != 1) {
        break;
      }
      uint32_t magicDksh = 0;
      if (rec.size < 24 || fread(&magicDksh, 4, 1, file) != 1 || magicDksh != 0x48534B44u) {
        break;
      }
      const long data = at + long(sizeof(rec));
      const long next = data + long(AlignUp(rec.size, 8));
      if (fseek(file, next - 1, SEEK_SET) || fgetc(file) == EOF) {
        break;
      }
      fileIndex[rec.key] = {data, rec.size};
      at = next;
    }
    fileEnd = at;
    Log("shader cache: %zu variants in %s", fileIndex.size(), path.c_str());
  }

  bool ReadFromFile(uint64_t key, std::vector<uint8_t>& out) {
    auto it = fileIndex.find(key);
    if (it == fileIndex.end() || !file) {
      return false;
    }
    out.resize(it->second.second);
    return fseek(file, it->second.first, SEEK_SET) == 0 && fread(out.data(), 1, out.size(), file) == out.size();
  }

  // A cache file as the prebuilt set (another console's or an emulator's dksh.cache, copied as it is): its records
  // indexed like the cache file's, read as they are. 0 when the file is not one.
  bool OpenPrebuiltPlain() {
    char magic[8] = {};
    if (fseek(prebuilt, 0, SEEK_SET) || fread(magic, 1, 8, prebuilt) != 8 || memcmp(magic, kCacheMagic, 8)) {
      return false;
    }
    long at = 8;
    for (;;) {
      struct {
        uint64_t key;
        uint32_t size, zero;
      } rec;
      uint32_t magicDksh = 0;
      if (fseek(prebuilt, at, SEEK_SET) || fread(&rec, sizeof(rec), 1, prebuilt) != 1 || rec.size < 24 ||
          fread(&magicDksh, 4, 1, prebuilt) != 1 || magicDksh != 0x48534B44u) {
        break;
      }
      const long data = at + long(sizeof(rec));
      const long next = data + long(AlignUp(rec.size, 8));
      if (fseek(prebuilt, next - 1, SEEK_SET) || fgetc(prebuilt) == EOF) {
        break;
      }
      prebuiltIndex.push_back({rec.key, uint64_t(data), rec.size, rec.size});
      at = next;
    }
    std::sort(prebuiltIndex.begin(), prebuiltIndex.end(),
              [](const VkdkPrebuiltEntry& a, const VkdkPrebuiltEntry& b) { return a.key < b.key; });
    prebuiltPlain = true;
    Log("prebuilt shader cache: %zu variants (a cache file) in %s", prebuiltIndex.size(), prebuiltPath.c_str());
    return true;
  }

  void OpenPrebuilt() {
    if (prebuiltPath.empty() || prebuilt) {
      return;
    }
    prebuilt = fopen(prebuiltPath.c_str(), "rb");
    if (!prebuilt) {
      Log("prebuilt shader cache: no %s (every variant compiled here)", prebuiltPath.c_str());
      return;
    }
    if (OpenPrebuiltPlain()) {
      return;
    }
#if VKDK_ZSTD
    fseek(prebuilt, 0, SEEK_SET);
    VkdkPrebuiltHeader h{};
    std::vector<uint8_t> dict;
    bool ok = fread(&h, sizeof(h), 1, prebuilt) == 1 && !memcmp(h.magic, VKDK_PREBUILT_MAGIC, sizeof(h.magic)) &&
              h.count && h.count < (1u << 24) && h.dict_bytes < (1u << 24);
    if (ok) {
      dict.resize(h.dict_bytes);
      prebuiltIndex.resize(h.count);
      ok = (!h.dict_bytes || fread(dict.data(), 1, dict.size(), prebuilt) == dict.size()) &&
           fread(prebuiltIndex.data(), sizeof(VkdkPrebuiltEntry), h.count, prebuilt) == h.count;
    }
    if (ok) {
      prebuiltCtx = ZSTD_createDCtx();
      prebuiltDict = h.dict_bytes ? ZSTD_createDDict(dict.data(), dict.size()) : nullptr;
      ok = prebuiltCtx && (prebuiltDict || !h.dict_bytes);
    }
    if (!ok) {
      Log("prebuilt shader cache: %s is not one this build reads, not used", prebuiltPath.c_str());
      fclose(prebuilt);
      prebuilt = nullptr;
      prebuiltIndex.clear();
      return;
    }
    Log("prebuilt shader cache: %u variants (built on the PC with uam flags %u) in %s", h.count, h.uam_flags,
        prebuiltPath.c_str());
#else
    Log("prebuilt shader cache: %s is a compressed pack, which this build does not read", prebuiltPath.c_str());
    fclose(prebuilt);
    prebuilt = nullptr;
#endif
  }

  bool ReadFromPrebuilt(uint64_t key, std::vector<uint8_t>& out) {
    if (prebuilt && prebuiltPlain) {
      auto it = std::lower_bound(prebuiltIndex.begin(), prebuiltIndex.end(), key,
                                 [](const VkdkPrebuiltEntry& e, uint64_t k) { return e.key < k; });
      if (it == prebuiltIndex.end() || it->key != key) {
        return false;
      }
      out.resize(it->size);
      if (fseek(prebuilt, long(it->offset), SEEK_SET) || fread(out.data(), 1, out.size(), prebuilt) != out.size()) {
        return false;
      }
      if (++prebuiltHits == 1 || prebuiltHits % 100 == 0) {
        Log("prebuilt shader cache: %llu variants read from it", (unsigned long long)prebuiltHits);
      }
      return true;
    }
#if VKDK_ZSTD
    if (!prebuilt) {
      return false;
    }
    auto it = std::lower_bound(prebuiltIndex.begin(), prebuiltIndex.end(), key,
                               [](const VkdkPrebuiltEntry& e, uint64_t k) { return e.key < k; });
    if (it == prebuiltIndex.end() || it->key != key) {
      return false;
    }
    const uint64_t t0 = armTicksToNs(armGetSystemTick());
    std::vector<uint8_t> frame(it->stored);
    out.resize(it->size);
    if (fseek(prebuilt, long(it->offset), SEEK_SET) || fread(frame.data(), 1, frame.size(), prebuilt) != frame.size()) {
      return false;
    }
    const size_t n = prebuiltDict ? ZSTD_decompress_usingDDict(prebuiltCtx, out.data(), out.size(), frame.data(),
                                                               frame.size(), prebuiltDict)
                                  : ZSTD_decompressDCtx(prebuiltCtx, out.data(), out.size(), frame.data(), frame.size());
    if (ZSTD_isError(n) || n != out.size()) {
      Log("prebuilt shader cache: variant %016llx damaged", (unsigned long long)key);
      return false;
    }
    prebuiltNs += armTicksToNs(armGetSystemTick()) - t0;
    if (++prebuiltHits == 1 || prebuiltHits % 100 == 0) {
      Log("prebuilt shader cache: %llu variants read from it (%.1f ms reading and inflating)",
          (unsigned long long)prebuiltHits, double(prebuiltNs) / 1e6);
    }
    return true;
#else
    (void)key;
    (void)out;
    return false;
#endif
  }

  void Append(uint64_t key, const std::vector<uint8_t>& dksh) {
    if (!file) {
      return;
    }
    struct {
      uint64_t key;
      uint32_t size, zero;
    } rec{key, uint32_t(dksh.size()), 0};
    std::vector<uint8_t> buf(sizeof(rec) + AlignUp(uint32_t(dksh.size()), 8), 0);
    memcpy(buf.data(), &rec, sizeof(rec));
    memcpy(buf.data() + sizeof(rec), dksh.data(), dksh.size());
    // One write a record, flushed: a process killed meanwhile leaves at most one cut record, which the next start drops.
    if (fseek(file, fileEnd, SEEK_SET) == 0 && fwrite(buf.data(), 1, buf.size(), file) == buf.size()) {
      fflush(file);
      // And committed: the SD card's file size is only written on a sync or a close, so a run that froze or was
      // closed from HOME kept none of its new shaders (round i: dksh.cache still at the first run's 155 variants).
      fsync(fileno(file));
      fileIndex[key] = {fileEnd + long(sizeof(rec)), rec.size};
      fileEnd += long(buf.size());
    }
  }
};

ShaderCache::ShaderCache(VkDevice device) : impl_(new Impl) {
  impl_->device = device;
}

ShaderCache::~ShaderCache() {
  if (impl_->file) {
    fclose(impl_->file);
  }
  if (impl_->prebuilt) {
    fclose(impl_->prebuilt);
  }
#if VKDK_ZSTD
  ZSTD_freeDDict(impl_->prebuiltDict);
  ZSTD_freeDCtx(impl_->prebuiltCtx);
#endif
}

void ShaderCache::SetCachePath(const char* path) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->path = path ? path : "";
  impl_->OpenFile();
}

void ShaderCache::SetPrebuiltPath(const char* path) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->prebuiltPath = path ? path : "";
  impl_->OpenPrebuilt();
}

static std::string VariantText(const ShaderSource& src, const VkSpecializationInfo* spec) {
  if (!spec || !spec->mapEntryCount || src.meta.spec.empty()) {
    return src.glsl;
  }
  std::string defines;
  for (uint32_t i = 0; i < spec->mapEntryCount; ++i) {
    const VkSpecializationMapEntry& e = spec->pMapEntries[i];
    char type = 'u';
    bool known = false;
    for (const auto& s : src.meta.spec) {
      if (s.first == e.constantID) {
        type = s.second;
        known = true;
      }
    }
    if (!known || e.offset + e.size > spec->dataSize) {
      continue;
    }
    uint32_t raw = 0;
    memcpy(&raw, static_cast<const uint8_t*>(spec->pData) + e.offset, std::min<size_t>(e.size, 4));
    // Only the bits this shader reads (the converter's specmask): other values give the same code.
    for (const auto& m : src.meta.specMask) {
      if (m.first == e.constantID) {
        raw &= m.second;
      }
    }
    char line[96];
    switch (type) {
      case 'b':
        snprintf(line, sizeof(line), "#define SPIRV_CROSS_CONSTANT_ID_%u %s\n", e.constantID, raw ? "true" : "false");
        break;
      case 'i':
        snprintf(line, sizeof(line), "#define SPIRV_CROSS_CONSTANT_ID_%u %d\n", e.constantID, int32_t(raw));
        break;
      case 'f': {
        snprintf(line, sizeof(line), "#define SPIRV_CROSS_CONSTANT_ID_%u uintBitsToFloat(0x%08xu)\n", e.constantID,
                 raw);
        break;
      }
      default:
        snprintf(line, sizeof(line), "#define SPIRV_CROSS_CONSTANT_ID_%u %uu\n", e.constantID, raw);
        break;
    }
    defines += line;
  }
  if (defines.empty()) {
    return src.glsl;
  }
  // After the #version line.
  const size_t nl = src.glsl.find('\n');
  if (nl == std::string::npos) {
    return src.glsl;
  }
  std::string out;
  out.reserve(src.glsl.size() + defines.size());
  out.append(src.glsl, 0, nl + 1);
  out += defines;
  out.append(src.glsl, nl + 1, std::string::npos);
  return out;
}

const Variant* ShaderCache::Get(const std::shared_ptr<const ShaderSource>& source, const VkSpecializationInfo* spec,
                                bool compile, bool* required) {
  if (!source) {
    return nullptr;
  }
  const std::string text = VariantText(*source, spec);
  const uint64_t key = Hash64(text.data(), text.size(), source->stage + (SafeSchedFlags() << 8));
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->variants.find(key);
    if (it != impl_->variants.end()) {
      return it->second.get();
    }
  }
  std::vector<uint8_t> dksh;
  bool fromFile;  // this install's cache file or the prebuilt one: nothing to append
  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    fromFile = impl_->ReadFromFile(key, dksh) || impl_->ReadFromPrebuilt(key, dksh);
  }
  if (!fromFile && !compile) {
    if (required) {
      *required = true;
    }
    return nullptr;
  }
  if (!fromFile) {
    std::string log;
    const uint64_t t0 = armTicksToNs(armGetSystemTick());
    if (!impl_->Compile(source->stage, text, dksh, log)) {
      Log("uam: %s (variant %016llx) failed: %.600s", source->name.c_str(), (unsigned long long)key, log.c_str());
      return nullptr;
    }
    const uint64_t ns = armTicksToNs(armGetSystemTick()) - t0;
    impl_->compileNs += ns;
    // Each compile on the console, for the PC-built cache and the first visit's cost: which shader (its stage and
    // the hash of its text), the #defines VariantText put after its first line ('|' for each line end), how long.
    std::string defines;
    const size_t nl = source->glsl.find('\n');
    if (nl != std::string::npos && text.size() > source->glsl.size()) {
      defines = text.substr(nl + 1, text.size() - source->glsl.size());
      for (char& c : defines) {
        c = c == '\n' ? '|' : c;
      }
    }
    Log("uam: %s variant %016llx [%s]: %zu bytes of GLSL -> %zu of DKSH in %.1f ms", source->name.c_str(),
        (unsigned long long)key, defines.c_str(), text.size(), dksh.size(), double(ns) / 1e6);
    impl_->compiled++;
  } else {
    impl_->loaded++;
  }
  auto v = std::make_unique<Variant>();
  v->key = key;
  v->meta = &source->meta;
  v->source = source;
  if (!impl_->device->LoadDksh(dksh.data(), dksh.size(), v->shader)) {
    Log("shader %016llx: DKSH could not be loaded", (unsigned long long)key);
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!fromFile) {
    impl_->Append(key, dksh);
  }
  auto [it, inserted] = impl_->variants.emplace(key, std::move(v));
  // A variant keeps its meta through the source: the module may go, the pipeline keeps the source alive (VkPipeline_T
  // holds the shared_ptrs).
  return it->second.get();
}

const Variant* ShaderCache::FromGlsl(DkStage stage, const char* glsl, const char* name) {
  auto src = std::make_shared<ShaderSource>();
  src->stage = stage;
  src->glsl = glsl;
  src->name = name;
  return Get(src, nullptr);
}

std::shared_ptr<const ShaderSource> ShaderCache::Builtin(const uint32_t* spirv, size_t bytes) {
  const uint64_t hash = Hash64(spirv, bytes);
  std::lock_guard<std::mutex> lock(impl_->builtinMutex);
  auto it = impl_->builtins.find(hash);
  if (it != impl_->builtins.end()) {
    return it->second;
  }
  for (const auto& b : kBuiltins) {
    if (b.spirvHash != hash || !b.glsl) {
      continue;
    }
    auto src = std::make_shared<ShaderSource>();
    src->stage = DkStage(b.stage);
    src->glsl = b.glsl;
    char name[40];
    snprintf(name, sizeof(name), "builtin %016llx", (unsigned long long)hash);
    src->name = name;
    const size_t at = src->glsl.find(kMetaTag);
    std::string error;
    if (at == std::string::npos || !src->meta.Parse(src->glsl.c_str() + at + strlen(kMetaTag), error)) {
      Log("%s: %s", name, error.c_str());
      return nullptr;
    }
    impl_->builtins.emplace(hash, src);
    return src;
  }
  return nullptr;
}

}  // namespace vkdk

using namespace vkdk;

// ---- shader modules

static VKAPI_ATTR VkResult VKAPI_CALL CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo* info,
                                                         const VkAllocationCallbacks*, VkShaderModule* out) {
  VKDK_TRACE("CreateShaderModule");
  VkShaderModule m = new (std::nothrow) VkShaderModule_T();
  if (!m) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  uint32_t first = 0;
  memcpy(&first, info->pCode, std::min<size_t>(info->codeSize, 4));
  if (first == VKDK_SHADER_BLOB_MAGIC) {
    std::string error;
    m->source = ParseShaderBlob(info->pCode, info->codeSize, error);
    if (!m->source) {
      Log("shader blob refused: %s", error.c_str());
      delete m;
      return VK_ERROR_INVALID_SHADER_NV;
    }
  } else if (first == 0x07230203u) {
    m->source = d->shaders->Builtin(info->pCode, info->codeSize);
    if (!m->source) {
      std::string error;
      m->source = ConvertSpirv(info->pCode, info->codeSize, error);
      if (!m->source) {
        VKDK_LOG_LIMITED(64, "SPIR-V module %016llx: %s", (unsigned long long)Hash64(info->pCode, info->codeSize),
                         error.c_str());
      }
    }
    if (!m->source) {
      // Kept as an empty module: pipelines made from it fail (and say so); modules nothing draws with cost nothing.
      VKDK_LOG_LIMITED(64, "SPIR-V module %016llx (%zu bytes) has no deko3d conversion",
                       (unsigned long long)Hash64(info->pCode, info->codeSize), info->codeSize);
    }
  } else {
    delete m;
    return VK_ERROR_INVALID_SHADER_NV;
  }
  *out = m;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyShaderModule(VkDevice, VkShaderModule m, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyShaderModule");
  delete m;
}

// ---- pipeline caches (deko3d has none: the DKSH file is the cache)

static VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo*,
                                                          const VkAllocationCallbacks*, VkPipelineCache* out) {
  VKDK_TRACE("CreatePipelineCache");
  *out = new (std::nothrow) VkPipelineCache_T();
  return *out ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
}

static VKAPI_ATTR void VKAPI_CALL DestroyPipelineCache(VkDevice, VkPipelineCache c, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyPipelineCache");
  delete c;
}

static VKAPI_ATTR VkResult VKAPI_CALL GetPipelineCacheData(VkDevice, VkPipelineCache, size_t* size, void*) {
  VKDK_TRACE("GetPipelineCacheData");
  *size = 0;
  return VK_SUCCESS;
}

// ---- pipeline layouts

static VKAPI_ATTR VkResult VKAPI_CALL CreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo* info,
                                                           const VkAllocationCallbacks*, VkPipelineLayout* out) {
  VKDK_TRACE("CreatePipelineLayout");
  VkPipelineLayout l = new (std::nothrow) VkPipelineLayout_T();
  if (!l) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  for (uint32_t i = 0; i < info->setLayoutCount; ++i) {
    l->sets.push_back(info->pSetLayouts[i] ? *info->pSetLayouts[i] : VkDescriptorSetLayout_T{});
  }
  for (uint32_t i = 0; i < info->pushConstantRangeCount; ++i) {
    l->pushBytes = std::max(l->pushBytes, info->pPushConstantRanges[i].offset + info->pPushConstantRanges[i].size);
  }
  *out = l;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyPipelineLayout(VkDevice, VkPipelineLayout l, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyPipelineLayout");
  delete l;
}

// ---- render passes and framebuffers

static VKAPI_ATTR VkResult VKAPI_CALL CreateRenderPass(VkDevice, const VkRenderPassCreateInfo* info,
                                                       const VkAllocationCallbacks*, VkRenderPass* out) {
  VKDK_TRACE("CreateRenderPass");
  if (info->subpassCount < 1) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
  }
  VkRenderPass p = new (std::nothrow) VkRenderPass_T();
  if (!p) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  for (uint32_t i = 0; i < info->attachmentCount; ++i) {
    const VkAttachmentDescription& a = info->pAttachments[i];
    p->attachments.push_back({a.format, a.loadOp, a.stencilLoadOp, IsDepthFormat(a.format), HasStencil(a.format)});
  }
  const VkSubpassDescription& s = info->pSubpasses[0];
  p->colorCount = std::min<uint32_t>(s.colorAttachmentCount, 8);
  for (uint32_t i = 0; i < p->colorCount; ++i) {
    p->colorRefs[i] = s.pColorAttachments[i].attachment;
  }
  p->depthRef = s.pDepthStencilAttachment ? s.pDepthStencilAttachment->attachment : VK_ATTACHMENT_UNUSED;
  for (uint32_t k = 0; k < info->subpassCount; ++k) {
    const VkSubpassDescription& sk = info->pSubpasses[k];
    VkRenderPass_T::Subpass sp;
    sp.colorCount = std::min<uint32_t>(sk.colorAttachmentCount, 8);
    for (uint32_t i = 0; i < sp.colorCount; ++i) {
      sp.colorRefs[i] = sk.pColorAttachments[i].attachment;
      sp.resolveRefs[i] = sk.pResolveAttachments ? sk.pResolveAttachments[i].attachment : VK_ATTACHMENT_UNUSED;
    }
    sp.depthRef = sk.pDepthStencilAttachment ? sk.pDepthStencilAttachment->attachment : VK_ATTACHMENT_UNUSED;
    if (sk.inputAttachmentCount) {
      VKDK_LOG_LIMITED(4, "render pass: subpass %u reads %u input attachments (not supported: sampled as nothing)", k,
                       sk.inputAttachmentCount);
    }
    p->subpasses.push_back(sp);
  }
  *out = p;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyRenderPass(VkDevice, VkRenderPass p, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyRenderPass");
  delete p;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateFramebuffer(VkDevice, const VkFramebufferCreateInfo* info,
                                                        const VkAllocationCallbacks*, VkFramebuffer* out) {
  VKDK_TRACE("CreateFramebuffer");
  VkFramebuffer f = new (std::nothrow) VkFramebuffer_T();
  if (!f) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  f->views.assign(info->pAttachments, info->pAttachments + info->attachmentCount);
  f->width = info->width;
  f->height = info->height;
  f->layers = info->layers;
  *out = f;
  return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL DestroyFramebuffer(VkDevice, VkFramebuffer f, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyFramebuffer");
  delete f;
}

// ---- pipelines

namespace {

// deko3d 0.5.0 writes the colour destination factor into FuncAlphaDst (SwitchWakerHD, Tenkaichi-3-NX): the target's
// IndependentBlend registers (method 0x781 + 8 * target, six words) are written again after dkCmdBufBindBlendStates.
uint32_t BlendFactorWord(uint32_t f) {  // DkBlendFactor; a plain bitfield in the 32-bit deko3d
  return (uint32_t(f) & 0x1Fu) | ((uint32_t(f) & 0x20u) ? 0xC000u : 0u);
}

void BlendWords(uint32_t words[7], uint32_t target, const DkBlendState& s) {
  words[0] = (1u << 29) | (6u << 16) | (0u << 13) | (0x781u + 8u * target);
  words[1] = s.colorBlendOp;
  words[2] = BlendFactorWord(s.srcColorBlendFactor);
  words[3] = BlendFactorWord(s.dstColorBlendFactor);
  words[4] = s.alphaBlendOp;
  words[5] = BlendFactorWord(s.srcAlphaBlendFactor);
  words[6] = BlendFactorWord(s.dstAlphaBlendFactor);
}

// Dynamic states as bits (vkdk_internal.h: VkPipeline_T::dynamic).
uint32_t DynamicBit(VkDynamicState s) {
  switch (s) {
    case VK_DYNAMIC_STATE_VIEWPORT: return 1u << 0;
    case VK_DYNAMIC_STATE_SCISSOR: return 1u << 1;
    case VK_DYNAMIC_STATE_DEPTH_BIAS: return 1u << 3;
    case VK_DYNAMIC_STATE_BLEND_CONSTANTS: return 1u << 4;
    case VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK: return 1u << 6;
    case VK_DYNAMIC_STATE_STENCIL_WRITE_MASK: return 1u << 7;
    case VK_DYNAMIC_STATE_STENCIL_REFERENCE: return 1u << 8;
    default: return 1u << 31;  // unsupported: logged once
  }
}

}  // namespace

static VkResult MakeGraphics(VkDevice d, const VkGraphicsPipelineCreateInfo& info, VkPipeline* out) {
  VkPipeline p = new (std::nothrow) VkPipeline_T();
  if (!p) {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
  if (info.layout) {
    p->layout = *info.layout;
  }
  for (uint32_t i = 0; i < info.stageCount; ++i) {
    const VkPipelineShaderStageCreateInfo& st = info.pStages[i];
    int index = st.stage == VK_SHADER_STAGE_VERTEX_BIT ? 0 : st.stage == VK_SHADER_STAGE_FRAGMENT_BIT ? 1 : -1;
    if (index < 0) {
      Log("pipeline stage %x is not supported", st.stage);
      delete p;
      return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    if (!st.module->source) {
      delete p;
      return VK_ERROR_INVALID_SHADER_NV;
    }
    bool required = false;
    p->stages[index] = d->shaders->Get(st.module->source, st.pSpecializationInfo,
                                       !(info.flags & VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT_EXT),
                                       &required);
    if (!p->stages[index]) {
      delete p;
      return required ? VK_PIPELINE_COMPILE_REQUIRED_EXT : VK_ERROR_INVALID_SHADER_NV;
    }
    p->stageMask |= index ? DkStageFlag_Fragment : DkStageFlag_Vertex;
  }
  if (!p->stages[0]) {
    Log("graphics pipeline without a vertex shader");
    delete p;
    return VK_ERROR_INVALID_SHADER_NV;
  }

  // Vertex input. deko3d's attribute i is the shader's input location i; a shader the converter renumbered says which
  // Vulkan location each of its inputs is (meta vin).
  const ShaderMeta& vsMeta = *p->stages[0]->meta;
  if (const VkPipelineVertexInputStateCreateInfo* vi = info.pVertexInputState) {
    uint32_t maxBinding = 0;
    for (uint32_t i = 0; i < vi->vertexBindingDescriptionCount; ++i) {
      const auto& b = vi->pVertexBindingDescriptions[i];
      if (b.binding >= DK_MAX_VERTEX_BUFFERS) continue;
      p->vtxBuffers[b.binding].stride = b.stride;
      p->vtxBuffers[b.binding].divisor = b.inputRate == VK_VERTEX_INPUT_RATE_INSTANCE ? 1 : 0;
      maxBinding = std::max(maxBinding, b.binding + 1);
    }
    p->vtxBufferCount = maxBinding;
    uint32_t count = 0;
    if (vsMeta.vinCount) {
      count = vsMeta.vinCount;
    } else {
      for (uint32_t i = 0; i < vi->vertexAttributeDescriptionCount; ++i) {
        count = std::max(count, vi->pVertexAttributeDescriptions[i].location + 1);
      }
    }
    count = std::min<uint32_t>(count, DK_MAX_VERTEX_ATTRIBS);
    for (uint32_t i = 0; i < count; ++i) {
      DkVtxAttribState& a = p->attribs[i];
      memset(&a, 0, sizeof(a));
      a.isFixed = 1;
      a.size = DkVtxAttribSize_1x32;
      a.type = DkVtxAttribType_Float;
      const uint32_t location = vsMeta.vinCount ? vsMeta.vin[i] : i;
      for (uint32_t k = 0; k < vi->vertexAttributeDescriptionCount; ++k) {
        const auto& va = vi->pVertexAttributeDescriptions[k];
        if (va.location != location) continue;
        DkVtxAttribSize size;
        DkVtxAttribType type;
        bool bgra;
        if (!VertexFormat(va.format, size, type, bgra)) {
          VKDK_LOG_LIMITED(16, "vertex format %d is not supported", int(va.format));
          break;
        }
        a.isFixed = 0;
        a.bufferId = va.binding;
        a.offset = va.offset;
        a.size = size;
        a.type = type;
        a.isBgra = bgra;
        break;
      }
    }
    p->attribCount = count;
  }

  if (const VkPipelineInputAssemblyStateCreateInfo* ia = info.pInputAssemblyState) {
    p->primitive = ToDk(ia->topology);
    p->restart = ia->primitiveRestartEnable;
  }
  if (info.pTessellationState) {
    p->patchSize = info.pTessellationState->patchControlPoints;
  }

  dkRasterizerStateDefaults(&p->rasterizer);
  if (const VkPipelineRasterizationStateCreateInfo* rs = info.pRasterizationState) {
    p->rasterizer.rasterizerEnable = rs->rasterizerDiscardEnable ? 0 : 1;
    p->rasterizer.depthClampEnable = rs->depthClampEnable ? 1 : 0;
    const DkPolygonMode mode = rs->polygonMode == VK_POLYGON_MODE_LINE    ? DkPolygonMode_Line
                               : rs->polygonMode == VK_POLYGON_MODE_POINT ? DkPolygonMode_Point
                                                                          : DkPolygonMode_Fill;
    p->rasterizer.polygonModeFront = mode;
    p->rasterizer.polygonModeBack = mode;
    p->rasterizer.cullMode = (rs->cullMode & VK_CULL_MODE_FRONT_AND_BACK) == VK_CULL_MODE_FRONT_AND_BACK
                                 ? DkFace_FrontAndBack
                             : (rs->cullMode & VK_CULL_MODE_FRONT_BIT) ? DkFace_Front
                             : (rs->cullMode & VK_CULL_MODE_BACK_BIT)  ? DkFace_Back
                                                                       : DkFace_None;
    // Front faces are judged on the screen in both APIs (y down); the viewport keeps Vulkan's orientation.
    p->rasterizer.frontFace = rs->frontFace == VK_FRONT_FACE_COUNTER_CLOCKWISE ? DkFrontFace_CCW : DkFrontFace_CW;
    p->rasterizer.provokingVertex = DkProvokingVertex_First;
    p->rasterizer.depthBiasEnableMask = rs->depthBiasEnable ? DkPolygonFlag_All : 0;
    p->depthBias[0] = rs->depthBiasConstantFactor;
    p->depthBias[1] = rs->depthBiasClamp;
    p->depthBias[2] = rs->depthBiasSlopeFactor;
  }

  dkMultisampleStateDefaults(&p->multisample);
  p->multisample.alphaToCoverageDither = false;  // renut-nx: Maxwell's dither makes non-square patterns
  if (const VkPipelineMultisampleStateCreateInfo* ms = info.pMultisampleState) {
    p->multisample.alphaToCoverageEnable = ms->alphaToCoverageEnable;
    if (ms->pSampleMask) {
      p->sampleMask = ms->pSampleMask[0];
    }
  }

  dkDepthStencilStateDefaults(&p->depthStencil);
  p->depthStencil.depthTestEnable = 0;
  p->depthStencil.depthWriteEnable = 0;
  if (const VkPipelineDepthStencilStateCreateInfo* ds = info.pDepthStencilState) {
    p->depthStencil.depthTestEnable = ds->depthTestEnable;
    p->depthStencil.depthWriteEnable = ds->depthWriteEnable;
    p->depthStencil.depthCompareOp = ToDk(ds->depthCompareOp);
    p->depthStencil.stencilTestEnable = ds->stencilTestEnable;
    p->depthStencil.stencilFrontFailOp = ToDk(ds->front.failOp);
    p->depthStencil.stencilFrontPassOp = ToDk(ds->front.passOp);
    p->depthStencil.stencilFrontDepthFailOp = ToDk(ds->front.depthFailOp);
    p->depthStencil.stencilFrontCompareOp = ToDk(ds->front.compareOp);
    p->depthStencil.stencilBackFailOp = ToDk(ds->back.failOp);
    p->depthStencil.stencilBackPassOp = ToDk(ds->back.passOp);
    p->depthStencil.stencilBackDepthFailOp = ToDk(ds->back.depthFailOp);
    p->depthStencil.stencilBackCompareOp = ToDk(ds->back.compareOp);
    p->stencilFront[0] = uint8_t(ds->front.writeMask);
    p->stencilFront[1] = uint8_t(ds->front.reference);
    p->stencilFront[2] = uint8_t(ds->front.compareMask);
    p->stencilBack[0] = uint8_t(ds->back.writeMask);
    p->stencilBack[1] = uint8_t(ds->back.reference);
    p->stencilBack[2] = uint8_t(ds->back.compareMask);
  }

  dkColorStateDefaults(&p->color);
  dkColorWriteStateDefaults(&p->colorWrite);
  const uint32_t targets = info.renderPass ? info.renderPass->colorCount : 0;
  if (const VkPipelineColorBlendStateCreateInfo* cb = info.pColorBlendState) {
    if (cb->logicOpEnable) {
      p->color.logicOp = DkLogicOp(cb->logicOp);  // Vulkan's and deko3d's orders are the same
    }
    for (uint32_t i = 0; i < std::min<uint32_t>(cb->attachmentCount, 8); ++i) {
      const VkPipelineColorBlendAttachmentState& a = cb->pAttachments[i];
      dkColorStateSetBlendEnable(&p->color, i, a.blendEnable);
      DkBlendState& b = p->blend[i];
      dkBlendStateDefaults(&b);
      dkBlendStateSetOps(&b, ToDk(a.colorBlendOp), ToDk(a.alphaBlendOp));
      dkBlendStateSetFactors(&b, ToDk(a.srcColorBlendFactor), ToDk(a.dstColorBlendFactor),
                             ToDk(a.srcAlphaBlendFactor), ToDk(a.dstAlphaBlendFactor));
      BlendWords(p->blendWords[i], i, b);
      dkColorWriteStateSetMask(&p->colorWrite, i, a.colorWriteMask & 0xF);
    }
    p->blendCount = std::min<uint32_t>(cb->attachmentCount, 8);
    memcpy(p->blendConstants, cb->blendConstants, sizeof(p->blendConstants));
  }
  for (uint32_t i = p->blendCount; i < 8; ++i) {
    dkColorWriteStateSetMask(&p->colorWrite, i, i < targets ? 0xF : 0);
  }

  if (const VkPipelineDynamicStateCreateInfo* dyn = info.pDynamicState) {
    for (uint32_t i = 0; i < dyn->dynamicStateCount; ++i) {
      const uint32_t bit = DynamicBit(dyn->pDynamicStates[i]);
      if (bit == (1u << 31)) {
        VKDK_LOG_LIMITED(8, "dynamic state %d is not supported (fixed at the pipeline's value)",
                         int(dyn->pDynamicStates[i]));
        continue;
      }
      p->dynamic |= bit;
    }
  }
  if (const VkPipelineViewportStateCreateInfo* vp = info.pViewportState) {
    if (!(p->dynamic & 1u) && vp->pViewports && vp->viewportCount) {
      p->staticViewport = vp->pViewports[0];
      p->hasStaticViewport = true;
    }
    if (!(p->dynamic & 2u) && vp->pScissors && vp->scissorCount) {
      p->staticScissor = vp->pScissors[0];
      p->hasStaticScissor = true;
    }
  }

  // Groups hashed for the command buffers' skips: 0 rasterizer, 1 multisample, 2 colour, 3 colour write, 4 blend,
  // 5 depth/stencil, 6 stencil values, 7 vertex input.
  p->groupHash[0] = Hash64(&p->rasterizer, sizeof(p->rasterizer), 1) | 1;
  p->groupHash[1] = Hash64(&p->multisample, sizeof(p->multisample), 2) | 1;
  p->groupHash[2] = Hash64(&p->color, sizeof(p->color), 3) | 1;
  p->groupHash[3] = Hash64(&p->colorWrite, sizeof(p->colorWrite), 4) | 1;
  p->groupHash[4] = Hash64(p->blendWords, sizeof(p->blendWords[0]) * p->blendCount, 5 + p->blendCount) | 1;
  p->groupHash[5] = Hash64(&p->depthStencil, sizeof(p->depthStencil), 6) | 1;
  p->groupHash[6] = Hash64(p->stencilFront, 3, Hash64(p->stencilBack, 3, 7)) | 1;
  p->groupHash[7] = Hash64(p->attribs, sizeof(p->attribs[0]) * p->attribCount,
                           Hash64(p->vtxBuffers, sizeof(p->vtxBuffers[0]) * p->vtxBufferCount, 8)) |
                    1;
  *out = p;
  return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(VkDevice d, VkPipelineCache, uint32_t count,
                                                              const VkGraphicsPipelineCreateInfo* infos,
                                                              const VkAllocationCallbacks*, VkPipeline* out) {
  VKDK_TRACE("CreateGraphicsPipelines");
  VkResult result = VK_SUCCESS;
  for (uint32_t i = 0; i < count; ++i) {
    out[i] = nullptr;
    const VkResult r = MakeGraphics(d, infos[i], &out[i]);
    if (r != VK_SUCCESS) {
      result = r;
    }
  }
  return result;
}

static VKAPI_ATTR VkResult VKAPI_CALL CreateComputePipelines(VkDevice d, VkPipelineCache, uint32_t count,
                                                             const VkComputePipelineCreateInfo* infos,
                                                             const VkAllocationCallbacks*, VkPipeline* out) {
  VKDK_TRACE("CreateComputePipelines");
  VkResult result = VK_SUCCESS;
  for (uint32_t i = 0; i < count; ++i) {
    out[i] = nullptr;
    const VkComputePipelineCreateInfo& info = infos[i];
    if (!info.stage.module || !info.stage.module->source) {
      result = VK_ERROR_INVALID_SHADER_NV;
      continue;
    }
    VkPipeline p = new (std::nothrow) VkPipeline_T();
    if (!p) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    p->compute = true;
    if (info.layout) {
      p->layout = *info.layout;
    }
    p->stages[0] = d->shaders->Get(info.stage.module->source, info.stage.pSpecializationInfo);
    if (!p->stages[0]) {
      delete p;
      result = VK_ERROR_INVALID_SHADER_NV;
      continue;
    }
    p->stageMask = DkStageFlag_Compute;
    out[i] = p;
  }
  return result;
}

static VKAPI_ATTR void VKAPI_CALL DestroyPipeline(VkDevice, VkPipeline p, const VkAllocationCallbacks*) {
  VKDK_TRACE("DestroyPipeline");
  // Variants stay in the cache (and their code in code memory) for the next pipeline that needs them.
  delete p;
}

namespace vkdk {

#define VKDK_ENTRY(name, fn) \
  if (!strcmp(n, #name)) return reinterpret_cast<PFN_vkVoidFunction>(fn)

PFN_vkVoidFunction vkdk_pipeline_proc(const char* n) {
  VKDK_ENTRY(vkCreateShaderModule, CreateShaderModule);
  VKDK_ENTRY(vkDestroyShaderModule, DestroyShaderModule);
  VKDK_ENTRY(vkCreatePipelineCache, CreatePipelineCache);
  VKDK_ENTRY(vkDestroyPipelineCache, DestroyPipelineCache);
  VKDK_ENTRY(vkGetPipelineCacheData, GetPipelineCacheData);
  VKDK_ENTRY(vkCreatePipelineLayout, CreatePipelineLayout);
  VKDK_ENTRY(vkDestroyPipelineLayout, DestroyPipelineLayout);
  VKDK_ENTRY(vkCreateRenderPass, CreateRenderPass);
  VKDK_ENTRY(vkDestroyRenderPass, DestroyRenderPass);
  VKDK_ENTRY(vkCreateFramebuffer, CreateFramebuffer);
  VKDK_ENTRY(vkDestroyFramebuffer, DestroyFramebuffer);
  VKDK_ENTRY(vkCreateGraphicsPipelines, CreateGraphicsPipelines);
  VKDK_ENTRY(vkCreateComputePipelines, CreateComputePipelines);
  VKDK_ENTRY(vkDestroyPipeline, DestroyPipeline);
  return nullptr;
}

}  // namespace vkdk

static std::string g_pendingCachePath;
static std::string g_pendingPrebuiltPath;

extern "C" void vkdkSetShaderCachePath(const char* path) {
  g_pendingCachePath = path ? path : "";
}

extern "C" void vkdkSetPrebuiltShaderCachePath(const char* path) {
  g_pendingPrebuiltPath = path ? path : "";
}

namespace vkdk {
// Called by CreateDevice's caller side: the cache path set before the device existed is applied when it is made.
void ApplyPendingCachePath(VkDevice d) {
  if (!g_pendingCachePath.empty()) {
    d->shaders->SetCachePath(g_pendingCachePath.c_str());
  }
  if (!g_pendingPrebuiltPath.empty()) {
    d->shaders->SetPrebuiltPath(g_pendingPrebuiltPath.c_str());
  }
}
}  // namespace vkdk
