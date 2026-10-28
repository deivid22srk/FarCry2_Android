/* Custom GPU driver support (libadrenotools, BSD-2-Clause, by Billy Laws).
 *
 * The app links against the system Vulkan loader, so a Turnip (or any other)
 * driver cannot simply be dropped in: the loader only ever loads Qualcomm's
 * vendor driver. libadrenotools opens the system loader inside an isolated
 * linker namespace with a hook in place that makes the loader pick up the
 * user's libvulkan.so instead, and hands back a handle to that patched loader.
 *
 * From there two things have to be told to use it:
 *   - DXVK, which resolves Vulkan through one vkGetInstanceProcAddr, and
 *   - SDL, which would otherwise dlopen a second, stock loader just to create
 *     the Android surface -- a surface that then would not belong to the
 *     instance DXVK created.
 *
 * Loading a driver is only half the job; the other half is finding out that
 * it did not work. Every failure inside the hook lands in logcat (tag
 * "hook_impl"), never in log.txt, and a half-working driver used to end as a
 * black screen or a silent fall back. So this file:
 *   - checks the user's library is really an arm64 ELF before opening it,
 *   - mirrors every decision to logcat (tag "fc2-driver") next to the hook's
 *     own diagnostics, and
 *   - probes the loaded loader with a throwaway VkInstance and enumerates the
 *     devices BEFORE the game starts. The probe is the moment the hook swaps
 *     the ICD, so "no devices" here means the custom driver did not take over
 *     and the system driver is kept instead of presenting a dead screen.
 * The probe's verdict is what the launcher shows as "última execução".
 *
 * With no driver selected none of this runs and everything stays on the
 * system driver, which is the default.
 */
#include "rt.h"

#ifdef __ANDROID__

#include <adrenotools/driver.h>
#include <dlfcn.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <sys/stat.h>
#include <android/log.h>

extern "C" void dxvk_vulkan_set_get_instance_proc_addr(void *pfn);
extern "C" void SDL_Vulkan_SetExternalLoader(void *handle, void *gipa);

/* One place for every driver line: log.txt through LOG() (respects the log
 * level) plus logcat, where libadrenotools' own hook diagnostics land too. */
static void drvlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void drvlog(const char *fmt, ...) {
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    LOG("%s", msg);
    __android_log_print(ANDROID_LOG_INFO, "fc2-driver", "%s", msg);
}

/* The slice of Vulkan this file needs, declared locally so the runtime build
 * does not depend on where (or whether) a vulkan.h is available. */
struct VkInstance_T;
struct VkPhysicalDevice_T;
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef int VkResult;
typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char *);
typedef VkResult (*PFN_vkCreateInstance)(const void *, const void *, VkInstance *);
typedef void (*PFN_vkDestroyInstance)(VkInstance, const void *);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t *, VkPhysicalDevice *);
typedef void (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, void *);

#define VK_PROBE_VERSION_1_1 0x00401000u
#define VK_PROBE_VERSION_1_0 0x00400000u

/* VkApplicationInfo (stable layout, 7 fields). */
typedef struct {
    uint32_t sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} ProbeAppInfo;

/* VkInstanceCreateInfo up to the fields the probe sets. */
typedef struct {
    uint32_t sType;
    const void *pNext;
    const ProbeAppInfo *pApplicationInfo;
    uint32_t flags;
} ProbeInstanceInfo;

/* Creates an instance and enumerates the devices the loader sees.
 * Returns 0 (VK_SUCCESS) with devices filled in when the ICD behind the
 * loader is alive; a negative sentinel means "the loader lacks
 * vkCreateInstance" (probe skipped, not a failure). */
static const int PROBE_NO_CREATE = -1;
static VkResult probe_devices(PFN_vkGetInstanceProcAddr gipa, uint32_t api_version,
                              VkInstance *out_inst, std::vector<VkPhysicalDevice> *out_devs) {
    auto vkCreateInstance = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
    auto vkEnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices)gipa(nullptr, "vkEnumeratePhysicalDevices");
    if (!vkCreateInstance || !vkEnumeratePhysicalDevices) return PROBE_NO_CREATE;

    static const char probe_name[] = "fc2-driver-probe";
    ProbeAppInfo ai;
    ai.sType = 1;  /* VK_STRUCTURE_TYPE_APPLICATION_INFO */
    ai.pNext = nullptr;
    ai.pApplicationName = probe_name;
    ai.applicationVersion = 1;
    ai.pEngineName = probe_name;
    ai.engineVersion = 1;
    ai.apiVersion = api_version;

    ProbeInstanceInfo ci;
    ci.sType = 0;  /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO */
    ci.pNext = nullptr;
    ci.pApplicationInfo = &ai;
    ci.flags = 0;

    VkInstance inst = nullptr;
    VkResult r = vkCreateInstance(&ci, nullptr, &inst);
    if (r != 0) return r;
    *out_inst = inst;

    uint32_t n = 0;
    VkResult er = vkEnumeratePhysicalDevices(inst, &n, nullptr);
    if (er != 0) return er;
    if (!n) return 0;  /* enumerated fine, but nothing behind the loader */
    if (n > 8) n = 8;
    out_devs->assign(n, nullptr);
    er = vkEnumeratePhysicalDevices(inst, &n, out_devs->data());
    return er;
}

/* The launcher validates packs on import; this is the last line of defence
 * before a driver for the wrong architecture ends up inside the hook, where
 * it would only show up as an opaque failure in logcat. */
static int usable_arm64_elf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char h[20];
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < sizeof h) return 0;
    if (h[0] != 0x7f || h[1] != 'E' || h[2] != 'L' || h[3] != 'F') return 0;
    if (h[4] != 2 /* 64-bit */ || h[5] != 1 /* little endian */) return 0;
    unsigned machine = h[18] | (h[19] << 8);
    return machine == 183;  /* EM_AARCH64 */
}

