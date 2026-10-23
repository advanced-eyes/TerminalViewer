# TerminalViewer

터미널 화면 스냅샷을 그리는 **Qt5 C++ 정적 라이브러리**. Vulkan(GPU)으로 그리고, 쓸 수 없으면 CPU 로 그린다. Ocean 의 터미널 렌더러(터미널 기능 ② 렌더러)를 떼어 냈다. Ocean 은 이 저장소를 git 서브모듈로 붙여 쓴다.

- 작성일: 2026-10-24
- 상태: 단독으로 빌드 · 테스트된다(§7 단위 A). Ocean 에 붙이고 Ocean 의 사본을 지우는 일(단위 B)은 아직이다.
- 바탕: Ocean [터미널 GPU 렌더러 설계](../Ocean/docs/superpowers/specs/2026-10-08-terminal-renderer-design.md)와 Ocean `main` 6d539cc + 작업 트리(UI 재배치로 `src/UI/Common/Terminal/` 에 있던 판)의 코드. 로직은 그대로 옮겼다(§10).

## 1. 무엇인가

- 화면 공급원(`ITerminalFrameSource`)이 주는 스냅샷(`TerminalFrame`: 칸 · 커서 · 선택 · IME 조합)을 받아 그린다. 터미널 세션(PTY · 해석)은 이 라이브러리 밖이다.
- 그리기 목록(`DrawList`)을 만들고 화면마다 붙은 Strategy 로 그린다: GPU 는 Vulkan(macOS 는 MoltenVK), CPU 는 소프트웨어 합성.
- 렌더 스레드 하나가 모든 터미널 화면을 그린다. 대량 출력에도 렌더 비용은 화면 크기에만 비례하고, 픽셀 단위 스크롤 · 커서 깜빡임을 화면 주사율에 맞춘다.
- `TerminalSurface` 는 그리기 대상 위젯이다. 입력(키 · 마우스 · IME)은 다루지 않는다.

## 2. 폴더

```
TerminalViewer/
├── README.md               이 문서
├── CMakeLists.txt          라이브러리 · 테스트 지원 · 테스트 타깃
├── cmake/
│   ├── TerminalViewerDependencies.cmake   Vulkan 의존성 내려받기(TERMINALVIEWER_GPU=ON)
│   ├── CheckShaders.cmake                 셰이더 원본 해시 검사(구성할 때)
│   └── tests/ShaderHashTest.cmake         그 검사의 시험(CTest)
├── tools/shaders/BuildShaders.cmake       셰이더 원본 → SPIR-V 다시 만들기(glslc)
├── resources/
│   ├── shaders/            terminal.vert/.frag(원본) · *.spv(만든 것) · shaders.sha256 · TerminalViewerShaders.qrc
│   └── licenses/           LICENSE.MoltenVK-embedded.txt(macOS 배포물에 넣는 고지)
├── src/TerminalViewer/     라이브러리 코드. .h 와 .cpp 를 한 폴더에 둔다(Ocean 과 같은 배치)
│   ├── Interface/          IAlgorithm.h · IStrategy.h (namespace TerminalViewer)
│   ├── Algorithm/TerminalRenderer/
│   │   ├── Common/         TerminalFrame · DrawList(Builder) · Glyph* · CellMetrics · BoxGlyphPainter · SoftwareCompositor · ScrollAnimator · FrameScheduler · TerminalTheme
│   │   ├── Strategy/       TerminalRendererCPU · TerminalRendererGPU
│   │   │   └── Vulkan/     VulkanLoader · VulkanContext · VulkanView · VulkanOffscreen · VulkanAtlasTexture · VmaImplementation
│   │   └── TerminalRenderer · TerminalPlatform · TerminalRendererMode
│   ├── Thread/             Channel.h (namespace TerminalViewer) · TerminalRenderThread
│   └── UI/                 TerminalSurface · TerminalSurfaceNative.h · TerminalSurfaceMac.mm
├── tests/                  TerminalViewerTests (Qt Test)
│   ├── Support/            FakeFrameSource — 테스트 · 도구용 화면 공급원(TerminalViewerTestSupport 타깃)
│   ├── fonts/              Cascadia Mono 4개 + OFL.txt + TestFonts.qrc (테스트 실행 파일에만 넣는다)
│   └── TestMain.cpp · TestFonts.* · ScopedEnv.h · *Test.cpp
└── docs/                   CppConvention.md · Qt.md · Patterns.md(Ocean docs/ 에서 복사) · ThirdParty.md
```

