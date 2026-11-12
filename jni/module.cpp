// Pico 4 Steam Link head-wobble fix (Zygisk, app-side, version-independent).
//
// Root cause (found and measured by https://github.com/ClaikoCode, whose derived-velocity fix this
// module re-implements at the OpenXR layer): after a recenter the
// Pico runtime reports the head's STAGE *pose* and its *linear velocity* in different yaw frames.
// Steam Link reads the head via the standard OpenXR call xrLocateSpace(VIEW, STAGE, now, +velocity)
// and forwards that velocity to the PC, where SteamVR extrapolates the pose ~78 ms along the
// (wrong-after-recenter) direction -> the drift/wobble.
//
// The "derived35" fix: keep the official pose and angular velocity, but REPLACE the forwarded
// linear velocity with one derived from positions (how far the head moves over the next ~35 ms),
// which is automatically in the correct frame. This module does exactly that, but at the OpenXR
// layer instead of by patching Steam Link's bytes -- so it survives Steam Link updates and needs no
// APK modification (install official Steam Link, enable this module).
//
// Tunable via /data/local/tmp/pico4velfix-corr = "mode window_ms":
//   (file absent / default) = mode 1, 35 ms   (the fix, on)
//   "0"      = off / passthrough (for A/B)
//   "1 35"   = derive linear velocity over 35 ms   (try 35..40)
//   "2"      = zero linear velocity (comparison)
// Scoped strictly to the Steam Link process; never touches Pico system/tracking.

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cmath>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <elf.h>
#include <link.h>
#include <android/log.h>
#include "zygisk.hpp"

using zygisk::Api; using zygisk::AppSpecializeArgs;
#define TAG "Pico4VelFix"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ---- minimal OpenXR ABI ----
using XrResult = int32_t; using XrSpace = uint64_t; using XrTime = int64_t;
struct Quat { float x, y, z, w; };
struct Vec3 { float x, y, z; };
struct XrPosef { Quat o; Vec3 p; };
// XrSpaceLocation: type(4)+pad(4)+next(8)+locationFlags(8)+pose(28)
struct XrSpaceLocation { int32_t type; int32_t _p; void *next; uint64_t flags; XrPosef pose; };
static constexpr int XR_TYPE_SPACE_LOCATION = 42;
static constexpr uint64_t XR_SPACE_LOCATION_POSITION_VALID = 0x2;   // POSITION_VALID_BIT
// XrSpaceVelocity: type(4)+pad(4)+next(8)+velocityFlags(8)+linearVelocity(12)+angularVelocity(12)
struct XrSpaceVelocity { int32_t type; int32_t _p; void *next; uint64_t flags; Vec3 lin; Vec3 ang; };
// Core struct, NOT an extension: XR_TYPE_SPACE_VELOCITY = 43 (confirmed in Steam Link's
// QSVLDeviceHmd::GetPose, which chains {type=43} onto {type=42}). The old value 1000070000 never
// matched, so the head locate was passed through untouched.
static constexpr int XR_TYPE_SPACE_VELOCITY = 43;
static constexpr uint64_t XR_SPACE_VELOCITY_LINEAR_VALID = 0x1;
// XrReferenceSpaceCreateInfo: referenceSpaceType @16
static constexpr uintptr_t RSCI_TYPE = 16;

using locatespace_fn = XrResult (*)(XrSpace, XrSpace, XrTime, XrSpaceLocation *);
using crs_fn = XrResult (*)(void *, const void *, uint64_t *);

static locatespace_fn g_ls = nullptr;     // REAL xrLocateSpace (for querying + calling through)
static crs_fn g_crs = nullptr;
static XrSpace g_local = 0, g_stage = 0, g_view = 0;
static const char *space_name(XrSpace s) {
    return s == g_view ? "VIEW" : s == g_local ? "LOCAL" : s == g_stage ? "STAGE" : "ctrl/other";
}
static void *discover_export(const char *sym);   // fwd

// walk a struct's next-chain for a given XrStructureType
static void *find_in_chain(void *next, int type) {
    while (next) {
        if (*reinterpret_cast<int32_t *>(next) == type) return next;
        next = *reinterpret_cast<void **>(reinterpret_cast<char *>(next) + 8);   // ->next @ +8
    }
    return nullptr;
}