/* Opens the loader with `driver_dir` swapped in and proves it works.
 * Returns 1 and fills the handle + vkGetInstanceProcAddr on success; 0 means
 * "stay on the system driver", which is always a safe fallback. */
extern "C" int usm_driver_open(const char *driver_dir, const char *native_lib_dir,
                               void **out_handle, void **out_gipa) {
    *out_handle = nullptr;
    *out_gipa = nullptr;

    if (!driver_dir || !driver_dir[0]) {
        drvlog("driver: system (no imported driver selected)");
        return 0;
    }
    if (!native_lib_dir || !native_lib_dir[0]) {
        drvlog("driver: no nativeLibraryDir, keeping the system driver");
        return 0;
    }

    /* libadrenotools stats customDriverDir + customDriverName, so the trailing
     * slash is not optional. */
    std::string dir(driver_dir);
    if (dir.back() != '/') dir += '/';
    std::string lib = dir + "libvulkan.so";

    struct stat st;
    if (stat(lib.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        drvlog("driver: %s is missing, keeping the system driver", lib.c_str());
        return 0;
    }
    if (!usable_arm64_elf(lib.c_str())) {
        drvlog("driver: %s is not an arm64 ELF library, keeping the system driver", lib.c_str());
        return 0;
    }

    /* Writable dir for the loader copies adrenotools patches (the official
     * example always passes one; below API 29 memfd may not exist and the
     * minSdk here is 28). */
    std::string tmp = std::string(g_save_dir) + "/driver-tmp";
    mkdir(tmp.c_str(), 0777);

    dlerror();
    void *handle = adrenotools_open_libvulkan(RTLD_NOW | RTLD_LOCAL, ADRENOTOOLS_DRIVER_CUSTOM,
                                              tmp.c_str(), native_lib_dir, dir.c_str(),
                                              "libvulkan.so", nullptr, nullptr);
    if (!handle) {
        /* Missing hook libraries, a namespace the kernel refuses, or a driver
         * built for another ABI all land here. The reason itself is only
         * visible in logcat (dlerror or the hook_impl tag). */
        const char *err = dlerror();
        drvlog("driver: libadrenotools could not open %s, keeping the system driver%s%s",
               lib.c_str(), err ? ": " : " (logcat tag hook_impl has the reason)", err ? err : "");
        return 0;
    }

    auto gipa = (PFN_vkGetInstanceProcAddr)dlsym(handle, "vkGetInstanceProcAddr");
    if (!gipa) {
        drvlog("driver: %s has no vkGetInstanceProcAddr, keeping the system driver", lib.c_str());
        return 0;
    }

    /* The decisive check: does the hook actually serve the custom ICD?
     * 1.1 first, then 1.0 for old system loaders (a loader older than the
     * requested apiVersion refuses to create the instance outright). */
    VkInstance inst = nullptr;
    std::vector<VkPhysicalDevice> devs;
    VkResult r = probe_devices(gipa, VK_PROBE_VERSION_1_1, &inst, &devs);
    if (r != 0 && r != PROBE_NO_CREATE && inst == nullptr)
        r = probe_devices(gipa, VK_PROBE_VERSION_1_0, &inst, &devs);

    if (inst) {
        auto vdi = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
        if (r == 0 && !devs.empty()) {
            auto gpdp = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
            if (gpdp) {
                /* The real VkPhysicalDeviceProperties is ~560 bytes; the pad
                 * keeps the callee's write inside this object. */
                struct { uint32_t apiVersion, driverVersion, vendorID, deviceID, deviceType;
                         char deviceName[256]; uint8_t uuid[16]; char pad[768]; } p;
                gpdp(devs[0], &p);
                drvlog("driver: ACTIVE %s (vulkan %u.%u.%u, driver %u.%u.%u) from %s",
                       p.deviceName,
                       p.apiVersion >> 22, (p.apiVersion >> 12) & 0x3ff, p.apiVersion & 0xfff,
                       p.driverVersion >> 22, (p.driverVersion >> 12) & 0x3ff, p.driverVersion & 0xfff,
                       lib.c_str());
                for (size_t i = 1; i < devs.size(); i++) {
                    gpdp(devs[i], &p);
                    /* not "driver: " so the launcher's status parser keeps
                     * showing the ACTIVE line */
                    drvlog("driver-dev: device %d: %s", (int)i, p.deviceName);
                }
            } else {
                drvlog("driver: ACTIVE (devices enumerated) from %s", lib.c_str());
            }
        } else {
            drvlog("driver: probe found no working device (VkResult %d) -- the custom driver did "
                   "not take over (logcat tag hook_impl), keeping the system driver", (int)r);
            if (vdi) vdi(inst, nullptr);
            return 0;
        }
        if (vdi) vdi(inst, nullptr);
    } else if (r == PROBE_NO_CREATE) {
        /* Loader so bare the probe cannot run: trust adrenotools' verdict. */
        drvlog("driver: ACTIVE (probe unavailable) from %s", lib.c_str());
    } else {
        /* vkCreateInstance failed through the custom loader on both tries:
         * the game would not get a device either. */
        drvlog("driver: probe vkCreateInstance failed (VkResult %d) -- keeping the system driver",
               (int)r);
        return 0;
    }

    *out_handle = handle;
    *out_gipa = (void *)gipa;
    return 1;
}

#else

extern "C" int usm_driver_open(const char *, const char *, void **out_handle, void **out_gipa) {
    *out_handle = nullptr;
    *out_gipa = nullptr;
    return 0;
}

#endif
