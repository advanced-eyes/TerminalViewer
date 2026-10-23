cmake_minimum_required(VERSION 3.24)

project(TerminalViewer VERSION 0.1.0 LANGUAGES C CXX)
if(APPLE)
    # 터미널 GPU 화면의 CAMetalLayer 붙이기(src/TerminalViewer/UI/TerminalSurfaceMac.mm)
    enable_language(OBJCXX)
endif()

# 테스트는 이 저장소를 따로 빌드할 때만 만든다. Ocean 이 서브모듈로 붙이면 OFF 다(README §6).
option(TERMINALVIEWER_BUILD_TESTS "Build unit tests (Qt Test)" ${PROJECT_IS_TOP_LEVEL})
# Windows 에서는 GPU 렌더러를 켠 빌드를 아직 확인하지 않았다: 확인할 때까지 기본은 OFF 다(-DTERMINALVIEWER_GPU=ON 으로 켤 수 있다).
# 부모 프로젝트가 같은 이름의 일반 변수를 먼저 정하면 그 값을 쓴다(CMP0077 NEW, 예: Ocean 이 OCEAN_TERMINAL_GPU 를 넘긴다).
if(WIN32)
    set(TERMINALVIEWER_GPU_DEFAULT OFF)
else()
    set(TERMINALVIEWER_GPU_DEFAULT ON)
endif()
option(TERMINALVIEWER_GPU "Build the Vulkan terminal renderer" ${TERMINALVIEWER_GPU_DEFAULT})

################################################################################
# Dependencies (상용 배포 라이선스 정책은 README §9 참고)
#   Qt5    : LGPLv3 모듈(Core, Gui, Widgets)만 쓰고 반드시 동적 링크한다.
#   Vulkan : TERMINALVIEWER_GPU=ON 일 때만 cmake/TerminalViewerDependencies.cmake 가 감사한 버전을 내려받는다.
################################################################################
find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Widgets)

foreach(TERMINALVIEWER_QT_MODULE Core Gui Widgets)
    get_target_property(TERMINALVIEWER_QT_LIBRARY_TYPE Qt5::${TERMINALVIEWER_QT_MODULE} TYPE)
    if(NOT TERMINALVIEWER_QT_LIBRARY_TYPE STREQUAL "SHARED_LIBRARY")
        message(FATAL_ERROR
            "Qt5::${TERMINALVIEWER_QT_MODULE} must be a shared library to keep LGPLv3 compliance "
            "(found: ${TERMINALVIEWER_QT_LIBRARY_TYPE}).")
    endif()
endforeach()

if(TERMINALVIEWER_GPU)
    include(cmake/TerminalViewerDependencies.cmake)
endif()

# 라이브러리와 테스트가 함께 쓰는 컴파일 옵션(Ocean 과 같다)
if(MSVC)
    # 소스에 한국어 글자가 있어 /utf-8 이 꼭 있어야 한다. /Zc:__cplusplus: __cplusplus 를 실제 표준 값으로 둔다(기본은 199711L).
    # /bigobj: 템플릿이 많은 파일의 섹션 수 한도를 늘린다.
    set(TERMINALVIEWER_COMPILE_OPTIONS /utf-8 /permissive- /W4 /Zc:__cplusplus /bigobj)
else()
    set(TERMINALVIEWER_COMPILE_OPTIONS -Wall -Wextra)
endif()
if(MINGW)
    # /bigobj 의 MinGW 판
    list(APPEND TERMINALVIEWER_COMPILE_OPTIONS -Wa,-mbig-obj)