// ---- tunable params: default ON (mode 1, 35 ms window) ----
static int g_mode = 1; static float g_win = 35.f;
static void read_params() {
    g_mode = 1; g_win = 35.f;                          // defaults when file is absent/empty
    int fd = open("/data/local/tmp/pico4velfix-corr", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char b[64] = {}; ssize_t n = read(fd, b, sizeof(b) - 1); close(fd);
    if (n <= 0) return;
    float w = 0; int m = 1;
    int got = sscanf(b, "%d %f", &m, &w);
    if (got >= 1) g_mode = m;
    if (got >= 2 && w > 0) g_win = w;
    if (g_win < 5.f) g_win = 5.f; if (g_win > 60.f) g_win = 60.f;
}

// Label the reference spaces so we can recognize the head (VIEW) locate. (VIEW=1, LOCAL=2, STAGE=3)
static XrResult wrap_crs(void *s, const void *ci, uint64_t *out) {
    if (!g_crs) g_crs = reinterpret_cast<crs_fn>(discover_export("xrCreateReferenceSpace"));
    if (!g_crs) return -2;
    XrResult r = g_crs(s, ci, out);
    if (r >= 0 && out) {
        int t = *reinterpret_cast<const int32_t *>(reinterpret_cast<const char *>(ci) + RSCI_TYPE);
        if (t == 1) g_view = *out; else if (t == 2) g_local = *out; else if (t == 3) g_stage = *out;
        LOGI("refSpace type=%d handle=0x%llx (%s)", t, (unsigned long long)*out, space_name(*out));
    }
    return r;
}

// The head pose+velocity Steam Link forwards to the PC comes through here. Replace the (wrong-frame)
// linear velocity with one derived from positions, in whatever base space the app requested (so it is
// correct regardless of recenter). Pose and angular velocity are left exactly as the runtime gave them.
static XrResult wrap_ls(XrSpace space, XrSpace base, XrTime t, XrSpaceLocation *loc) {
    if (!g_ls) g_ls = reinterpret_cast<locatespace_fn>(discover_export("xrLocateSpace"));
    if (!g_ls) return -2;
    XrResult r = g_ls(space, base, t, loc);
    if (r < 0 || !loc) return r;
    static unsigned calls = 0;                                     // diagnostic: prove the hook is on the hot path
    if ((calls++ % 2000) == 0)
        LOGI("xrLocateSpace #%u space=0x%llx (%s) base=%s chain0=%d", calls, (unsigned long long)space,
             space_name(space), space_name(base), loc->next ? *reinterpret_cast<int32_t *>(loc->next) : -1);
    if (g_view == 0 || space != g_view) return r;                 // only the head (VIEW) space
    if ((loc->flags & XR_SPACE_LOCATION_POSITION_VALID) == 0) return r;

    auto *vel = reinterpret_cast<XrSpaceVelocity *>(find_in_chain(loc->next, XR_TYPE_SPACE_VELOCITY));
    if (!vel) return r;                                            // no velocity requested -> nothing to fix

    read_params();
    static unsigned n = 0;
    if ((n++ % 200) == 0)
        LOGI("HEAD in %s lin[%.3f %.3f %.3f] ang[%.3f %.3f %.3f] mode=%d win=%.0fms",
             space_name(base), vel->lin.x, vel->lin.y, vel->lin.z,
             vel->ang.x, vel->ang.y, vel->ang.z, g_mode, g_win);

    if (g_mode == 1) {                                             // derive linear velocity from positions
        XrTime dt = (XrTime)(g_win * 1e6);                        // window in ns
        XrSpaceLocation l2{}; l2.type = XR_TYPE_SPACE_LOCATION;    // no velocity chained -> position only
        XrResult r2 = g_ls(space, base, t + dt, &l2);
        if (r2 >= 0 && (l2.flags & XR_SPACE_LOCATION_POSITION_VALID)) {
            float inv = 1000.f / g_win;                           // 1 / (window seconds)
            vel->lin = { (l2.pose.p.x - loc->pose.p.x) * inv,
                         (l2.pose.p.y - loc->pose.p.y) * inv,
                         (l2.pose.p.z - loc->pose.p.z) * inv };
        } else {
            vel->lin = { 0, 0, 0 };                               // failed future locate -> zero (as the reference build)
        }
        vel->flags |= XR_SPACE_VELOCITY_LINEAR_VALID;
    } else if (g_mode == 2) {                                     // comparison: zero linear velocity
        vel->lin = { 0, 0, 0 };
        vel->flags |= XR_SPACE_VELOCITY_LINEAR_VALID;
    }
    // mode 0 (or anything else) = passthrough; pose + angular velocity are never touched
    return r;
}

// ---- GOT patching (data-page writes only) ----
static volatile bool g_done = false; static int g_hooked = 0;
static bool write_ptr(void **slot, void *val) {
    uintptr_t pg = (uintptr_t)slot & ~(uintptr_t)0xfff;
    if (mprotect((void *)pg, 0x1000, PROT_READ | PROT_WRITE) != 0) return false;
    *slot = val; mprotect((void *)pg, 0x1000, PROT_READ);
    __builtin___clear_cache((char *)slot, (char *)slot + 8); return true;
}
static int patch_got(uintptr_t base, const char *sym, void *repl) {
    auto *eh = reinterpret_cast<ElfW(Ehdr) *>(base);
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) return 0;
    auto *ph = reinterpret_cast<ElfW(Phdr) *>(base + eh->e_phoff);
    ElfW(Dyn) *dyn = nullptr;
    for (int i = 0; i < eh->e_phnum; i++) if (ph[i].p_type == PT_DYNAMIC) { dyn = reinterpret_cast<ElfW(Dyn) *>(base + ph[i].p_vaddr); break; }
    if (!dyn) return 0;
    const char *strtab = nullptr; ElfW(Sym) *symtab = nullptr; ElfW(Rela) *jmprel = nullptr, *rela = nullptr;
    size_t pltsz = 0, relasz = 0, ent = sizeof(ElfW(Rela));
    auto fix = [base](ElfW(Addr) v) -> uintptr_t { return v < base ? base + v : v; };
    for (ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; d++) switch (d->d_tag) {
        case DT_STRTAB: strtab = (const char *)fix(d->d_un.d_ptr); break;
        case DT_SYMTAB: symtab = (ElfW(Sym) *)fix(d->d_un.d_ptr); break;
        case DT_JMPREL: jmprel = (ElfW(Rela) *)fix(d->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        case DT_RELA: rela = (ElfW(Rela) *)fix(d->d_un.d_ptr); break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_RELAENT: ent = d->d_un.d_val; break;
        default: break;
    }
    if (!strtab || !symtab) return 0;
    int nn = 0;
    auto scan = [&](ElfW(Rela) *rr, size_t sz) {
        for (size_t o = 0; rr && o < sz; o += ent) {
            auto *e = (ElfW(Rela) *)((uintptr_t)rr + o);
            uint32_t ty = ELF64_R_TYPE(e->r_info), si = ELF64_R_SYM(e->r_info);
            if (ty != R_AARCH64_JUMP_SLOT && ty != R_AARCH64_GLOB_DAT) continue;
            if (strcmp(strtab + symtab[si].st_name, sym)) continue;
            auto **slot = (void **)(base + e->r_offset);
            if (*slot != repl && write_ptr(slot, repl)) nn++;
        }
    };
    scan(jmprel, pltsz); scan(rela, relasz); return nn;
}
static void *find_export(const char *path, uintptr_t base, const char *sym) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) return nullptr;
    void *res = nullptr; ElfW(Ehdr) eh; ElfW(Shdr) *sh = nullptr; char *str = nullptr; ElfW(Sym) *sy = nullptr;
    if (pread(fd, &eh, sizeof(eh), 0) != (ssize_t)sizeof(eh) || !eh.e_shnum) goto done;
    { size_t z = (size_t)eh.e_shentsize * eh.e_shnum; sh = (ElfW(Shdr) *)malloc(z);
      if (!sh || pread(fd, sh, z, eh.e_shoff) != (ssize_t)z) goto done;
      ElfW(Shdr) *ds = nullptr; for (int i = 0; i < eh.e_shnum; i++) if (sh[i].sh_type == SHT_DYNSYM) { ds = &sh[i]; break; }
      if (!ds || ds->sh_link >= eh.e_shnum) goto done;
      ElfW(Shdr) *st = &sh[ds->sh_link]; str = (char *)malloc(st->sh_size); sy = (ElfW(Sym) *)malloc(ds->sh_size);
      if (!str || !sy || pread(fd, str, st->sh_size, st->sh_offset) != (ssize_t)st->sh_size
          || pread(fd, sy, ds->sh_size, ds->sh_offset) != (ssize_t)ds->sh_size) goto done;
      for (size_t i = 0; i < ds->sh_size / sizeof(ElfW(Sym)); i++)
        if (sy[i].st_shndx != SHN_UNDEF && sy[i].st_value && sy[i].st_name < st->sh_size
            && !strcmp(str + sy[i].st_name, sym)) { res = (void *)(base + sy[i].st_value); break; } }