## 3. 층

| 층 | 내용 | 의존 |
| --- | --- | --- |
| 렌더러 (`Algorithm/TerminalRenderer/`) | 스냅샷 → 그리기 목록(`DrawListBuilder`), 글리프(`GlyphRasterizer` · `GlyphAtlas` · `GlyphCache` · `BoxGlyphPainter`), Strategy(`TerminalRendererCPU` · `TerminalRendererGPU` = Vulkan), 화면마다 Strategy 를 고르는 `TerminalRenderer`, 스크롤 애니메이션 · 프레임 시각(`ScrollAnimator` · `FrameScheduler`) | QtCore · QtGui, (GPU) Vulkan-Headers · vk-bootstrap · VMA |
| 스레드 (`Thread/`) | `TerminalRenderThread`: 모든 화면이 함께 쓰는 렌더 스레드 하나. 명령은 `Channel` 로만 보낸다(docs/Qt.md QThread 패턴, docs/Patterns.md §2) | 렌더러, QtCore |
| 위젯 (`UI/`) | `TerminalSurface`: 화면을 렌더 스레드에 붙이고 떼는 위젯. GPU 는 네이티브 창(macOS CAMetalLayer, Windows HWND), CPU 는 조각 이미지를 그린다 | 스레드, QtWidgets |

- 라이브러리의 어떤 파일도 Ocean 헤더(`Core/`, `UI/Common/Theme`, `pch.h` 등)를 include 하지 않는다. `Observers` · `SystemStatus` 도 쓰지 않는다: 바뀜은 `TerminalRenderThread` · `TerminalSurface` 의 시그널로 알리고, Ocean 이 그것을 자기 `Observers` 에 잇는다.
- 타입 · 함수 이름은 Ocean 그대로 전역에 둔다. Ocean 쪽 고침을 include 줄로 줄이려는 것이다.
- 예외: `IAlgorithm` · `IStrategy` · `Channel` 세 템플릿만 `namespace TerminalViewer` 에 넣었다. Ocean 도 같은 이름의 전역 템플릿을 계속 갖는다(FileDecoder · RuleMatcher 가 쓴다). 한 번역 단위가 둘을 함께 include 하면 재정의 오류, 따로라도 정의가 갈라지면 ODR 위반이다.

## 4. 빌드 · 테스트

필요한 것: CMake 3.24+, C++17 컴파일러, Qt 5.15(Core · Gui · Widgets · Test, 동적 라이브러리). GPU 를 켜면 처음 구성할 때 인터넷(Vulkan 의존성 내려받기). Vulkan SDK 는 필요 없다.

```sh
# Windows (Qt 5.15.2 MinGW 8.1 예. 한 줄로 친다)
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/5.15.2/mingw81_64 -DCMAKE_C_COMPILER=C:/Qt/Tools/mingw810_64/bin/gcc.exe -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw810_64/bin/g++.exe
cmake --build build
ctest --test-dir build --output-on-failure

# macOS
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=$(brew --prefix qt@5)
cmake --build build && ctest --test-dir build --output-on-failure
```

- CTest: `TerminalViewerTests`(Qt Test 전체, `QT_QPA_PLATFORM=offscreen`), GPU 를 켜면 `TerminalViewerShaderHashTest` 도.
- 클래스 하나만: `build/TerminalViewerTests CellMetricsTest`. 클래스 이름 뒤의 인자는 Qt Test 에 넘긴다. MinGW Qt 는 콘솔에 결과를 잘 쓰지 않으니 `-o 결과.txt,txt` 로 받거나 `ctest --output-on-failure` 로 돈다.
- 직접 실행할 때(Windows): `QT_QPA_PLATFORM=offscreen`, `PATH` 앞에 Qt `bin` 과 MinGW `bin`, `QT_QPA_FONTDIR=C:/Windows/Fonts` 를 준다(ctest 는 이것을 스스로 정한다).
- 테스트용 글꼴(Cascadia Mono)은 테스트 실행 파일에만 들어간다(`:/terminalviewer/testfonts/`, `TestFonts::Register()`).