endif()
set(TERMINALVIEWER_COMPILE_DEFINITIONS QT_DISABLE_DEPRECATED_BEFORE=0x050F00)
if(WIN32)
    # windows.h 의 min/max 매크로가 std::min/max 를 깨지 않게 하고, 쓰지 않는 API(소켓, 셸, OLE 등)는 읽지 않는다.
    list(APPEND TERMINALVIEWER_COMPILE_DEFINITIONS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

################################################################################
# TerminalViewer : 터미널 화면 렌더러(CPU/Vulkan) · 렌더 스레드 · 화면 위젯 정적 라이브러리
################################################################################
file(GLOB_RECURSE TERMINALVIEWER_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TerminalViewer/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/src/TerminalViewer/*.cpp)
if(NOT TERMINALVIEWER_GPU)
    # GPU 를 끈 구성에서는 Vulkan 코드(Strategy/Vulkan/, TerminalRendererGPU)를 빼고 CPU 로만 그린다.
    list(FILTER TERMINALVIEWER_SOURCES EXCLUDE REGEX "/src/TerminalViewer/Algorithm/TerminalRenderer/Strategy/(Vulkan/|TerminalRendererGPU\\.)")
endif()

add_library(TerminalViewer STATIC ${TERMINALVIEWER_SOURCES})
add_library(TerminalViewer::TerminalViewer ALIAS TerminalViewer)
# Ocean 이 add_subdirectory 로 붙여도 같게 돌도록 전역 CMAKE_* 대신 타깃 속성으로 정한다.
# CXX_STANDARD 는 .mm(OBJCXX)에도 같은 표준을 주려고 둔다(OBJCXX_STANDARD 가 없으면 CXX_STANDARD 를 따른다).
set_target_properties(TerminalViewer PROPERTIES
    AUTOMOC ON
    AUTORCC ON
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
target_compile_features(TerminalViewer PUBLIC cxx_std_17)
# 쓰는 쪽은 #include <TerminalViewer/Thread/TerminalRenderThread.h> 로 쓴다.
target_include_directories(TerminalViewer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(TerminalViewer PUBLIC Qt5::Core Qt5::Gui Qt5::Widgets)
target_compile_definitions(TerminalViewer PRIVATE ${TERMINALVIEWER_COMPILE_DEFINITIONS})
target_compile_options(TerminalViewer PRIVATE ${TERMINALVIEWER_COMPILE_OPTIONS})
if(TERMINALVIEWER_GPU)
    # 쓰는 쪽(테스트, Ocean)도 #ifdef TERMINALVIEWER_GPU 로 Vulkan 코드가 있는지 안다.
    target_compile_definitions(TerminalViewer PUBLIC TERMINALVIEWER_GPU=1)
    target_link_libraries(TerminalViewer PUBLIC TerminalViewerVulkanHeaders TerminalViewerVkBootstrap TerminalViewerVma)
    # 셰이더: 저장소의 SPIR-V 를 그대로 넣는다(일반 빌드에 glslc 가 필요 없다).
    # 원본(GLSL)이 shaders.sha256 과 다르면(고치고 SPIR-V 를 다시 만들지 않음) 구성을 멈춘다. 원본을 고치면 다음 빌드가 다시 구성한다.
    # 서브모듈로 붙어도 구성 때 돈다.
    include(cmake/CheckShaders.cmake)
    terminalviewer_check_shaders(${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders/terminal.vert
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders/terminal.frag
        ${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders/shaders.sha256)
    # :/terminalviewer/shaders/ 아래에 들어간다. 정적 라이브러리의 qrc 는 저절로 붙지 않아 VulkanContext 가 Q_INIT_RESOURCE 로 등록한다.
    target_sources(TerminalViewer PRIVATE resources/shaders/TerminalViewerShaders.qrc)
endif()
if(APPLE)
    # GLOB_RECURSE 는 .h/.cpp 만 모으므로 따로 넣는다. Vulkan 은 부르지 않고 레이어만 만든다(GPU 를 끈 빌드에도 들어간다).
    target_sources(TerminalViewer PRIVATE src/TerminalViewer/UI/TerminalSurfaceMac.mm)
    target_link_libraries(TerminalViewer PUBLIC "-framework QuartzCore" "-framework AppKit")
endif()
if(WIN32)
    # 터미널 렌더 스레드가 애니메이션 동안 타이머 해상도를 1ms 로 올린다(timeBeginPeriod).
    target_link_libraries(TerminalViewer PRIVATE winmm)
endif()

################################################################################
# 부모 프로젝트(Ocean)에게 넘기는 값(README §6)
#   캐시 INTERNAL 이라 add_subdirectory 깊이와 상관없이 부모에서 보인다. 구성할 때마다 다시 쓴다(GPU 를 끄면 빈 값).
################################################################################
if(TERMINALVIEWER_GPU)
    set(TERMINALVIEWER_VULKANHEADERS_LICENSE ${tv_vulkanheaders_SOURCE_DIR}/LICENSES/Apache-2.0.txt)
    set(TERMINALVIEWER_VKBOOTSTRAP_LICENSE ${tv_vkbootstrap_SOURCE_DIR}/LICENSE.txt)
    set(TERMINALVIEWER_VMA_LICENSE ${tv_vma_SOURCE_DIR}/LICENSE.txt)
else()
    set(TERMINALVIEWER_VULKANHEADERS_LICENSE "")
    set(TERMINALVIEWER_VKBOOTSTRAP_LICENSE "")
    set(TERMINALVIEWER_VMA_LICENSE "")
endif()
if(TERMINALVIEWER_GPU AND APPLE)
    set(TERMINALVIEWER_MOLTENVK_LICENSE ${tv_moltenvk_SOURCE_DIR}/LICENSE)
    set(TERMINALVIEWER_MOLTENVK_EMBEDDED_LICENSE ${CMAKE_CURRENT_SOURCE_DIR}/resources/licenses/LICENSE.MoltenVK-embedded.txt)
else()
    set(TERMINALVIEWER_MOLTENVK_DYLIB_PATH "")
    set(TERMINALVIEWER_MOLTENVK_LICENSE "")
    set(TERMINALVIEWER_MOLTENVK_EMBEDDED_LICENSE "")
endif()
set(TERMINALVIEWER_MOLTENVK_DYLIB "${TERMINALVIEWER_MOLTENVK_DYLIB_PATH}" CACHE INTERNAL
    "macOS + TERMINALVIEWER_GPU: libMoltenVK.dylib to put in the app bundle (Contents/Frameworks), else empty")
set(TERMINALVIEWER_VULKANHEADERS_LICENSE "${TERMINALVIEWER_VULKANHEADERS_LICENSE}" CACHE INTERNAL "Vulkan-Headers license text (GPU only)")
set(TERMINALVIEWER_VKBOOTSTRAP_LICENSE "${TERMINALVIEWER_VKBOOTSTRAP_LICENSE}" CACHE INTERNAL "vk-bootstrap license text (GPU only)")
set(TERMINALVIEWER_VMA_LICENSE "${TERMINALVIEWER_VMA_LICENSE}" CACHE INTERNAL "VulkanMemoryAllocator license text (GPU only)")
set(TERMINALVIEWER_MOLTENVK_LICENSE "${TERMINALVIEWER_MOLTENVK_LICENSE}" CACHE INTERNAL "MoltenVK license text (macOS + GPU only)")
set(TERMINALVIEWER_MOLTENVK_EMBEDDED_LICENSE "${TERMINALVIEWER_MOLTENVK_EMBEDDED_LICENSE}" CACHE INTERNAL
    "Notice for code statically linked into libMoltenVK.dylib (macOS + GPU only)")

# 배포물에 넣을 서드파티 라이선스 전문을 Ocean 과 같은 이름으로 설치한다. GPU 를 끈 구성에서는 아무것도 하지 않는다.
#   예: terminalviewer_install_licenses(${OCEAN_LICENSE_DESTINATION})
function(terminalviewer_install_licenses destination)
    if(TERMINALVIEWER_VKBOOTSTRAP_LICENSE)
        install(FILES ${TERMINALVIEWER_VKBOOTSTRAP_LICENSE} DESTINATION ${destination} RENAME LICENSE.vk-bootstrap-MIT)
    endif()
    if(TERMINALVIEWER_VMA_LICENSE)
        install(FILES ${TERMINALVIEWER_VMA_LICENSE} DESTINATION ${destination} RENAME LICENSE.VulkanMemoryAllocator-MIT)
    endif()
    if(TERMINALVIEWER_VULKANHEADERS_LICENSE)
        install(FILES ${TERMINALVIEWER_VULKANHEADERS_LICENSE} DESTINATION ${destination} RENAME LICENSE.Vulkan-Headers)
    endif()
    if(TERMINALVIEWER_MOLTENVK_LICENSE)
        install(FILES ${TERMINALVIEWER_MOLTENVK_LICENSE} DESTINATION ${destination} RENAME LICENSE.MoltenVK-Apache-2.0)
    endif()
    if(TERMINALVIEWER_MOLTENVK_EMBEDDED_LICENSE)
        install(FILES ${TERMINALVIEWER_MOLTENVK_EMBEDDED_LICENSE} DESTINATION ${destination})
    endif()
endfunction()

################################################################################
# TerminalViewerTestSupport : 테스트·도구용 화면 공급원(FakeFrameSource). 라이브러리 본체에는 넣지 않는다.
#   테스트 옵션과 상관없이 정의하되 EXCLUDE_FROM_ALL 이다: 링크하는 대상이 있을 때만 만든다(Ocean 의 테스트·TerminalRenderDemo).
################################################################################
add_library(TerminalViewerTestSupport STATIC EXCLUDE_FROM_ALL
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/Support/FakeFrameSource.h
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/Support/FakeFrameSource.cpp)
add_library(TerminalViewer::TestSupport ALIAS TerminalViewerTestSupport)
set_target_properties(TerminalViewerTestSupport PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF)
# 쓰는 쪽은 #include <FakeFrameSource.h> 로 쓴다.
target_include_directories(TerminalViewerTestSupport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/tests/Support)
target_link_libraries(TerminalViewerTestSupport PUBLIC TerminalViewer)
target_compile_definitions(TerminalViewerTestSupport PRIVATE ${TERMINALVIEWER_COMPILE_DEFINITIONS})
target_compile_options(TerminalViewerTestSupport PRIVATE ${TERMINALVIEWER_COMPILE_OPTIONS})

################################################################################
# Tests (Qt Test, 개발용 — Ocean 빌드에는 끼지 않는다)
################################################################################
if(TERMINALVIEWER_BUILD_TESTS)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Test)
    enable_testing()
    file(GLOB TERMINALVIEWER_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.h)
    # 테스트용 글꼴(Cascadia Mono)은 실행 파일에만 넣는다(:/terminalviewer/testfonts/, TestFonts::Register).
    add_executable(TerminalViewerTests ${TERMINALVIEWER_TEST_SOURCES} tests/fonts/TestFonts.qrc)
    set_target_properties(TerminalViewerTests PROPERTIES
        AUTOMOC ON
        AUTORCC ON
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF)
    target_include_directories(TerminalViewerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_link_libraries(TerminalViewerTests PRIVATE TerminalViewer TerminalViewerTestSupport Qt5::Test)
    target_compile_definitions(TerminalViewerTests PRIVATE ${TERMINALVIEWER_COMPILE_DEFINITIONS})
    target_compile_options(TerminalViewerTests PRIVATE ${TERMINALVIEWER_COMPILE_OPTIONS})
    add_test(NAME TerminalViewerTests COMMAND TerminalViewerTests)
    set_tests_properties(TerminalViewerTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32)
        # 실행 파일 옆에 DLL 이 없어도 Qt DLL 과 MinGW 런타임 DLL 을 찾게 Qt bin 폴더와 컴파일러 폴더를 PATH 앞에 둔다.
        # 콘솔 창 없이 돌면(IDE 등에서 부른 ctest) Qt Test 는 결과를 OutputDebugString 으로만 보내므로 QT_FORCE_STDERR_LOGGING 으로 출력에 남긴다.
        # Windows 의 offscreen 플랫폼은 시스템 글꼴 대신 Qt 의 lib/fonts(배포본에 없음)를 읽으므로 Windows 글꼴 폴더를 알려 준다.
        file(TO_CMAKE_PATH "$ENV{WINDIR}/Fonts" TERMINALVIEWER_WINDOWS_FONT_DIR)
        set(TERMINALVIEWER_TEST_ENVIRONMENT "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt5::Core>")
        if(MINGW)
            get_filename_component(TERMINALVIEWER_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
            list(APPEND TERMINALVIEWER_TEST_ENVIRONMENT "PATH=path_list_prepend:${TERMINALVIEWER_MINGW_BIN_DIR}")
        endif()
        list(APPEND TERMINALVIEWER_TEST_ENVIRONMENT "QT_FORCE_STDERR_LOGGING=set:1" "QT_QPA_FONTDIR=set:${TERMINALVIEWER_WINDOWS_FONT_DIR}")
        set_tests_properties(TerminalViewerTests PROPERTIES ENVIRONMENT_MODIFICATION "${TERMINALVIEWER_TEST_ENVIRONMENT}")
    endif()
    if(TERMINALVIEWER_GPU)
        # 셰이더 해시 검사(cmake/CheckShaders.cmake)가 원본이 한 글자만 바뀌어도 멈추는지 시험한다.
        add_test(NAME TerminalViewerShaderHashTest
                 COMMAND ${CMAKE_COMMAND} -DCHECK_SCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckShaders.cmake
                         -DSHADER_DIR=${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders
                         -DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/terminalviewer-shader-hash-test
                         -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/tests/ShaderHashTest.cmake)
    endif()
endif()
