// Vulkan Memory Allocator 의 구현을 이 파일 하나에만 넣는다.
// Vulkan 라이브러리를 링크하지 않으므로 정적 함수는 끄고, VmaVulkanFunctions 의 vkGetInstanceProcAddr/vkGetDeviceProcAddr 로 함수를 얻는다.
#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <vk_mem_alloc.h>