## 5. 옵션 · 환경 변수

| CMake 옵션 | 기본 | 뜻 |
| --- | --- | --- |
| `TERMINALVIEWER_GPU` | Windows `OFF`, 그 밖 `ON` | Vulkan 렌더러와 그 의존성을 만든다. `OFF` 이면 Vulkan 코드(`Strategy/Vulkan/`, `TerminalRendererGPU`)를 빼고 CPU 로만 그린다. 켜면 컴파일 정의 `TERMINALVIEWER_GPU=1` 을 PUBLIC 으로 내보낸다 |
| `TERMINALVIEWER_BUILD_TESTS` | 최상위 프로젝트면 `ON`, 서브모듈이면 `OFF` (`PROJECT_IS_TOP_LEVEL`) | `TerminalViewerTests` 와 CTest |

- Windows 기본이 `OFF` 인 까닭은 Ocean 과 같다: Windows 의 GPU 빌드를 아직 확인하지 않았다(§10).
- 부모 프로젝트가 `add_subdirectory` 전에 같은 이름의 **일반 변수**를 정하면 그 값을 쓴다(`cmake_minimum_required(VERSION 3.24)` 라 CMP0077 NEW).

| 환경 변수(개발용) | 뜻 |
| --- | --- |
| `OCEAN_VK_DISABLE=1` | Vulkan 이 없는 환경을 흉내 낸다(터미널은 CPU 로 그린다) |
| `OCEAN_VK_VALIDATION=1` | Vulkan SDK 로더로 검증 레이어를 켠다 |

- 이름은 Ocean 그대로 둔다(테스트와 Ocean README 가 이 이름을 쓴다). 로그 · 경고 문구도 그대로다(예: `Terminal renderer: GPU unavailable, drawing with CPU: …`, GPU 를 끈 빌드의 이유 문구 `…(OCEAN_TERMINAL_GPU=OFF)`).
- 앱 옵션 `--terminal-renderer <auto|cpu>` 를 읽는 것(`ParseTerminalRendererMode`)은 Ocean 에 남는다. 라이브러리는 그 결과(`ETerminalRendererMode`)만 받는다.

## 6. Ocean 에 붙이는 모양(단위 B)

```
Ocean/
├── external/TerminalViewer/   git 서브모듈
└── CMakeLists.txt
```

```cmake
# OCEAN_TERMINAL_GPU 를 정한 뒤, OceanCore 를 만들기 전에
set(TERMINALVIEWER_GPU ${OCEAN_TERMINAL_GPU})          # 일반 변수로 넘긴다(CMP0077 NEW)
add_subdirectory(external/TerminalViewer)
target_link_libraries(OceanCore PUBLIC TerminalViewer::TerminalViewer)

# Ocean 테스트(applicationForwardsRendererEvents)와 tools/TerminalRenderDemo 만
target_link_libraries(OceanTests PRIVATE TerminalViewer::TestSupport)
target_link_libraries(TerminalRenderDemo PRIVATE TerminalViewer::TestSupport)

# 설치: 지금 Ocean 이 OCEAN_TERMINAL_GPU 일 때 하는 라이선스 설치 대신
terminalviewer_install_licenses(${OCEAN_LICENSE_DESTINATION})
# macOS: 지금의 ${OCEAN_MOLTENVK_DYLIB} 대신
#   ${TERMINALVIEWER_MOLTENVK_DYLIB} 를 Ocean.app/TerminalRenderDemo.app 의 Contents/Frameworks 에 복사 · 설치
```

**링크 타깃**

