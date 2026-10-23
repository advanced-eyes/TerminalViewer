#pragma once

#include <vulkan/vulkan_core.h>

#include <QString>

// Vulkan 라이브러리를 열고 vkGetInstanceProcAddr 를 돌려준다. 연 라이브러리는 프로세스가 끝날 때까지 닫지 않는다.
// 순서: 환경 변수 TERMINALVIEWER_VK_DISABLE=1 이면 nullptr(Vulkan 없는 환경 흉내)
//       → TERMINALVIEWER_VK_VALIDATION=1 이면 시스템 로더 + 검증 레이어
//       → 앱 번들의 Frameworks/libMoltenVK.dylib(macOS)
//       → 시스템 로더 기본 검색
// pLibraryUsed: 연 라이브러리(실패하면 이유)
// pValidation: 검증 레이어를 요청해야 한다(TERMINALVIEWER_VK_VALIDATION=1 이고 Vulkan 로더를 열었다). 레이어가 실제로 켜졌는지는
//              VulkanContext::IsValidationEnabled 가 알려 준다(로더가 레이어를 못 찾으면 꺼진다).
// 주의: vk-bootstrap 의 함수 표는 프로세스에 하나라서 처음 넘긴 vkGetInstanceProcAddr 가 계속 쓰인다.
//       그래서 프로세스 도중에 TERMINALVIEWER_VK_VALIDATION 을 바꿔 다른 라이브러리를 고르는 것은 지원하지 않는다.
PFN_vkGetInstanceProcAddr LoadVulkanLibrary(QString* pLibraryUsed, bool* pValidation);

// Vulkan 라이브러리를 열 수 있는지만 본다: LoadVulkanLibrary 와 같은 곳을 같은 순서로 열어 vkGetInstanceProcAddr 가 있는지 보고
// 바로 닫는다. Vulkan 함수를 부르지 않고 인스턴스를 만들지 않으므로 GUI 스레드에서 불러도 된다. TERMINALVIEWER_VK_DISABLE=1 이면 거짓.
bool IsVulkanLibraryAvailable();
