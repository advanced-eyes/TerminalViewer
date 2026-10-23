################################################################################
# 서드파티 라이브러리(TERMINALVIEWER_GPU=ON 일 때만): 내려받기와 빌드 대상
#   - 저장소에는 서드파티 소스를 두지 않는다. 구성할 때 감사한 버전(태그 + SHA256)을 내려받는다. 출처·라이선스: docs/ThirdParty.md
#   - 서드파티의 CMakeLists.txt 는 실행하지 않는다(SOURCE_SUBDIR 에 없는 폴더를 준다). 빌드 대상은 아래에서 만든다.
#   - 처음 한 번 내려받은 뒤에는 -DFETCHCONTENT_FULLY_DISCONNECTED=ON 으로 인터넷 없이 구성할 수 있다.
#   - FetchContent 이름은 tv_, 대상 이름은 TerminalViewer 로 시작한다: 서브모듈로 붙인 부모(Ocean)의 이름과 겹치지 않게 한다.
#     FetchContent 이름은 내려받기 단계의 파일 경로에 네 번 들어가므로 짧게 둔다(Windows 경로 260자 한도, 긴 이름은 넘었다).
#   - 부모에게 넘기는 값(라이선스 파일 경로, MoltenVK dylib)은 CMakeLists.txt 의 캐시 변수와 terminalviewer_install_licenses() 다(README §6).
#
#   Vulkan-Headers 1.4.365 (Apache-2.0 OR MIT, 헤더만), vk-bootstrap 1.4.365 (MIT),
#   VulkanMemoryAllocator 3.4.0 (MIT, 헤더만), MoltenVK 1.4.2 (Apache-2.0, macOS 만. 미리 만든 dylib)
################################################################################
include(FetchContent)

set(TERMINALVIEWER_NO_UPSTREAM_CMAKE terminalviewer-no-upstream-cmake)   # 없는 폴더: FetchContent_MakeAvailable 이 add_subdirectory 를 하지 않는다

FetchContent_Declare(tv_vulkanheaders
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.365.tar.gz
    URL_HASH SHA256=ed832b3292cce324ebba6522fd2d3004bf1b797fa49bfbcad0b302ff23d56abc
    SOURCE_SUBDIR ${TERMINALVIEWER_NO_UPSTREAM_CMAKE})
FetchContent_Declare(tv_vkbootstrap
    URL https://github.com/charles-lunarg/vk-bootstrap/archive/refs/tags/v1.4.365.tar.gz
    URL_HASH SHA256=35eef8e046ca0a63f2ffc180a52561081d71bd18d19395a45fb78eb095423b4b
    SOURCE_SUBDIR ${TERMINALVIEWER_NO_UPSTREAM_CMAKE})
FetchContent_Declare(tv_vma
    URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.4.0.tar.gz
    URL_HASH SHA256=822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1
    SOURCE_SUBDIR ${TERMINALVIEWER_NO_UPSTREAM_CMAKE})
set(TERMINALVIEWER_GPU_DEPENDENCIES tv_vulkanheaders tv_vkbootstrap tv_vma)
if(APPLE)
    # 미리 만든 MoltenVK 릴리스(Apache-2.0). 쓰는 쪽이 libMoltenVK.dylib 를 앱 번들에 넣는다(TERMINALVIEWER_MOLTENVK_DYLIB).
    FetchContent_Declare(tv_moltenvk
        URL https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar
        URL_HASH SHA256=f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e
        SOURCE_SUBDIR ${TERMINALVIEWER_NO_UPSTREAM_CMAKE})
    list(APPEND TERMINALVIEWER_GPU_DEPENDENCIES tv_moltenvk)
endif()
FetchContent_MakeAvailable(${TERMINALVIEWER_GPU_DEPENDENCIES})

################################################################################
# Vulkan-Headers (Apache-2.0 OR MIT, 헤더만)
################################################################################
add_library(TerminalViewerVulkanHeaders INTERFACE)
target_include_directories(TerminalViewerVulkanHeaders SYSTEM INTERFACE ${tv_vulkanheaders_SOURCE_DIR}/include)

################################################################################
# vk-bootstrap (MIT)
################################################################################
add_library(TerminalViewerVkBootstrap STATIC ${tv_vkbootstrap_SOURCE_DIR}/src/VkBootstrap.cpp)
target_include_directories(TerminalViewerVkBootstrap SYSTEM PUBLIC ${tv_vkbootstrap_SOURCE_DIR}/src)
target_link_libraries(TerminalViewerVkBootstrap PUBLIC TerminalViewerVulkanHeaders ${CMAKE_DL_LIBS})
set_target_properties(TerminalViewerVkBootstrap PROPERTIES AUTOMOC OFF AUTORCC OFF CXX_STANDARD 17 CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(TerminalViewerVkBootstrap PRIVATE /utf-8 /W0)
else()
    target_compile_options(TerminalViewerVkBootstrap PRIVATE -w)
endif()

################################################################################
# VulkanMemoryAllocator (MIT, 헤더만. 구현은 src/.../Vulkan/VmaImplementation.cpp 한 곳)
################################################################################
add_library(TerminalViewerVma INTERFACE)
target_include_directories(TerminalViewerVma SYSTEM INTERFACE ${tv_vma_SOURCE_DIR}/include)
target_link_libraries(TerminalViewerVma INTERFACE TerminalViewerVulkanHeaders)

################################################################################
# MoltenVK (Apache-2.0, macOS 만)
################################################################################
set(TERMINALVIEWER_MOLTENVK_DYLIB_PATH "")
if(APPLE)
    set(TERMINALVIEWER_MOLTENVK_DYLIB_PATH ${tv_moltenvk_SOURCE_DIR}/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib)
    if(NOT EXISTS ${TERMINALVIEWER_MOLTENVK_DYLIB_PATH})
        message(FATAL_ERROR "TerminalViewer: libMoltenVK.dylib not found: ${TERMINALVIEWER_MOLTENVK_DYLIB_PATH}")
    endif()
endif()