| 타깃 | 별칭 | 내용 |
| --- | --- | --- |
| `TerminalViewer` (STATIC) | `TerminalViewer::TerminalViewer` | 라이브러리. PUBLIC: include 폴더 `src/`, `Qt5::Core/Gui/Widgets`, C++17, (GPU) `TERMINALVIEWER_GPU=1` · Vulkan 대상 · (macOS) QuartzCore · AppKit |
| `TerminalViewerTestSupport` (STATIC, `EXCLUDE_FROM_ALL`) | `TerminalViewer::TestSupport` | `FakeFrameSource`. 테스트 옵션과 상관없이 정의한다. 링크하는 대상이 있을 때만 만든다. include: `#include <FakeFrameSource.h>` |
| `TerminalViewerVulkanHeaders` · `TerminalViewerVkBootstrap` · `TerminalViewerVma` | — | GPU 를 켤 때만. `TerminalViewer` 가 PUBLIC 으로 링크한다 |

**부모가 돌려받는 값**(캐시 INTERNAL. `add_subdirectory` 뒤 부모 어디서나 보인다. GPU 를 끄면 빈 값)

| 변수 / 함수 | 뜻 | 지금 Ocean 에서 바꿀 것 |
| --- | --- | --- |
| `TERMINALVIEWER_MOLTENVK_DYLIB` | macOS + GPU: `libMoltenVK.dylib` 경로 | `OCEAN_MOLTENVK_DYLIB` (CMakeLists.txt 144–151, 233–239줄) |
| `TERMINALVIEWER_VKBOOTSTRAP_LICENSE` | vk-bootstrap `LICENSE.txt` | `${vkbootstrap_SOURCE_DIR}/LICENSE.txt` |
| `TERMINALVIEWER_VMA_LICENSE` | VulkanMemoryAllocator `LICENSE.txt` | `${vulkanmemoryallocator_SOURCE_DIR}/LICENSE.txt` |
| `TERMINALVIEWER_VULKANHEADERS_LICENSE` | Vulkan-Headers `LICENSES/Apache-2.0.txt` | `${vulkanheaders_SOURCE_DIR}/LICENSES/Apache-2.0.txt` |
| `TERMINALVIEWER_MOLTENVK_LICENSE` | macOS + GPU: MoltenVK `LICENSE` | `${moltenvk_SOURCE_DIR}/LICENSE` |
| `TERMINALVIEWER_MOLTENVK_EMBEDDED_LICENSE` | macOS + GPU: `resources/licenses/LICENSE.MoltenVK-embedded.txt` | Ocean `resources/licenses/LICENSE.MoltenVK-embedded.txt` |
| `terminalviewer_install_licenses(<폴더>)` | 위 파일을 지금 Ocean 과 같은 이름(`LICENSE.vk-bootstrap-MIT` · `LICENSE.VulkanMemoryAllocator-MIT` · `LICENSE.Vulkan-Headers` · `LICENSE.MoltenVK-Apache-2.0` · `LICENSE.MoltenVK-embedded.txt`)으로 설치한다 | CMakeLists.txt 175–183줄 |

- 셰이더 해시 검사(`CheckShaders.cmake`)는 GPU 를 켜면 서브모듈로 붙어도 구성 때 돈다. 셰이더는 `:/terminalviewer/shaders/` 에 들어간다(Ocean 의 `:/shaders/` 와 겹치지 않게). 정적 라이브러리의 qrc 는 저절로 붙지 않아 `VulkanContext` 가 셰이더를 처음 읽기 전에 `Q_INIT_RESOURCE(TerminalViewerShaders)` 를 부른다.
- 라이브러리는 글꼴을 갖지 않는다. Ocean 은 지금처럼 `:/fonts/` 로 Cascadia Mono 를 넣고 등록한다.
- include: Ocean 에게 이 라이브러리는 외부 라이브러리다. `docs/CppConvention.md` §9 대로 `<>` 로 쓰고 외부 라이브러리 묶음에 둔다.

```cpp
#include "Core/Application.h"

#include "Core/Observers.h"

#include <TerminalViewer/Thread/TerminalRenderThread.h>
#include <QString>

#include <memory>
```

