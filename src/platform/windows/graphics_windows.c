/* Application-local software Vulkan; no registry or environment-driver override.
 * Direct driver loading works for elevated processes as well as ordinary users.
 * Raven keeps its existing Vulkan renderer and consumes our device through its
 * public API. All Windows loading and ownership details stay in this file. */
#define WIN32_LEAN_AND_MEAN
#define VK_USE_PLATFORM_WIN32_KHR
#define VK_NO_PROTOTYPES
#include "platform/windows/graphics_windows.h"
#include "platform/windows/diagnostics_windows.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <wchar.h>
#include <windows.h>

static struct {
    HMODULE loader, driver;
    VkInstance instance;
    VkDevice device;
    PFN_vkGetInstanceProcAddr address;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkDestroyDevice destroy_device;
} software;
static int started, prefer_software;

static void release_software(void)
{
    Rvulkan_set_external_device(NULL, NULL, NULL, NULL, 0);
    if (software.device && software.destroy_device)
        software.destroy_device(software.device, NULL);
    if (software.instance && software.destroy_instance)
        software.destroy_instance(software.instance, NULL);
    if (software.driver)
        FreeLibrary(software.driver);
    if (software.loader)
        FreeLibrary(software.loader);
    memset(&software, 0, sizeof(software));
}
static HMODULE packaged_library(const wchar_t *relative)
{
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(NULL, path, 32768);
    wchar_t *last = n && n < 32768 ? wcsrchr(path, L'\\') : NULL;
    if (!last || (size_t)(last - path) + wcslen(relative) + 2 > 32768)
        return NULL;
    wcscpy(last + 1, relative);
    HMODULE module =
        LoadLibraryExW(path, NULL,
                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_APPLICATION_DIR |
                           LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        fprintf(stderr,
                "[graphics] Cannot load packaged %ls (Windows error %lu). "
                "Extract the complete Windows ZIP, including software-vulkan.\n",
                relative, (unsigned long)GetLastError());
    return module;
}
static int has_extension(PFN_vkEnumerateInstanceExtensionProperties enumerate, const char *name)
{
    uint32_t count = 0;
    if (!enumerate || enumerate(NULL, &count, NULL) != VK_SUCCESS || count > 4096)
        return 0;
    VkExtensionProperties *items = calloc(count ? count : 1, sizeof(*items));
    if (!items)
        return 0;
    VkResult rc = enumerate(NULL, &count, items);
    int found = 0;
    if (rc == VK_SUCCESS)
        for (uint32_t i = 0; i < count; i++)
            if (!strcmp(items[i].extensionName, name))
                found = 1;
    free(items);
    return found;
}
static int software_error(const char *operation, VkResult rc)
{
    fprintf(stderr, "[graphics] Software Vulkan: %s failed (VkResult %d).\n", operation, rc);
    return 0;
}
static int make_software_device(void)
{
    software.loader = packaged_library(L"vulkan-1.dll");
    if (!software.loader)
        return 0;
    software.address =
        (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(software.loader, "vkGetInstanceProcAddr");
    if (!software.address)
        return software_error("load Vulkan entry point", VK_ERROR_INITIALIZATION_FAILED);
    PFN_vkEnumerateInstanceExtensionProperties enumerate =
        (PFN_vkEnumerateInstanceExtensionProperties)software.address(
            NULL, "vkEnumerateInstanceExtensionProperties");
    if (!has_extension(enumerate, VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME)) {
        fputs("[graphics] The packaged Vulkan loader does not support direct driver loading.\n",
              stderr);
        return 0;
    }
    software.driver = packaged_library(L"software-vulkan\\vulkan_lvp.dll");
    if (!software.driver)
        return 0;
    PFN_vkGetInstanceProcAddrLUNARG driver_address =
        (PFN_vkGetInstanceProcAddrLUNARG)(void *)GetProcAddress(software.driver,
                                                                "vk_icdGetInstanceProcAddr");
    if (!driver_address)
        return software_error("load Lavapipe entry point", VK_ERROR_INITIALIZATION_FAILED);
    /* A loader cannot enumerate extensions of an unregistered application driver
     * before instance creation. Query the supplied driver as specified by LUNARG. */
    enumerate = (PFN_vkEnumerateInstanceExtensionProperties)driver_address(
        NULL, "vkEnumerateInstanceExtensionProperties");
    if (!has_extension(enumerate, VK_KHR_SURFACE_EXTENSION_NAME) ||
        !has_extension(enumerate, VK_KHR_WIN32_SURFACE_EXTENSION_NAME))
        return software_error("Lavapipe Win32 surface support", VK_ERROR_EXTENSION_NOT_PRESENT);
    const char *extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
                                VK_LUNARG_DIRECT_DRIVER_LOADING_EXTENSION_NAME};
    VkDirectDriverLoadingInfoLUNARG driver = {
        .sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_INFO_LUNARG,
        .pfnGetInstanceProcAddr = driver_address};
    VkDirectDriverLoadingListLUNARG drivers = {
        .sType = VK_STRUCTURE_TYPE_DIRECT_DRIVER_LOADING_LIST_LUNARG,
        .mode = VK_DIRECT_DRIVER_LOADING_MODE_EXCLUSIVE_LUNARG,
        .driverCount = 1,
        .pDrivers = &driver};
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "CCurate",
                             .apiVersion = VK_API_VERSION_1_0};
    VkInstanceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pNext = &drivers,
                                 .pApplicationInfo = &app,
                                 .enabledExtensionCount = 3,
                                 .ppEnabledExtensionNames = extensions};
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)software.address(NULL, "vkCreateInstance");
    if (!create)
        return software_error("load vkCreateInstance", VK_ERROR_INITIALIZATION_FAILED);
    VkResult rc = create(&info, NULL, &software.instance);
    if (rc != VK_SUCCESS)
        return software_error("vkCreateInstance", rc);