done: free(sh); free(str); free(sy); close(fd); return res;
}
static void *discover_export(const char *sym) {
    FILE *f = fopen("/proc/self/maps", "re"); if (!f) return nullptr;
    char line[512], last[256] = {}; void *app = nullptr, *any = nullptr;
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s = 0, off = (uintptr_t)-1; char pm[8] = {}, path[256] = {};
        if (sscanf(line, "%lx-%*x %7s %lx %*s %*d %255s", &s, pm, &off, path) < 4) continue;
        if (off || path[0] != '/' || !strstr(path, "libopenxr") || !strcmp(path, last)) continue;
        strncpy(last, path, 255); void *p = find_export(path, s, sym);
        if (!p) continue; if (strstr(path, "/data/app")) { app = p; break; } if (!any) any = p;
    }
    fclose(f); return app ? app : any;
}
static bool has_openxr() {
    FILE *f = fopen("/proc/self/maps", "re"); if (!f) return false; char l[512]; bool y = false;
    while (fgets(l, sizeof(l), f)) if (strstr(l, "libopenxr")) { y = true; break; } fclose(f); return y;
}
static void try_hook() {
    FILE *f = fopen("/proc/self/maps", "re"); if (!f) return;
    char line[512], last[256] = {};
    while (fgets(line, sizeof(line), f)) {
        uintptr_t s = 0, off = (uintptr_t)-1; char pm[8] = {}, path[256] = {};
        if (sscanf(line, "%lx-%*x %7s %lx %*s %*d %255s", &s, pm, &off, path) < 4) continue;
        if (off || path[0] != '/' || !strcmp(path, last)) continue; strncpy(last, path, 255);
        if (strstr(path, "linker") || strstr(path, "/libc.so") || strstr(path, "libpxrruntime")) continue;
        int a = patch_got(s, "xrLocateSpace", (void *)&wrap_ls);
        int b = patch_got(s, "xrCreateReferenceSpace", (void *)&wrap_crs);
        if (a > 0) { g_hooked += a; LOGI("hooked xrLocateSpace in %s", path); }
        if (b > 0) LOGI("hooked xrCreateReferenceSpace in %s", path);
    }
    fclose(f);
    if (g_hooked > 0) { g_done = true; LOGI("active: head-velocity fix in place (tune via /data/local/tmp/pico4velfix-corr)"); }
}
static void *watcher(void *) {
    int noxr = 0;
    for (int i = 0; i < 3600 && !g_done; i++) {
        if (!has_openxr()) { if (++noxr > 1500) break; usleep(20000); continue; }   // ~30 s for the loader to appear
        try_hook(); usleep(i < 600 ? 20000 : 500000);   // poll fast at first so we hook before the ref spaces are created
    }
    return nullptr;
}
// Scope STRICTLY to Steam Link (Zygisk nice_name is correct even for app-zygote/USAP forks).
class CorrModule : public zygisk::ModuleBase {
    JNIEnv *env_ = nullptr;
public:
    void onLoad(Api *, JNIEnv *env) override { env_ = env; }
    void postAppSpecialize(const AppSpecializeArgs *args) override {
        bool target = false;
        if (env_ && args && args->nice_name) {
            const char *nm = env_->GetStringUTFChars(args->nice_name, nullptr);
            target = nm && strstr(nm, "steamlinkvr") != nullptr;
            if (nm) env_->ReleaseStringUTFChars(args->nice_name, nm);
        }
        if (!target) return;   // ONLY Steam Link; never Pico system/tracking processes
        pthread_t th; if (pthread_create(&th, nullptr, watcher, nullptr) == 0) pthread_detach(th);
    }
};
REGISTER_ZYGISK_MODULE(CorrModule)
