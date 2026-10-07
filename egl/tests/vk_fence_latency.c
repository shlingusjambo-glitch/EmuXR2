// Raw Vulkan fence round trip (no ANGLE): empty submit + vkWaitForFences, or polling vkGetFenceStatus.
// Usage: vk_fence_latency [n] [poll 0/1]
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }
static int cmp(const void *a, const void *b) { double d = *(const double *)a - *(const double *)b; return (d > 0) - (d < 0); }
int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 200, poll = argc > 2 ? atoi(argv[2]) : 0;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "fence", 1, NULL, 0, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app};
    VkInstance inst; if (vkCreateInstance(&ici, NULL, &inst)) return 1;
    uint32_t c = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &c, &pd);
    float pri = 1; VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &pri};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q};
    VkDevice dev; if (vkCreateDevice(pd, &dci, NULL, &dev)) return 2;
    VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence f; vkCreateFence(dev, &fci, NULL, &f);
    double *ms = malloc(n * sizeof *ms);
    for (int i = 0; i < n; i++) {
        double t = now();
        vkQueueSubmit(queue, 0, NULL, f);
        if (poll) while (vkGetFenceStatus(dev, f) == VK_NOT_READY) usleep(100);
        else vkWaitForFences(dev, 1, &f, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &f);
        ms[i] = now() - t;
    }
    qsort(ms, n, sizeof *ms, cmp);
    printf("vk fence ms (n %d, %s): p50 %.2f p90 %.2f p99 %.2f max %.2f\n", n, poll ? "poll" : "wait", ms[n / 2], ms[n * 9 / 10], ms[n * 99 / 100], ms[n - 1]);
    return 0;
}
