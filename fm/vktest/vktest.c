/* Minimal Vulkan driver smoke test for the Tegra K1 / mocha device.
 *
 * We avoid the Vulkan SDK headers on purpose: only the ABI layout of a
 * handful of structs is needed, and hard-coding it keeps this tool
 * buildable without pulling in external/vulkan-headers.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef int VkResult;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;

#define VK_SUCCESS 0

#define VK_STRUCTURE_TYPE_APPLICATION_INFO 0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO 1

typedef struct {
    uint32_t sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
} VkApplicationInfo;

typedef struct {
    uint32_t sType;
    const void *pNext;
    uint32_t flags;
    const VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

/* Only the leading fields of VkPhysicalDeviceProperties are needed. */
typedef struct {
    uint32_t sType;
    void *pNext;
    uint32_t apiVersion;
    uint32_t vendorID;
    uint32_t deviceID;
    uint32_t deviceType;
    char deviceName[256];
} VkPhysicalDevicePropertiesHead;

typedef VkResult (*PFN_vkCreateInstance)(const VkInstanceCreateInfo *,
                                         const void *, VkInstance *);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t *,
                                                   VkPhysicalDevice *);
typedef void (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, void *);
typedef VkResult (*PFN_vkEnumerateInstanceExtensionProperties)(
    const char *, uint32_t *, void *);
typedef VkResult (*PFN_vkEnumerateInstanceLayerProperties)(uint32_t *, void *);

static const char *result_name(VkResult r)
{
    switch (r) {
    case 0: return "VK_SUCCESS";
    case -1: return "VK_NOT_READY";
    case -2: return "VK_TIMEOUT";
    case -3: return "VK_EVENT_SET";
    case -4: return "VK_EVENT_RESET";
    case -5: return "VK_INCOMPLETE";
    case -7: return "VK_ERROR_OUT_OF_HOST_MEMORY";
    case -8: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
    case -9: return "VK_ERROR_INITIALIZATION_FAILED";
    case -10: return "VK_ERROR_DEVICE_LOST";
    case -11: return "VK_ERROR_EXTENSION_NOT_PRESENT";
    case -12: return "VK_ERROR_FEATURE_NOT_PRESENT";
    case -13: return "VK_ERROR_INCOMPATIBLE_DRIVER";
    case -14: return "VK_ERROR_TOO_MANY_OBJECTS";
    case -15: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
    case -16: return "VK_ERROR_FRAGMENTED_POOL";
    default: return "unknown";
    }
}

static const char *device_type_name(uint32_t t)
{
    switch (t) {
    case 0: return "OTHER";
    case 1: return "INTEGRATED_GPU";
    case 2: return "DISCRETE_GPU";
    case 3: return "VIRTUAL_GPU";
    case 4: return "CPU";
    default: return "unknown";
    }
}

int main(void)
{
    void *h = dlopen("libvulkan.so", RTLD_NOW);
    if (!h) {
        printf("FAIL: dlopen(libvulkan.so): %s\n", dlerror());
        return 1;
    }
    printf("OK: libvulkan.so loaded\n");

    /* DIAG: why does the shim fail to dlopen libvkcore.so? */
    void *vkcore = dlopen("libvkcore.so", RTLD_NOW);
    printf("DIAG dlopen(libvkcore.so): %s\n",
           vkcore ? "OK" : dlerror());
    if (vkcore) {
        void *fn = dlsym(vkcore, "NvGlEglGetFunctions");
        printf("DIAG NvGlEglGetFunctions: %s\n", fn ? "found" : "MISSING");
    }
    void *vkcore2 = dlopen("/vendor/lib/libvkcore.so", RTLD_NOW);
    printf("DIAG dlopen(/vendor/lib/libvkcore.so): %s\n",
           vkcore2 ? "OK" : dlerror());

    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)dlsym(h, "vkCreateInstance");
    PFN_vkEnumeratePhysicalDevices enumerate =
        (PFN_vkEnumeratePhysicalDevices)dlsym(h, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties get_props =
        (PFN_vkGetPhysicalDeviceProperties)dlsym(h,
                                                 "vkGetPhysicalDeviceProperties");
    PFN_vkEnumerateInstanceExtensionProperties enum_ext =
        (PFN_vkEnumerateInstanceExtensionProperties)dlsym(
            h, "vkEnumerateInstanceExtensionProperties");
    PFN_vkEnumerateInstanceLayerProperties enum_layers =
        (PFN_vkEnumerateInstanceLayerProperties)dlsym(
            h, "vkEnumerateInstanceLayerProperties");

    if (!create || !enumerate || !get_props) {
        printf("FAIL: missing core Vulkan entry points\n");
        return 2;
    }

    if (enum_ext) {
        uint32_t n = 0;
        VkResult r = enum_ext(NULL, &n, NULL);
        printf("instance extensions: result=%s count=%u\n",
               result_name(r), n);
    }
    if (enum_layers) {
        uint32_t n = 0;
        VkResult r = enum_layers(&n, NULL);
        printf("instance layers: result=%s count=%u\n", result_name(r), n);
    }

    VkApplicationInfo app;
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vktest";
    app.apiVersion = 0; /* let the driver pick its highest supported version */

    VkInstanceCreateInfo info;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;

    VkInstance inst = NULL;
    VkResult r = create(&info, NULL, &inst);
    printf("vkCreateInstance: %s\n", result_name(r));
    if (r != VK_SUCCESS || !inst)
        return 3;

    uint32_t count = 0;
    r = enumerate(inst, &count, NULL);
    printf("vkEnumeratePhysicalDevices: %s count=%u\n",
           result_name(r), count);
    if (r != VK_SUCCESS || count == 0)
        return 4;

    VkPhysicalDevice devs[8];
    if (count > 8)
        count = 8;
    r = enumerate(inst, &count, devs);
    printf("vkEnumeratePhysicalDevices(list): %s count=%u\n",
           result_name(r), count);

    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDevicePropertiesHead p;
        memset(&p, 0, sizeof(p));
        get_props(devs[i], &p);
        printf("device[%u]: sType=%u api=%u.%u.%u vendor=0x%04x "
               "device=0x%04x type=%s name='%s'\n",
               i, p.sType,
               (p.apiVersion >> 22) & 0x7ff,
               (p.apiVersion >> 12) & 0x3ff,
               p.apiVersion & 0xfff,
               p.vendorID, p.deviceID,
               device_type_name(p.deviceType), p.deviceName);
    }

    printf("RESULT: VULKAN_DRIVER_OK\n");
    return 0;
}