#define INSTANCE(name)                                                                             \
    PFN_vk##name name = (PFN_vk##name)software.address(software.instance, "vk" #name)
    INSTANCE(DestroyInstance);
    software.destroy_instance = DestroyInstance;
    INSTANCE(EnumeratePhysicalDevices);
    INSTANCE(GetPhysicalDeviceProperties);
    INSTANCE(GetPhysicalDeviceQueueFamilyProperties);
    INSTANCE(GetPhysicalDeviceWin32PresentationSupportKHR);
    INSTANCE(CreateDevice);
    INSTANCE(GetDeviceProcAddr);
    if (!DestroyInstance || !EnumeratePhysicalDevices || !GetPhysicalDeviceProperties ||
        !GetPhysicalDeviceQueueFamilyProperties || !GetPhysicalDeviceWin32PresentationSupportKHR ||
        !CreateDevice || !GetDeviceProcAddr)
        return software_error("load instance entry points", VK_ERROR_INITIALIZATION_FAILED);
    VkPhysicalDevice devices[16];
    uint32_t count = 16;
    rc = EnumeratePhysicalDevices(software.instance, &count, devices);
    if (rc != VK_SUCCESS && rc != VK_INCOMPLETE)
        return software_error("vkEnumeratePhysicalDevices", rc);
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties properties;
        GetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU)
            continue;
        VkQueueFamilyProperties families[32];
        uint32_t family_count = 32;
        GetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, families);
        for (uint32_t family = 0; family < family_count; family++) {
            if (!families[family].queueCount ||
                !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
                !GetPhysicalDeviceWin32PresentationSupportKHR(devices[i], family))
                continue;
            float priority = 1;
            VkDeviceQueueCreateInfo queue_info = {.sType =
                                                      VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                  .queueFamilyIndex = family,
                                                  .queueCount = 1,
                                                  .pQueuePriorities = &priority};
            const char *device_extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
            VkDeviceCreateInfo device_info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                              .queueCreateInfoCount = 1,
                                              .pQueueCreateInfos = &queue_info,
                                              .enabledExtensionCount = 1,
                                              .ppEnabledExtensionNames = device_extensions};
            rc = CreateDevice(devices[i], &device_info, NULL, &software.device);
            if (rc != VK_SUCCESS)
                return software_error("vkCreateDevice", rc);
            software.destroy_device =
                (PFN_vkDestroyDevice)GetDeviceProcAddr(software.device, "vkDestroyDevice");
            PFN_vkGetDeviceQueue get_queue =
                (PFN_vkGetDeviceQueue)GetDeviceProcAddr(software.device, "vkGetDeviceQueue");
            if (!software.destroy_device || !get_queue)
                return software_error("load device entry points", VK_ERROR_INITIALIZATION_FAILED);
            VkQueue queue;
            get_queue(software.device, family, 0, &queue);
            Rvulkan_set_external_device(software.instance, devices[i], software.device, queue,
                                        family);
            fprintf(stderr, "[graphics] Software rendering device: %s (CPU, packaged Lavapipe).\n",
                    properties.deviceName);
            return 1;
        }
    }
#undef INSTANCE
    return software_error("find a CPU queue with Win32 presentation",
                          VK_ERROR_INITIALIZATION_FAILED);
}
static int native_can_present(void)
{
    HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    PFN_vkGetInstanceProcAddr address =
        loader ? (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(loader, "vkGetInstanceProcAddr")
               : NULL;
    VkInstance instance = Rvulkan_get_instance();
    PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR present =
        address ? (PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR)address(
                      instance, "vkGetPhysicalDeviceWin32PresentationSupportKHR")
                : NULL;
    return present && present(Rvulkan_get_physical_device(), Rvulkan_get_graphics_queue_family());
}
int cc_win_graphics_init(const RInitDesc *init)
{
    if (started)
        return 0;
    const char *mode = getenv("CCURATE_RENDERER");
    if (!mode || !*mode)
        mode = "auto";
    int automatic = !_stricmp(mode, "auto"), forced_software = !_stricmp(mode, "software");
    if (!automatic && !forced_software && _stricmp(mode, "vulkan")) {
        fputs("[graphics] CCURATE_RENDERER must be auto, software or vulkan.\n", stderr);
        return 0;
    }
    if (!forced_software && !(automatic && prefer_software)) {
        cc_win_startup_note("Trying system Vulkan rendering.");
        if (Rinit_ex(init)) {
            if (native_can_present()) {
                started = 1;
                cc_win_startup_note("System Vulkan rendering initialized.");
                return 1;
            }
            cc_win_startup_note("System Vulkan device cannot present to a Win32 window.");
            Rshutdown();
        }
        if (!automatic)
            return 0;
        cc_win_startup_note(
            "System Vulkan initialization failed; trying packaged software rendering.");
    }
    if (make_software_device() && Rinit_ex(init)) {
        started = prefer_software = 1;
        cc_win_startup_note("Software rendering active (CPU / Mesa Lavapipe).");
        return 1;
    }
    release_software();
    cc_win_startup_note("Software rendering initialization failed; see graphics messages above.");
    return 0;
}
void cc_win_graphics_shutdown(void)
{
    if (started)
        Rshutdown();
    started = 0;
    release_software();
}