| Ocean 경로(지금) | TerminalViewer 경로 |
| --- | --- |
| `Algorithm/TerminalRenderer/…` | `<TerminalViewer/Algorithm/TerminalRenderer/…>` |
| `Thread/TerminalRenderThread.h` | `<TerminalViewer/Thread/TerminalRenderThread.h>` |
| `UI/Common/Terminal/TerminalSurface.h`, `TerminalSurfaceNative.h` | `<TerminalViewer/UI/TerminalSurface.h>`, `<TerminalViewer/UI/TerminalSurfaceNative.h>` |
| `tests/FakeFrameSource.h` | `<FakeFrameSource.h>` (`TerminalViewer::TestSupport`) |
| `Interface/IAlgorithm.h` · `IStrategy.h` · `Thread/Channel.h` | Ocean 것은 그대로 남는다. 라이브러리 것은 `TerminalViewer::` 안에 있어 겹치지 않는다 |

- Ocean 에서 지울 것(각각 `CMakeLists.txt` 의 그 줄도): `src/Algorithm/TerminalRenderer/`, `src/Thread/TerminalRenderThread.*`, `src/UI/Common/Terminal/TerminalSurface*`, `resources/shaders/`, `cmake/CheckShaders.cmake`, `cmake/tests/ShaderHashTest.cmake`(+ `OceanShaderHashTest`), `tools/shaders/`, `cmake/OceanDependencies.cmake` 의 GPU 부분, `resources/licenses/LICENSE.MoltenVK-embedded.txt`, `tests/FakeFrameSource.*`, 옮긴 테스트 17개 클래스(`TerminalSurfaceTest` 는 아래 3개만 남긴다).
- Ocean 에 남는 것: `TerminalThemeMapper`(`MakeTerminalTheme` · `ParseTerminalRendererMode`), `Application` 의 연결 코드, `Observers::TerminalRendererFellBack`, `tools/TerminalRenderDemo.cpp`, Ocean 의 `IAlgorithm` · `IStrategy` · `Channel`, `TerminalSurfaceTest` 의 `rendererModeOption` · `terminalThemeFollowsPalette` · `applicationForwardsRendererEvents`, Cascadia Mono 글꼴과 `BundledFonts`.
- Ocean 코드의 `#ifdef OCEAN_TERMINAL_GPU`(지금 `tools/TerminalRenderDemo.cpp`)는 Ocean 이 계속 그 정의를 넣거나 라이브러리가 내보내는 `TERMINALVIEWER_GPU` 로 바꾼다.
- 받기: `git clone --recurse-submodules`, 이미 받았으면 `git submodule update --init`. Ocean `README.md` 의 빌드 절에 적는다.

## 7. 순서

| 단위 | 할 일 | 끝난 기준 |
| --- | --- | --- |
| A | 이 저장소를 만들고 라이브러리 · 테스트 · 셰이더 · Vulkan 의존성 · 문서를 옮긴다. Ocean 은 그대로다 | `TerminalViewerTests` 통과, 클래스별 통과 수가 Ocean 과 같다(`TerminalSurfaceTest` 는 3개 적게) — 지금 |
| B | Ocean 에 서브모듈로 붙이고 Ocean 의 사본을 지운다(§6) | 전체 `OceanTests` 통과, 동작 바뀜 없음, 설치 결과가 같다 |

- 코드는 [`../CLAUDE.md`](../CLAUDE.md)의 순서를 따른다: 구현 에이전트(테스트 → 옮기기 → 통과) → 리뷰 에이전트(`docs/` 규칙으로 정리 → 통과). 옮기기는 동작을 바꾸지 않는다.

## 8. 코드 규칙

- 이 저장소의 코드는 [docs/CppConvention.md](docs/CppConvention.md) · [docs/Qt.md](docs/Qt.md) · [docs/Patterns.md](docs/Patterns.md) 를 따른다. Ocean `docs/` 의 같은 파일(작업 트리 판)을 복사했다. 한쪽을 고치면 다른 쪽도 고친다.
- Ocean 과 다른 점: `pch.h` 가 없다. `.cpp` 의 맨 앞은 대응 헤더다. 라이브러리 안의 include 는 `"TerminalViewer/…"`, 쓰는 쪽은 `<TerminalViewer/…>` 다.

## 9. 라이선스

- Qt5 는 LGPLv3 로 쓴다: Core · Gui · Widgets 만 쓰고 **반드시 동적 링크**한다(공유 라이브러리가 아니면 구성이 멈춘다). Qt Charts · Qt Data Visualization 등 오픈소스판이 GPLv3 전용인 모듈은 쓰지 않는다.
- GPL · AGPL · 출처 불명 코드와 에셋은 들이지 않는다. LGPL 은 동적 링크로만 쓴다. 새 의존성은 Ocean `docs/LicenseCompliance.md` §2 의 감사 절차를 밟는다.
- 서드파티(Vulkan-Headers · vk-bootstrap · VulkanMemoryAllocator · MoltenVK)와 테스트용 Cascadia Mono 의 출처 · 해시 · 고지 의무: [docs/ThirdParty.md](docs/ThirdParty.md).
- 이 저장소 자체의 라이선스: **정하지 않음**.
- 원격 저장소 주소(서브모듈 URL): **정하지 않음**.

## 10. 출처 · 바꾼 것

- 스펙: Ocean `docs/superpowers/specs/2026-10-08-terminal-renderer-design.md`(상태: 구현 완료, Windows 확인 대기).
- 코드: Ocean `main` 6d539cc + 작업 트리. 작업 트리에서 위젯은 `src/UI/Terminal/` → `src/UI/Common/Terminal/` 로 옮겨져 있었다(이 저장소는 `src/TerminalViewer/UI/`).
- 옮기며 바꾼 것(동작은 같다):
  - include 경로: `"Algorithm/…"` → `"TerminalViewer/Algorithm/…"` 등. `TerminalRenderThread.cpp` 의 `pch.h` 를 뺐다.
  - `IAlgorithm` · `IStrategy` · `Channel` 을 `namespace TerminalViewer` 에 넣었다(§3).
  - 컴파일 정의 `OCEAN_TERMINAL_GPU` → `TERMINALVIEWER_GPU`. 문구는 그대로다(§5).
  - 셰이더 리소스: `:/shaders/` → `:/terminalviewer/shaders/`, qrc 이름 `shaders.qrc` → `TerminalViewerShaders.qrc`, `Q_INIT_RESOURCE` 추가(§6).
  - CMake: 전역 `CMAKE_*` 대신 타깃 속성. 셰이더 스크립트의 `ocean_` 함수 · 변수 이름을 `terminalviewer_` 로. Vulkan 대상 · FetchContent 이름을 겹치지 않게 바꿨다(버전 · URL · 해시는 그대로).
  - 테스트: `Core/BundledFonts.h` → `TestFonts.h`, `TerminalSurfaceTest` 의 Ocean 의존 3개를 뺐다(나머지 단언은 그대로).
- Windows 의 GPU 빌드(`TERMINALVIEWER_GPU=ON`)는 아직 확인되지 않았다. Ocean 도 마찬가지다. 2026-10-24 Windows · MinGW 8.1 에서 해 본 결과(고치지 않았다):
  - vk-bootstrap v1.4.365 의 `VkBootstrap.h` 가 `<cstddef>` 없이 `std::byte` 를 써서 GCC 8.1(libstdc++ 8)에서 컴파일되지 않는다. Ocean 의 플래그(`-std=c++17 -w`)로도 같다(옮기기와 상관없다).
  - 확인만 하려고 `-DCMAKE_CXX_FLAGS="-include cstddef"` 로 따로 빌드하면 모두 컴파일되고, 이 PC 의 Vulkan 장치로 `VulkanRendererTest` 의 `mixedMatchesCpu` · `partialUploadMatchesCpu` 두 개만 실패한다. 둘 다 컬러 이모지 글리프를 기대하는데 이 환경(offscreen)에는 컬러 이모지 글꼴이 없다(다른 테스트는 같은 까닭으로 건너뛴다). 나머지는 통과한다.
- macOS 전용 코드(`TerminalSurfaceMac.mm`, Metal 표면, MoltenVK)는 옮긴 뒤 아직 빌드해 보지 않았다. 바꾼 것은 include 경로뿐이다.
