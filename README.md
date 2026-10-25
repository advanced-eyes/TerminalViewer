# TerminalViewer

터미널 화면 스냅샷을 그리는 **Qt5 C++ 정적 라이브러리**. Vulkan(GPU)으로 그리고, 쓸 수 없으면 CPU 로 그린다. Ocean 의 터미널 렌더러(터미널 기능 ② 렌더러)를 떼어 냈다. Ocean 은 이 저장소를 git 서브모듈로 붙여 쓴다.

- 작성일: 2026-10-24
- 상태: 단독으로 빌드 · 테스트된다(§7 단위 A). Ocean 은 이 저장소를 `external/TerminalViewer` 서브모듈로 붙여 쓰고 자기 사본을 지웠다(단위 B).
- 바탕: Ocean [터미널 GPU 렌더러 설계](../Ocean/docs/superpowers/specs/2026-10-08-terminal-renderer-design.md)와 Ocean `main` 6d539cc + 작업 트리(UI 재배치로 `src/UI/Common/Terminal/` 에 있던 판)의 코드. 로직은 그대로 옮겼다(§10).

## 1. 무엇인가

- 화면 공급원(`ITerminalFrameSource`)이 주는 스냅샷(`TerminalFrame`: 칸 · 커서 · 선택 · IME 조합)을 받아 그린다. 터미널 세션(PTY · 해석)은 이 라이브러리 밖이다.
- 그리기 목록(`DrawList`)을 만들고 화면마다 붙은 Strategy 로 그린다: GPU 는 Vulkan(macOS 는 MoltenVK), CPU 는 소프트웨어 합성.
- 렌더 스레드 하나가 모든 터미널 화면을 그린다. 대량 출력에도 렌더 비용은 화면 크기에만 비례하고 화면마다 주사율만큼만 그린다(§3 "프레임 흐름"). 픽셀 단위 스크롤 · 커서 깜빡임을 화면 주사율에 맞춘다.
- `TerminalSurface` 는 그리기 대상 위젯이다. 입력(키 · 마우스 · IME)은 다루지 않는다.

## 2. 폴더

```
TerminalViewer/
├── README.md               이 문서
├── CMakeLists.txt          라이브러리 · 테스트 지원 · 예제 · 테스트 타깃
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
│   │   ├── Common/         TerminalFrame · DrawList(Builder) · Glyph* · CellMetrics · BoxGlyphPainter · SoftwareCompositor · ScrollAnimator · FrameScheduler · TerminalTheme · TerminalContrast(최소 대비, §5)
│   │   ├── Strategy/       TerminalRendererCPU · TerminalRendererGPU
│   │   │   └── Vulkan/     VulkanLoader · VulkanContext · VulkanView · VulkanOffscreen · VulkanAtlasTexture · VmaImplementation
│   │   └── TerminalRenderer · TerminalPlatform · TerminalRendererMode
│   ├── Thread/             Channel.h (namespace TerminalViewer) · TerminalRenderThread
│   └── UI/                 TerminalSurface · TerminalSurfaceNative.h · TerminalSurfaceMac.mm
├── examples/
│   └── TerminalShowcase/   예제 앱(§4 "예제"): main.cpp · ShowcaseWindow(창 · 입력 · 위젯 바꿔 끼우기) · ShowcaseScene(FakeFrameSource 를 채우는 화면 내용)
├── tests/                  TerminalViewerTests (Qt Test)
│   ├── Support/TerminalViewer/Testing/   FakeFrameSource — 테스트 · 도구용 화면 공급원(TerminalViewerTestSupport 타깃, `<TerminalViewer/Testing/FakeFrameSource.h>`)
│   ├── fonts/              Cascadia Mono 4개 + OFL.txt + TestFonts.qrc (테스트 · 예제 실행 파일에만 넣는다)
│   └── TestMain.cpp · TestMainTest.cmake · ShowcaseGrabTest.cmake · ShowcaseArgsTest.cmake · TestFonts.* · ScopedEnv.h · *Test.cpp
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

### 프레임 흐름: 주기당 한 번 · CPU 역압 · 재시도

출력이 쏟아질 때 렌더 스레드가 주사율보다 자주 그리지 않게 한다(2026-10-25). 코드: `Common/FrameScheduler`, `Thread/TerminalRenderThread`.

- **주기당 한 번**: `Wake` 로 그리는 프레임은 그 화면의 프레임 간격(`SetRefreshRate`, 기본 60Hz)당 한 번 이하다. 마감(위상 기준 시각 + 간격)이 지난 뒤에 온 `Wake`(한가함)는 바로 그리고(키 입력 지연은 그대로), 마감 전에 온 `Wake` 는 마감에 그때의 최신 상태를 그린다. 그린 프레임은 무엇으로 그렸든(깨움 · 애니메이션 · 깜빡임 · 설정) 센다. 크기 · 배율 · 글꼴 · 테마 바꿈, 다시 보임, 포커스 바뀜, 첫 프레임은 기다리지 않는다(화면이 틀린 채로 남고, GUI 가 내는 드문 일이다). 깜빡임 규칙은 그대로다.
- **위상 고정**: 마감 전부터 기다린 `Wake` 를 마감 뒤 한 간격 안에 그렸으면(제때) 다음 마감은 이전 마감 + 간격이다. 렌더 스레드가 마감보다 조금씩 늦게 깨어나도(1ms 타이머로 0.3–1ms) 늦음이 쌓이지 않아, 쏟는 동안 간격대로 그린다(GPU 재시도 끝에 그려도 같다). 한가했다 온 `Wake`, 한 간격 넘게 늦게 그림(예: GUI 가 조각을 늦게 받음), 마감 전에 다른 까닭으로 그림, 첫 프레임은 그린 시각부터 다시 센다(놓친 마감을 따라잡으려 몰아서 그리지 않는다). 마감 전에 온 `Wake` 인지는 `Wake` 를 부른 시각으로 본다. 스크롤 애니메이션 프레임도 같은 규칙이다: 애니메이션 중이던 화면을 그 마감 뒤 한 간격 안에 그렸으면 다음은 이전 애니메이션 마감 + 간격이고, 애니메이션을 새로 시작했거나 한 간격 넘게 늦었거나 마감 전에 다른 까닭으로 그렸으면 그린 시각 + 간격이다.
- **정확한 주사율(Windows)**: `TerminalSurface` 는 `ScreenRefreshRate`(`TerminalPlatform.h`)를 넘긴다. Qt 5.15 의 windows QPA 는 `QScreen::refreshRate()` 를 정수로 버리므로(이 PC 59.951Hz → 59) Windows 에서는 그 화면의 모니터의 정확한 값을 읽는다: `QScreen::name()`(GDI 장치 이름 `\\.\DISPLAYn`)을 `QueryDisplayConfig` 활성 경로의 원본 이름(`DisplayConfigGetDeviceInfo` 의 `viewGdiDeviceName`)과 맞춰 그 대상의 주사율(유리수)을 쓴다. 모니터마다 다른 주사율을 따른다. 못 찾거나 API 가 실패하거나 Qt 값과 1Hz 이상 다르면 Qt 값을 쓴다. 다른 플랫폼은 `QScreen::refreshRate()` 그대로다. 판정은 순수 함수 `FindDisplayRefreshRate`(장치 이름 + 경로 목록 → Hz) · `ChooseRefreshRate`(Qt 값과 정확한 값 중 고르기)다. 링크: `user32`(PRIVATE).
- **Wake 합치기**: 렌더 스레드가 아직 캡처하지 않은 `Wake` 가 있으면 새 `Wake` 는 채널에 넣지 않는다(렌더 스레드를 깨우지 않는다). 다음 캡처가 그 바뀜도 본다.
- **CPU 역압**: CPU 화면은 GUI 가 아직 받지 않은 조각(`FrameReady`)이 하나 있으면 그리지 않고 그릴 일만 남긴다. `TerminalRenderThread` 가 자기 스레드(GUI)에서 워커의 조각을 받아 `FrameReady` 로 다시 내는 자리가 "받았다" 이고, 그 뒤 렌더 스레드가 그때의 최신 상태를 그린다. 건너뛴 동안에는 캡처 · 합성도 하지 않으므로 다음 조각이 마지막 조각 뒤에 바뀐 영역을 모두 덮는다. 떼면(또는 `Stop`) 받지 않은 조각도 함께 버린다.
- **재시도**: GPU 가 표시를 못 하면(펜스 · 이미지가 아직, `bNotReady`) 1ms 뒤에 다시 보고, 잇달아 못 하면 2 · 4ms … 로 늘리되 프레임 간격의 1/4(60Hz 4.17ms, 120Hz 2.08ms)을 넘지 않는다. 이 PC(FIFO)에서 상한 전에는 대량 출력 때 프레임마다 약 16.5ms(한 주기) 동안 표시를 못 해 25번쯤 다시 봤고, 상한 뒤에는 주사율과 맞으면 0회였다. 주사율을 화면(60Hz)보다 높게 주면 프레임마다 5.6ms(90Hz) · 8.7ms(120Hz) 동안 나며, 1ms 고정과 비교해 한 번에 다시 본 횟수(p50)가 4 → 2 · 6 → 4 로 줄고 fps(60) · `Wake` → 표시 지연(p50 16.7ms)은 같았다.
- 쓰는 쪽이 알 것:
  - `FrameReady` 를 직접 받는 쪽(위젯 밖의 도구 · 시험)은 이제 프레임이 덜 온다: 출력이 쏟아져도 화면마다 주사율만큼, GUI 가 바쁘면 GUI 가 받는 만큼만 온다. 프레임 수를 세는 도구는 결과가 달라진다.
  - `TerminalRenderThread` 객체가 사는 스레드의 이벤트 루프가 돌아야 CPU 화면이 계속 그려진다. `QTest::qWait` 는 이벤트를 한 번 처리하고 10ms 씩 자므로 프레임 수를 세는 시험은 `QEventLoop` 로 기다린다(`TerminalRenderThreadTest::RunEventLoop`).
  - `SetRefreshRate` 가 이제 `Wake` 프레임의 상한도 정한다(`TerminalSurface` 는 `ScreenRefreshRate` 를 넘긴다. 위젯 없이 붙인 화면은 60Hz). 위젯 없이 `SetRefreshRate` 를 직접 부르는 쪽은 `QScreen::refreshRate()` 대신 `ScreenRefreshRate(pScreen)` 을 넘긴다: Windows 에서 Qt 값(정수)을 넘기면 59.95Hz 화면을 59fps 로 그려 FIFO 에서 같은 프레임이 반복된다(이 PC 5초에 약 5번).
  - `TerminalSurface` 는 주사율을 붙일 때와 다른 화면으로 옮길 때만 보낸다(전과 같다). 같은 모니터의 주사율만 바꾸면(디스플레이 설정) 다시 붙일 때까지 옛 값으로 그린다.
  - 대량 출력 중 그린 내용의 나이는 최대 한 간격 + 그리기 시간이다.
  - Windows: 다음 마감이 50ms 안이면 1ms 타이머 해상도를 켠다(그대로). 출력이 이어지는 동안 켜져 있다.

이 PC, 120×40, 다른 스레드가 1kHz 로 화면 전체를 바꾸고 `Wake`(실제 창, 5초). 화면 59.951Hz(`QueryDisplayConfig`, DWM 59.950Hz, Qt 59). 위젯(`TerminalSurface`)이 정한 주사율로 잰다. "반복" = 그 동안의 vsync 수 − 표시한 프레임 수(FIFO 에서 같은 프레임이 다시 보인 횟수).

| | 전(상한 없음) | 후 |
| --- | --- | --- |
| CPU(`build/`): 초당 프레임 · 렌더 스레드 CPU | 175.2 · 99.1% | 60.0 · 32.8% |
| GPU(`build-gpu`, FIFO): 초당 프레임 · 반복 | 60.0 · 0 | 60.2 · 0 |
| GPU: 렌더 스레드 깨어남 · `IsReady` 거짓 | 1565/s · 7607회 | 106/s · 0회 |
| 대량 출력 중 `Wake` → 그리기 끝 p50(CPU · GPU) | 10.7 · 16.6ms | 21.7 · 16.8ms |
| 스크롤 애니메이션 중 프레임 간격 평균(CPU · GPU) | 17.25 · 17.46ms(58.0 · 57.3fps, Qt 59Hz) | 16.63 · 16.64ms(60.1fps) |
| GUI 를 200ms 막은 동안 쌓인 CPU 조각(`TerminalRenderThreadTest`) | 203 | 1 |
| 한가한 뒤 `Wake` → 그리기 끝 p50(CPU · GPU) | 0.08 · 0.30ms | 0.08 · 0.29ms |

- 주사율 60 을 직접 준 경우와 같다(CPU 60.0fps, GPU 60.2fps · 반복 0).
- 거쳐 온 단계(같은 조건): "그린 시각 + 간격" 이면서 Qt 59Hz 일 때 CPU 57.8fps, GPU 57.6fps · 반복 11.7. 깨움 위상 고정 + Qt 59Hz 일 때 59.0fps · 반복 4.7. 정확한 주사율을 쓰고 애니메이션만 "그린 시각 + 간격" 이면 애니메이션 간격 17.09 · 17.19ms(58.5 · 58.2fps).
- 애니메이션 간격은 그린 프레임 사이가 25ms 안인 것만 평균했다(스크롤 끝 무렵 픽셀이 바뀌지 않아 표시하지 않은 프레임을 낀 간격 9개는 뺐다. 이것을 넣으면 같은 "전" 이 17.9–18.2ms 로 보인다).

### CPU 프레임 비용: 합성 · 조각

Windows 기본 빌드(GPU 끔)와 RDP · VM 은 모두 CPU 로 그리므로 이 비용이 곧 체감 성능이다(2026-10-25). 코드: `Common/SoftwareCompositor`, `Strategy/TerminalRendererCPU`. 그린 결과는 전과 비트 단위로 같다(`SoftwareCompositorTest`: 옛 합성을 그대로 옮긴 기준 합성과 무작위 목록 600 씨드 · 큰 목록 · 부분 합성 이어 붙이기를 비교. 실제 쇼케이스 화면도 windows · offscreen QPA 에서 같았다).

- 합성(`Compose`)이 비쌌다: 바뀐 행 띠마다 목록의 쿼드 전부를 봐서 비용이 띠 수 × 쿼드 수였고(200×60 대량 출력: 띠 60 × 쿼드 24,000), 픽셀마다 채널 넷을 `/255` 로 섞었다(불투명 배경도).
  - 이어진 바뀐 행은 띠 하나로 합친다. 띠가 여럿이면 쿼드를 세로로 겹치는 띠에 한 번 나눠 담아(목록 순서 그대로, 행을 넘는 글리프는 겹치는 띠마다) 띠마다 그 쿼드만 그린다. 픽셀마다 결과는 지운 색 위에 그 픽셀을 덮는 쿼드를 목록 순서로 섞은 값이라 띠를 어떻게 나눠도 같다.
  - 섞기 식과 같은 값을 내는 빠른 길: 불투명 사각형은 채우기, 색이 0 인 사각형 · 단색 글리프(컬러 글리프는 쿼드 색을 쓰지 않는다) · 커버리지 0 · 네 바이트가 0 인 컬러 픽셀은 건너뛰기, 커버리지 255 이고 색이 불투명한 단색 글리프 · 알파 255 인 컬러 픽셀은 쓰기.
  - `(t + 127) / 255` 를 `((t + 128) * 257) >> 16`(`SoftwareCompositor::DivideBy255`)으로 바꿨다. 0..255² 모든 값에서 같다(전수 검사). 줄마다 `QImage::scanLine` 을 부르지 않고 포인터 · 줄 간격으로 쓴다.
- 조각(`FrameReady`)도 비쌌다: 바뀐 영역을 감싼 사각형을 매번 새 이미지로 복사했다(맨 위 · 맨 아래 줄만 바뀌어도 화면 전체, 200×60 에서 0.7ms). 이제 감싼 사각형이 화면의 절반 이상이면 화면 이미지를 복사하지 않고 그대로 조각으로 낸다(`rectPatch` = 화면 전체, QImage 암시적 공유). 받는 쪽이 조각을 쥐고 있는 동안 다음 합성이 쓰려 하면 그때 QImage 가 복사하므로 받은 조각은 바뀌지 않는다(`TerminalSurface` 는 받자마자 옮기고 놓는다). 절반보다 작으면 전처럼 그 사각형만 복사한다. 신호와 CPU 역압은 그대로다.
- 쓰는 쪽이 알 것: `FrameReady` 조각이 바뀐 영역보다 클 수 있다(화면 전체. 바뀌지 않은 픽셀도 지금 화면 그대로라 덮어 써도 같다). 조각을 오래 쥐는 쪽이 있으면 렌더 스레드가 다음 프레임에 화면 전체를 한 번 복사한다(전과 같은 비용).

이 PC, Release, Consolas 14px(셀 8×17), 배율 1. 단계별은 같은 파이프라인을 한 스레드에서 차례로 부른 300 프레임(GUI = `OnFrameReady` 복사 + RGB32 이미지에 `drawImage`), 렌더 스레드 · Wake 는 실제 창(위 표와 같은 1kHz 대량 출력, 5초 두 번). p50(p99), 120×40 · 200×60.

| | 전 | 후 |
| --- | --- | --- |
| 대량 출력(칸마다 무작위 글자색 · 배경색) 합성 | 4.93(8.05) · 13.32(18.01) ms | 0.69(1.08) · 1.71(2.36) ms |
| 같은 경우 조각 복사 | 0.31 · 0.71 ms | 0.01 · 0.01 ms |
| 같은 경우 한 프레임(캡처 → GUI 그리기) | 5.77 · 15.41 ms | 1.17 · 2.94 ms |
| 글자 대량 출력(기본 배경) 한 프레임 | 1.61 · 4.54 ms | 0.57 · 1.52 ms |
| 맨 위 · 맨 아래 줄만 바뀜 한 프레임 | 0.46 · 1.07 ms | 0.15 · 0.30 ms |
| 스크롤 애니메이션 한 프레임 | 0.84 · 1.84 ms | 0.23 · 0.48 ms |
| 대량 출력 중 렌더 스레드 CPU(두 번) | 32.2–33.8 · 91.3–93.1% | 8.1–9.1 · 17.5–17.8% |
| 대량 출력 중 `Wake` → 그리기 끝 p50(두 번) | 21.8 · 31.6 ms | 17.3–17.4 · 19.0–19.2 ms |

- `Wake` → 그리기 끝은 대량 출력 중 대략 프레임 간격(16.7ms) + 그리기 시간이라, 그리기 시간이 준 만큼 줄었다.
- 남은 비용(200×60 무작위 대량 출력, 지우기 포함 전체 합성으로 잼): 그리기 목록 0.95ms(`DrawListBuilder`, CPU · GPU 공통), 단색 글리프 약 1.4ms, 배경 사각형 0.3ms, GUI 0.25ms.
- 하지 않은 것: (1) 떨어진 띠를 띠마다 조각으로 보내기. 신호를 한 프레임에 여러 번 내야 해서 CPU 역압의 "받지 않은 조각 하나" 가 "받지 않은 프레임 하나" 로 바뀌고 `FrameReady` 수를 프레임 수로 세는 쪽(시험 · 도구)이 달라진다. 감싼 사각형이 절반 바로 아래(46%)인 경우 200×60 에서 복사 0.34ms + GUI 0.12ms 가 띠마다 보내면 약 0.01ms 다. (2) 스크롤 애니메이션 중 남은 그림을 옮기고 드러난 띠만 합성하기. 전체 합성이 0.11 · 0.19ms 로 줄어 옮기기(`memmove` 0.06 · 0.18ms)로 얻을 것이 거의 없고, GUI 로 가는 조각은 어차피 화면 전체다.

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

- CTest: `TerminalViewerTests`(Qt Test 전체, `QT_QPA_PLATFORM=offscreen`), `TerminalViewerTestMainTest`(첫 인자 처리 시험, `tests/TestMainTest.cmake`), 예제도 만들면 `TerminalShowcaseGrabTest`(예제를 `--renderer cpu --grab` 으로, 또 `--min-contrast 4.5` 를 더해 한 번 더 돌려 둘 다 0 으로 끝나고 PNG 가 남는지, `tests/ShowcaseGrabTest.cmake`. 그림은 `build/TerminalShowcaseGrab.png` · `build/TerminalShowcaseGrab-contrast.png`)와 `TerminalShowcaseArgsTest`(잘못된 인자는 모두 2, 도움말은 0 으로 끝나는지, `tests/ShowcaseArgsTest.cmake`), GPU 를 켜면 `TerminalViewerShaderHashTest` 도.
- 클래스 하나만: `build/TerminalViewerTests CellMetricsTest`. 클래스 이름 뒤의 인자는 Qt Test 에 넘긴다. 모르는 클래스 이름이면(옵션을 클래스 이름보다 먼저 쓴 경우 포함) 아무것도 돌리지 않고 클래스 목록을 표준 오류에 찍은 뒤 1 로 끝난다. MinGW Qt 는 콘솔에 결과를 잘 쓰지 않으니 `-o 결과.txt,txt` 로 받거나 `ctest --output-on-failure` 로 돈다.
- 직접 실행할 때(Windows): `QT_QPA_PLATFORM=offscreen`, `PATH` 앞에 Qt `bin` 과 MinGW `bin`, `QT_QPA_FONTDIR=C:/Windows/Fonts` 를 준다(ctest 는 이것을 스스로 정한다).
- GPU 빌드(Windows 도 같다): 위 구성 명령에 `-DTERMINALVIEWER_GPU=ON` 을 더한다(예: `-B build-gpu`). 실제 창이 있어야 보는 시험 3개는 offscreen(ctest)에서 건너뛰고, `QT_QPA_PLATFORM=windows` 로 직접 돌린다(창이 잠깐 뜬다. §10): `build-gpu/TerminalViewerTests TerminalSurfaceTest gpuSurfaceFillingWindowDraws gpuSurfaceResizesAndSkipsMinimized`, `build-gpu/TerminalViewerTests ShowcaseWindowTest gpuGrabCapturesTheScreen`. 같은 방법으로 `VulkanRendererTest` 를 돌리면 컬러 이모지까지 GPU 와 CPU 를 비교한다.
- 테스트용 글꼴(Cascadia Mono)은 테스트 · 예제 실행 파일에만 들어간다(`:/terminalviewer/testfonts/`, 테스트는 `TestFonts::Register()`, 예제는 `main.cpp` 가 등록한다).

### 예제: TerminalShowcase

`examples/TerminalShowcase/` 는 이 라이브러리가 그릴 수 있는 것을 한 창에 보이는 Qt Widgets 앱이다: 상자 문자 제목 틀, ANSI 16색 글자색 · 배경색, 256색(6x6x6 + 16 + 회색 24), 트루컬러 그라디언트, 글자 꾸밈(굵게 · 기울임 · 굵은 기울임 · 흐리게 · 반전 · 숨김 · 취소선), 대비가 낮은 글자(`contrast` 줄: 검정 위 어두운 파랑, 기본 배경 위 어두운 회색, 회색 위 회색, 고동색 위 빨강, 흰색 위 노랑, 흐린 회색. 모두 대비 3 미만이라 최소 대비를 켜면 고쳐진다), 색 밑줄 5종, 2칸 글자(한글 · 일본어 · 중국어), 이모지(ZWJ 조합은 `vecGraphemes`. 회색 · 흰색뿐인 컬러 이모지 🤍 · 🏳️(U+1F3F3 U+FE0F, `vecGraphemes`) 는 빨간 글자색으로 넣는다: 컬러로 판정되면 흰색, 단색으로 잘못 판정되면 빨간 실루엣), 상자 · 블록 문자(`BoxGlyphPainter`), 선택 강조, 프롬프트 줄의 깜빡이는 커서와 IME 조합 글자, 스크롤백(로그 수백 줄), 0.1초마다 바뀌는 실시간 줄(스피너 · 진행 막대 · 시각 · 막대 그래프).

- 최상위 빌드면 위의 `cmake --build build` 가 함께 만든다(`TERMINALVIEWER_BUILD_EXAMPLES`, §5). 실행 파일: Windows `build/TerminalShowcase.exe`, macOS `build/TerminalShowcase.app`(MoltenVK 를 번들의 `Contents/Frameworks/` 에 복사한다).
- 구조: `ShowcaseScene` 은 `FakeFrameSource` 를 채우는 순수 로직이다(열이 모자라면 조각을 다음 줄로 넘기고 그래도 넘치면 칸 경계에서 자른다. `ShowcaseSceneTest`). `ShowcaseWindow` 는 `TerminalSurface` 를 품고 위젯이 받지 않는 입력을 처리한다(`ShowcaseWindowTest`). `GridSizeChanged` 마다 공급원을 `Resize` 하고 다시 채운 뒤 `Wake` 한다. GPU 위젯이 `CpuFallbackRequested` 를 내면 같은 공급원으로 CPU 위젯을 먼저 만들고 옛 위젯을 `deleteLater` 한다. 끌 때는 창 → 렌더 스레드 `Stop` → 공급원 순서다.
- 창 제목에 지금 그리는 방법(`GPU (Vulkan)` / `CPU`)과 CPU 인 이유(`GetGpuError`, 없으면 `--renderer cpu` · `built with TERMINALVIEWER_GPU=OFF` · `TERMINALVIEWER_VK_DISABLE=1` · 네이티브 화면이 없는 QPA · `Vulkan library not found` 순), 격자, 글꼴 크기, 커서 모양, 최소 대비(`contrast off` / `contrast 4.5`)를 보인다.
- 글꼴: 테스트용 Cascadia Mono 를 넣어 등록한다. Cascadia 에 없는 글자(한글 · 가나 · 한자 · 이모지)는 Qt 글꼴 대체가 그린다(Windows: Malgun Gothic · Segoe UI Emoji 등). `windows` QPA 는 컬러 이모지로, offscreen 은 단색으로 그린다.

| 명령행 | 기본 | 뜻 |
| --- | --- | --- |
| `--renderer auto\|cpu` | `auto` | `auto` 는 GPU 를 쓸 수 있으면 GPU(Windows 기본 빌드는 GPU 를 빼서 CPU) |
| `--font-px N` | `14` | Cascadia Mono 픽셀 크기(6..48) |
| `--cols C --rows R` | `100` `30` | 처음 창 크기(칸). 창 크기를 바꾸면 격자를 다시 재고 화면을 다시 배치한다 |
| `--min-contrast R` | `1` | 최소 대비(WCAG 2.x 대비비 1..21, `1` = 끔. 소수 가능, 예: `4.5`). 테마의 `dMinContrastRatio`(§5) |
| `--grab <파일.png>` | — | 첫 프레임을 기다렸다가(0.5초 더 기다림) 창을 PNG 로 저장하고 끝난다(종료 코드는 아래). CPU 는 위젯을 그려 받고 GPU 는 화면에서 창 영역을 잡는다(창 자체를 잡으면 Vulkan 이 표시한 내용이 없다. 그 위를 덮은 창, Windows 11 의 둥근 창 모서리도 함께 찍힌다) |
| `--help`(`-h`, Windows 는 `-?` 도), `--help-all` | — | 도움말을 찍고 0 으로 끝난다(`--help-all` 은 Qt 옵션도) |

- 종료 코드: `0` 정상(창을 닫음 · `--grab` 저장함 · 도움말), `1` `--grab` 이 15초 안에 프레임을 받지 못했거나 창을 잡지 못함, `2` 잘못된 인자(모르는 옵션 · 값이 빠진 옵션 · 범위 밖 값 · `--renderer` 가 `auto`/`cpu` 아님 · 남는 인자. `--grab` 과 상관없이 창을 만들기 전에 끝난다), `3` `--grab` 이 한 가지 색뿐인 그림을 잡음, `4` `--grab` 저장 실패. 오류는 표준 오류에 `error: …` 로 쓴다.

| 키 · 마우스 | 동작 |
| --- | --- |
| 휠 | 스크롤(`ScrollPixels`, 한 칸 3줄). 프롬프트 줄이 화면 밖이면 커서 · 조합 글자를 숨긴다 |
| `PgUp` / `PgDn`, `Home` / `End` | 한 화면 / 맨 위 · 맨 아래로 스크롤 |
| `+` / `-` | 글꼴 크기(`SetFont` → `CellLayoutChanged` → `GridSizeChanged` → 다시 채우기) |
| `C` | 커서 모양: 블록 → 막대 → 밑줄 → 빈 블록 |
| `M` | 최소 대비: 켜져 있으면(1 보다 크면, `--min-contrast` 로 준 값 포함) 끔, 꺼져 있으면 4.5(`SetTheme` → 전체 다시 그리기) |
| `Esc` / `Q` | 끝내기 |

Windows PowerShell(저장소 폴더에서):

```powershell
$env:PATH = "C:\Qt\Tools\mingw810_64\bin;C:\Qt\5.15.2\mingw81_64\bin;$env:PATH"
.\build\TerminalShowcase.exe
.\build\TerminalShowcase.exe --renderer cpu --font-px 18 --cols 90 --rows 28
.\build\TerminalShowcase.exe --min-contrast 4.5
.\build\TerminalShowcase.exe --renderer cpu --grab build\showcase.png
```

Windows Git Bash(저장소 폴더에서. 마지막 줄은 창 없이 offscreen 으로 그린다):

```sh
export PATH="/c/Qt/Tools/mingw810_64/bin:/c/Qt/5.15.2/mingw81_64/bin:$PATH"
./build/TerminalShowcase.exe
./build/TerminalShowcase.exe --renderer cpu --grab build/showcase.png
QT_QPA_PLATFORM=offscreen QT_QPA_FONTDIR=C:/Windows/Fonts ./build/TerminalShowcase.exe --renderer cpu --grab build/showcase-offscreen.png
```

macOS(이 문서를 쓴 PC 에서는 돌려 보지 못했다):

```sh
./build/TerminalShowcase.app/Contents/MacOS/TerminalShowcase
./build/TerminalShowcase.app/Contents/MacOS/TerminalShowcase --renderer cpu --grab build/showcase.png
```

## 5. 옵션 · 환경 변수

| CMake 옵션 | 기본 | 뜻 |
| --- | --- | --- |
| `TERMINALVIEWER_GPU` | Windows `OFF`, 그 밖 `ON` | Vulkan 렌더러와 그 의존성을 만든다. `OFF` 이면 Vulkan 코드(`Strategy/Vulkan/`, `TerminalRendererGPU`)를 빼고 CPU 로만 그린다. 켜면 컴파일 정의 `TERMINALVIEWER_GPU=1` 을 PUBLIC 으로 내보낸다 |
| `TERMINALVIEWER_BUILD_TESTS` | 최상위 프로젝트면 `ON`, 서브모듈이면 `OFF` (`PROJECT_IS_TOP_LEVEL`) | `TerminalViewerTests` 와 CTest. 예제의 `ShowcaseScene` · `ShowcaseWindow` 는 예제 옵션과 상관없이 여기에 함께 컴파일해 시험한다 |
| `TERMINALVIEWER_BUILD_EXAMPLES` | 최상위 프로젝트면 `ON`, 서브모듈이면 `OFF` (`PROJECT_IS_TOP_LEVEL`) | 예제 `TerminalShowcase`(§4 "예제"). 테스트도 켜져 있으면 CTest `TerminalShowcaseGrabTest` · `TerminalShowcaseArgsTest` 를 더한다 |

- Windows 기본이 `OFF` 인 까닭은 Ocean 과 같다: 스펙 부록 C 의 Windows 확인 목록(MSVC 빌드, 도킹, 여러 GPU 에서 고른 장치 등)이 끝나지 않았다. MinGW 8.1 빌드 · 테스트와 한 PC 의 실제 창 확인은 했다(§10).
- 부모 프로젝트가 `add_subdirectory` 전에 같은 이름의 **일반 변수**를 정하면 그 값을 쓴다(`cmake_minimum_required(VERSION 3.24)` 라 CMP0077 NEW).

| 환경 변수(개발용) | 뜻 |
| --- | --- |
| `TERMINALVIEWER_VK_DISABLE=1` | Vulkan 이 없는 환경을 흉내 낸다(터미널은 CPU 로 그린다) |
| `TERMINALVIEWER_VK_VALIDATION=1` | Vulkan SDK 로더로 검증 레이어를 켠다 |

- 이름은 라이브러리 이름을 따른다(§10, Ocean 에서 쓰던 이름은 읽지 않는다). 문구에 든 이름도 같다: GPU 를 끈 빌드의 이유 문구(`GetGpuError`)는 `GPU 렌더러 없이 만든 빌드다(TERMINALVIEWER_GPU=OFF)` 이고, Vulkan 인스턴스의 앱 · 엔진 이름(드라이버 · 검증 레이어에 보인다)은 `TerminalViewer` 다. 그 밖의 로그 · 경고 문구는 옮겨 온 그대로다(예: `Terminal renderer: GPU unavailable, drawing with CPU: …`).
- 앱 옵션 `--terminal-renderer <auto|cpu>` 를 읽는 것(`ParseTerminalRendererMode`)은 Ocean 에 남는다. 라이브러리는 그 결과(`ETerminalRendererMode`)만 받는다.

### 테마: 최소 대비

`TerminalTheme::dMinContrastRatio`(`double`, 기본 `1` = 끔)는 글자색과 배경색의 WCAG 2.x 대비비(1..21)가 이보다 낮으면 글자색의 밝기를 옮겨 맞춘다(Ghostty `minimum-contrast`, xterm.js `minimumContrastRatio` 와 같은 뜻). 1 이하 · NaN 은 끔, 21 보다 크면 21. 끄면 그리기 목록은 이 옵션이 없던 때와 비트 단위로 같다. 값을 바꾼 `SetTheme` 는 다른 테마 바꿈처럼 행 캐시를 버리고 전체를 다시 그린다. 계산은 `DrawListBuilder` 에서 하므로 CPU 와 GPU 가 같은 색을 쓴다. 코드: `Common/TerminalContrast.h`.

- 고치는 방법: 상대 휘도(sRGB → 선형)로 대비를 재고, 글자색을 배경 반대쪽(글자가 배경보다 밝으면 흰색, 어두우면 검정)과 섞어 목표에 닿는 가장 작은 비율을 고른다(흰색 · 검정과 섞어 색조가 남는다). 그쪽으로 닿지 못하면 다른 쪽, 어느 쪽도 못 닿으면 대비가 더 큰 흰색 또는 검정. 예: 빨강 배경 위 밝은 글자는 4.5 에서 흰색으로도 못 닿아(4.0) 어두운 글자로 바뀐다.
- 순서: 반전 → 선택(선택 배경 · 선택 글자색으로 잰다) → 최소 대비 → 흐리게. 흐린 글자는 고친 색을 배경과 반씩 섞으므로 여전히 보통 글자보다 흐리다.
- 고치는 것: 글리프 색과 취소선 색, 조합 글자(IME), 블록 커서 위 글자(칸의 배경색으로 그리는 글자를 커서 색에 대해 맞춘다).
- 고치지 않는 것: 밑줄 색(따로 정한 색이다. 렌더러는 밑줄 색을 반전 · 선택으로도 바꾸지 않는다), 배경 · 커서 · 선택 색, 숨김 칸, 컬러 글리프(이모지), 그림 문자(상자 · 블록 U+2500-U+259F, Powerline U+E0B0-U+E0D7, Symbols for Legacy Computing U+1FB00-U+1FBFF · U+1CC00-U+1CEBF. 반칸 화소 · 구분선처럼 글자색 · 배경색 짝에 뜻이 있다. Ghostty 와 같은 범위).
- 비용: 켰을 때만 계산한다. 화면마다 (글자색, 배경색) → 결과를 기억한다(바로 앞 짝 + 8192 칸 표, 128 KiB. 켰을 때만 잡는다). 200x60 전체 다시 그리기(이 PC, Release): 로그처럼 색이 몇 가지면 0.49 → 0.53 ms, 칸마다 무작위 트루컬러(88% 가 고칠 대상)가 프레임마다 바뀌는 최악은 0.68 → 1.75 ms.

## 6. Ocean 에 붙인 모양(단위 B)

Ocean 은 이 저장소를 서브모듈로 붙여 쓴다. 아래의 `OCEAN_*` · `Ocean*` 이름은 Ocean 의 옵션 · 변수 · 대상이다(이 라이브러리는 그 이름을 모른다).

```
Ocean/
├── .gitmodules                [submodule "external/TerminalViewer"] url = ../TerminalViewer.git
├── external/TerminalViewer/   git 서브모듈
└── CMakeLists.txt
```

Ocean `CMakeLists.txt` 에서 이 라이브러리에 닿는 줄(간추림):

```cmake
# Ocean 의 GPU 옵션 OCEAN_TERMINAL_GPU 를 정한 뒤, OceanCore 를 만들기 전에
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/external/TerminalViewer/CMakeLists.txt)
    message(FATAL_ERROR "external/TerminalViewer is empty: run 'git submodule update --init' ...")
endif()
set(TERMINALVIEWER_GPU ${OCEAN_TERMINAL_GPU})          # 일반 변수로 넘긴다(CMP0077 NEW)
add_subdirectory(external/TerminalViewer)
target_link_libraries(OceanCore PUBLIC TerminalViewer::TerminalViewer)

# macOS + GPU: MoltenVK 를 앱 번들에 넣는다. TerminalRenderDemo 도 같은 POST_BUILD 복사를 한다.
if(APPLE AND OCEAN_TERMINAL_GPU)
    add_custom_command(TARGET Ocean POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_BUNDLE_CONTENT_DIR:Ocean>/Frameworks
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TERMINALVIEWER_MOLTENVK_DYLIB} $<TARGET_BUNDLE_CONTENT_DIR:Ocean>/Frameworks/libMoltenVK.dylib
        VERBATIM)
    install(FILES ${TERMINALVIEWER_MOLTENVK_DYLIB} DESTINATION Ocean.app/Contents/Frameworks)
endif()

# 설치: GPU 서드파티 라이선스. GPU 를 끄면 아무것도 하지 않는다.
terminalviewer_install_licenses(${OCEAN_LICENSE_DESTINATION})

# FakeFrameSource 를 쓰는 것만: 도구(OCEAN_BUILD_TOOLS)와 테스트(OCEAN_BUILD_TESTS, TerminalRendererBindingTest)
target_link_libraries(TerminalRenderDemo PRIVATE OceanCore TerminalViewer::TestSupport)
target_link_libraries(OceanTests PRIVATE OceanCore Qt5::Test TerminalViewer::TestSupport)
```

**링크 타깃**

| 타깃 | 별칭 | 내용 |
| --- | --- | --- |
| `TerminalViewer` (STATIC) | `TerminalViewer::TerminalViewer` | 라이브러리. PUBLIC: include 폴더 `src/`, `Qt5::Core/Gui/Widgets`, C++17, (GPU) `TERMINALVIEWER_GPU=1` · Vulkan 대상 · (macOS) QuartzCore · AppKit |
| `TerminalViewerTestSupport` (STATIC, `EXCLUDE_FROM_ALL`) | `TerminalViewer::TestSupport` | `FakeFrameSource`. 테스트 옵션과 상관없이 정의한다. 링크하는 대상이 있을 때만 만든다. include: `#include <TerminalViewer/Testing/FakeFrameSource.h>`(파일은 `tests/Support/TerminalViewer/Testing/`) |
| `TerminalViewerVulkanHeaders` · `TerminalViewerVkBootstrap` · `TerminalViewerVma` | — | GPU 를 켤 때만. `TerminalViewer` 가 PUBLIC 으로 링크한다 |

**부모가 돌려받는 값**(캐시 INTERNAL. `add_subdirectory` 뒤 부모 어디서나 보인다. GPU 를 끄면 빈 값)

| 변수 / 함수 | 뜻 | Ocean 이 쓰는 곳 |
| --- | --- | --- |
| `TERMINALVIEWER_MOLTENVK_DYLIB` | macOS + GPU: `libMoltenVK.dylib` 경로 | `Ocean` · `TerminalRenderDemo` 번들에 복사(POST_BUILD), `Ocean.app/Contents/Frameworks` 에 설치(위) |
| `TERMINALVIEWER_VKBOOTSTRAP_LICENSE` | vk-bootstrap `LICENSE.txt` | 직접 읽지 않는다(`terminalviewer_install_licenses` 가 쓴다) |
| `TERMINALVIEWER_VMA_LICENSE` | VulkanMemoryAllocator `LICENSE.txt` | 〃 |
| `TERMINALVIEWER_VULKANHEADERS_LICENSE` | Vulkan-Headers `LICENSES/Apache-2.0.txt` | 〃 |
| `TERMINALVIEWER_MOLTENVK_LICENSE` | macOS + GPU: MoltenVK `LICENSE` | 〃 |
| `TERMINALVIEWER_MOLTENVK_EMBEDDED_LICENSE` | macOS + GPU: `resources/licenses/LICENSE.MoltenVK-embedded.txt` | 〃 |
| `terminalviewer_install_licenses(<폴더>)` | 위 파일을 옮기기 전 Ocean 이 설치하던 것과 같은 이름(`LICENSE.vk-bootstrap-MIT` · `LICENSE.VulkanMemoryAllocator-MIT` · `LICENSE.Vulkan-Headers` · `LICENSE.MoltenVK-Apache-2.0` · `LICENSE.MoltenVK-embedded.txt`)으로 설치한다 | `terminalviewer_install_licenses(${OCEAN_LICENSE_DESTINATION})`(`licenses`, macOS 는 `Ocean.app/Contents/Resources/licenses`) |

- 셰이더 해시 검사(`CheckShaders.cmake`)는 GPU 를 켜면 서브모듈로 붙어도 구성 때 돈다. 셰이더는 `:/terminalviewer/shaders/` 에 들어간다(Ocean 의 `:/shaders/` 와 겹치지 않게). 정적 라이브러리의 qrc 는 저절로 붙지 않아 `VulkanContext` 가 셰이더를 처음 읽기 전에 `Q_INIT_RESOURCE(TerminalViewerShaders)` 를 부른다.
- 라이브러리는 글꼴을 갖지 않는다. Ocean 은 `:/fonts/` 로 Cascadia Mono 를 넣고 `BundledFonts` 로 등록한다.
- include: Ocean 에게 이 라이브러리는 외부 라이브러리다. `docs/CppConvention.md` §9 대로 `<>` 로 쓰고 외부 라이브러리 묶음에 둔다. 예: Ocean `src/Core/Application.cpp` 의 앞부분(간추림)

```cpp
#include "pch.h"
#include "Core/Application.h"

#include <TerminalViewer/Thread/TerminalRenderThread.h>
#include <QApplication>

#include "Core/BundledFonts.h"
```

옮긴 기록(Ocean 의 include 경로):

| 옮기기 전 Ocean 경로 | TerminalViewer 경로 |
| --- | --- |
| `Algorithm/TerminalRenderer/…` | `<TerminalViewer/Algorithm/TerminalRenderer/…>` |
| `Thread/TerminalRenderThread.h` | `<TerminalViewer/Thread/TerminalRenderThread.h>` |
| `UI/Common/Terminal/TerminalSurface.h`, `TerminalSurfaceNative.h` | `<TerminalViewer/UI/TerminalSurface.h>`, `<TerminalViewer/UI/TerminalSurfaceNative.h>` |
| `tests/FakeFrameSource.h` | `<TerminalViewer/Testing/FakeFrameSource.h>` (`TerminalViewer::TestSupport`) |
| `Interface/IAlgorithm.h` · `IStrategy.h` · `Thread/Channel.h` | 옮기지 않았다. Ocean 것은 그대로 있고, 라이브러리 것은 `TerminalViewer::` 안에 있어 겹치지 않는다 |

- Ocean 에서 지운 것(`CMakeLists.txt` 의 그 줄도): `src/Algorithm/TerminalRenderer/`, `src/Thread/TerminalRenderThread.*`, `TerminalSurface*`(`TerminalSurface` · `TerminalSurfaceNative.h` · `TerminalSurfaceMac.mm`), `resources/shaders/`, `cmake/CheckShaders.cmake`, `cmake/tests/ShaderHashTest.cmake`(+ `OceanShaderHashTest`), `tools/shaders/`, `cmake/OceanDependencies.cmake` 의 GPU 부분, `resources/licenses/LICENSE.MoltenVK-embedded.txt`, `tests/FakeFrameSource.*`, 이 저장소로 옮긴 테스트 16개 클래스(`TerminalSurfaceTest` 포함. `ChannelTest` 는 Ocean 의 `Channel` 을 보므로 Ocean 에도 남는다).
- Ocean 에 남은 것: `TerminalThemeMapper`(`src/UI/Common/Terminal/`, `MakeTerminalTheme` · `ParseTerminalRendererMode`), `Application` 의 연결 코드(`ConnectTerminalRenderThread`: `StrategyChanged` → `Observers::TerminalRendererFellBack`, `ThemeChanged` → `SetTheme`), `tools/TerminalRenderDemo.cpp`, Ocean 의 `IAlgorithm` · `IStrategy` · `Channel`, Cascadia Mono 글꼴과 `BundledFonts`, 그리고 `tests/TerminalRendererBindingTest.cpp`: 옛 `TerminalSurfaceTest` 의 Ocean 의존 3개(`rendererModeOption` · `terminalThemeFollowsPalette` · `applicationForwardsRendererEvents`)를 맡는다.
- Ocean 코드는 Vulkan 코드가 있는지를 이 라이브러리가 PUBLIC 으로 내보내는 `TERMINALVIEWER_GPU` 로 안다(`tools/TerminalRenderDemo.cpp` 의 `#ifdef TERMINALVIEWER_GPU`). Ocean 은 `OCEAN_TERMINAL_GPU` 를 컴파일 정의로 넣지 않는다.
- 받기: `git clone --recurse-submodules`, 이미 받았으면 `git submodule update --init`(Ocean `README.md` 의 "받기"). 서브모듈이 비어 있으면 Ocean 구성이 멈춘다(위 `FATAL_ERROR`). 이 저장소를 고치면 여기서 커밋한 뒤 Ocean 에서 서브모듈을 그 커밋으로 올린다(`git -C external/TerminalViewer pull`, `git add external/TerminalViewer`).

## 7. 순서

| 단위 | 할 일 | 끝난 기준 |
| --- | --- | --- |
| A | 이 저장소를 만들고 라이브러리 · 테스트 · 셰이더 · Vulkan 의존성 · 문서를 옮긴다. Ocean 은 그대로다 | `TerminalViewerTests` 통과, 클래스별 통과 수가 Ocean 과 같다(`TerminalSurfaceTest` 는 3개 적게) — 끝 |
| B | Ocean 에 서브모듈로 붙이고 Ocean 의 사본을 지운다(§6) | 전체 `OceanTests` 통과, 동작 바뀜 없음, 설치 결과가 같다 — 끝 |

- 코드는 [`../CLAUDE.md`](../CLAUDE.md)의 순서를 따른다: 구현 에이전트(테스트 → 옮기기 → 통과) → 리뷰 에이전트(`docs/` 규칙으로 정리 → 통과). 옮기기는 동작을 바꾸지 않는다.

## 8. 코드 규칙

- 이 저장소의 코드는 [docs/CppConvention.md](docs/CppConvention.md) · [docs/Qt.md](docs/Qt.md) · [docs/Patterns.md](docs/Patterns.md) 를 따른다. Ocean `docs/` 의 같은 파일(작업 트리 판)을 복사했다. 한쪽을 고치면 다른 쪽도 고친다.
- Ocean 과 다른 점: `pch.h` 가 없다. `.cpp` 의 맨 앞은 대응 헤더다. 라이브러리 안의 include 는 `"TerminalViewer/…"`, 쓰는 쪽은 `<TerminalViewer/…>` 다.

## 9. 라이선스

- Qt5 는 LGPLv3 로 쓴다: Core · Gui · Widgets 만 쓰고 **반드시 동적 링크**한다(공유 라이브러리가 아니면 구성이 멈춘다). Qt Charts · Qt Data Visualization 등 오픈소스판이 GPLv3 전용인 모듈은 쓰지 않는다.
- GPL · AGPL · 출처 불명 코드와 에셋은 들이지 않는다. LGPL 은 동적 링크로만 쓴다. 새 의존성은 Ocean `docs/LicenseCompliance.md` §2 의 감사 절차를 밟는다.
- 서드파티(Vulkan-Headers · vk-bootstrap · VulkanMemoryAllocator · MoltenVK)와 테스트 · 예제용 Cascadia Mono 의 출처 · 해시 · 고지 의무: [docs/ThirdParty.md](docs/ThirdParty.md).
- 이 저장소 자체의 라이선스: **MIT**([LICENSE](LICENSE)). 서드파티 · 테스트 · 예제용 글꼴은 각자의 라이선스를 따른다(위).
- 원격 저장소: `git@github.com:advanced-eyes/TerminalViewer.git`. Ocean 의 `.gitmodules` 는 상대 URL `../TerminalViewer.git` 으로 이 주소를 가리킨다.

## 10. 출처 · 바꾼 것

- 스펙: Ocean `docs/superpowers/specs/2026-10-08-terminal-renderer-design.md`(상태: 구현 완료, Windows 확인 대기).
- 코드: Ocean `main` 6d539cc + 작업 트리. 작업 트리에서 위젯은 `src/UI/Terminal/` → `src/UI/Common/Terminal/` 로 옮겨져 있었다(이 저장소는 `src/TerminalViewer/UI/`).
- 옮기며 바꾼 것(동작은 같다):
  - include 경로: `"Algorithm/…"` → `"TerminalViewer/Algorithm/…"` 등. `TerminalRenderThread.cpp` 의 `pch.h` 를 뺐다.
  - `IAlgorithm` · `IStrategy` · `Channel` 을 `namespace TerminalViewer` 에 넣었다(§3).
  - 컴파일 정의 `OCEAN_TERMINAL_GPU` → `TERMINALVIEWER_GPU`. 이 이름이 든 문구는 그때는 그대로 두었다(옮긴 뒤 바꿨다, 아래).
  - 셰이더 리소스: `:/shaders/` → `:/terminalviewer/shaders/`, qrc 이름 `shaders.qrc` → `TerminalViewerShaders.qrc`, `Q_INIT_RESOURCE` 추가(§6).
  - CMake: 전역 `CMAKE_*` 대신 타깃 속성. 셰이더 스크립트의 `ocean_` 함수 · 변수 이름을 `terminalviewer_` 로. Vulkan 대상 · FetchContent 이름을 겹치지 않게 바꿨다(버전 · URL · 해시는 그대로).
  - 테스트: `Core/BundledFonts.h` → `TestFonts.h`, `TerminalSurfaceTest` 의 Ocean 의존 3개를 뺐다(나머지 단언은 그대로).
- 옮긴 뒤 바꾼 것(쓰는 쪽이 알아야 한다):
  - 개발용 환경 변수: `OCEAN_VK_DISABLE` → `TERMINALVIEWER_VK_DISABLE`, `OCEAN_VK_VALIDATION` → `TERMINALVIEWER_VK_VALIDATION`(옛 이름은 읽지 않는다). 이 이름이 든 경고 문구도 따라 바뀐다(예: `Vulkan: TERMINALVIEWER_VK_VALIDATION=1 but …`).
  - GPU 를 끈 빌드의 이유 문구(`TerminalRenderer` · `TerminalRenderThread` 의 `GetGpuError`, 그것을 붙인 CPU 대체 경고, Ocean 에서는 `Observers::TerminalRendererFellBack` 의 이유): `GPU 렌더러 없이 만든 빌드다(OCEAN_TERMINAL_GPU=OFF)` → `GPU 렌더러 없이 만든 빌드다(TERMINALVIEWER_GPU=OFF)`.
  - Vulkan 인스턴스의 앱 · 엔진 이름(`VkApplicationInfo`): `Ocean` → `TerminalViewer`. 드라이버 · 검증 레이어에만 보인다.
  - `FakeFrameSource::SetLine(줄, 칸들, 여러 코드포인트 글자들)` 을 더했다: 줄 하나를 통째로 바꾼다(열 수에 맞춰 자르거나 빈칸으로 채우고, `vecGraphemes` 를 바꾸고, 그 줄의 선택을 지운다). ZWJ 이모지 같은 `vecGraphemes` 를 넣는 공개 함수가 없어 예제(§4)를 위해 더했다. 기존 함수는 그대로다.
  - 렌더 스레드의 프레임 흐름(§3 "프레임 흐름"): `Wake` 프레임을 화면의 프레임 간격당 한 번으로 제한하고(깨움 · 스크롤 애니메이션 모두 마감의 위상을 고정해 간격대로 그린다) 그 사이 `Wake` 를 합친다, `TerminalSurface` 는 Windows 에서 모니터의 정확한 주사율을 넘긴다(Qt 는 정수로 버린다), CPU 화면은 GUI 가 받지 않은 조각을 하나만 둔다, GPU 표시 재시도 간격을 1ms 에서 프레임 간격의 1/4 까지 늘린다. `FrameReady` 를 직접 받는 쪽은 프레임이 덜 온다. 공개 API 는 그대로다.
  - CPU 합성 · 조각(§3 "CPU 프레임 비용"): 그린 결과는 같고 빨라졌다. 바뀐 영역을 감싼 사각형이 화면의 절반 이상이면 `FrameReady` 조각이 화면 전체다(복사하지 않고 화면 이미지를 함께 쓴다). 시험용 `SoftwareCompositor::DivideBy255` 를 더했다.
  - `TerminalSurface`: windows QPA 에서는 최상위 창의 Expose 가 드러났는지를 `isExposed` 만으로 본다(`IsTopLevelExposeVisible`). 전에는 빈 영역의 Expose 를 가려짐으로 보아, GPU 화면이 최상위 창을 꽉 채우면 Windows 에서 한 번도 그리지 않았다(아래 Windows GPU 빌드). cocoa · 그 밖의 QPA 는 그대로다.
- Windows 의 GPU 빌드(`TERMINALVIEWER_GPU=ON`): 2026-10-24 Windows 11 · MinGW 8.1 에서 빌드하고 테스트를 통과했고, 한 PC 의 실제 창으로 확인했다(아래). 스펙 부록 C 의 Windows 확인 목록이 남아 기본값은 아직 `OFF` 다(§5). Ocean 은 아직 Windows 에서 GPU 를 켜고 빌드해 보지 않았다.
  - 빌드: vk-bootstrap v1.4.365 의 `VkBootstrap.h` 가 `<cstddef>` 없이 `std::byte` 를 써서 GCC 8.1(libstdc++ 8)에서 컴파일되지 않았다(Ocean 의 플래그 `-std=c++17 -w` 로도 같다). 서드파티 소스는 고치지 않는다. vk-bootstrap 자신의 `VkBootstrap.cpp` 는 `cmake/TerminalViewerDependencies.cmake` 가 GCC 로 컴파일할 때만 `-include cstddef` 를 준다(`TerminalViewerVkBootstrap` 의 PRIVATE 옵션, `$<COMPILE_LANG_AND_ID:CXX,GNU>`. MSVC · Clang(macOS 포함) · C 파일에는 더하지 않는다). 그 헤더를 include 하는 우리 파일은 `Strategy/Vulkan/VulkanContext.h` 하나이고, 그 파일이 `<VkBootstrap.h>` 바로 앞에서 `<cstddef>` 를 include 한다(테스트 · 예제 · Ocean 의 `TerminalRenderDemo` 도 이 헤더를 거친다). 쓰는 쪽 대상의 컴파일 명령에는 아무것도 더하지 않으므로 그 대상의 미리 컴파일한 헤더(PCH, 예: Ocean `OceanCore`)에 영향이 없다(옵션을 PUBLIC 으로 주면 GCC 가 PCH 를 조용히 쓰지 않는 것을 `g++ -H` 로 확인해 이렇게 했다). 빌드 명령은 §4 그대로(`-DTERMINALVIEWER_GPU=ON` 만 더함)다. 그 밖의 컴파일 오류는 없었다(경고: `tests/VulkanViewTest.cpp` 에서 macOS 전용 시험만 쓰는 도움 함수 7개가 `-Wunused-function`).
  - 테스트(`build-gpu`, ctest = offscreen, 이 PC 의 Vulkan 장치): `VulkanRendererTest` 의 `mixedMatchesCpu` · `partialUploadMatchesCpu` 는 컬러 이모지 글꼴이 없으면(offscreen) 실패했었다. 이제 다른 시험과 같은 판정(😀 가 컬러 페이지로 가는가)으로 전제가 없을 때만, 컬러 쿼드 · 컬러 페이지 검사를 빼고 나머지(GPU 와 CPU 비교 등)를 한 뒤 건너뛴다. `QT_QPA_PLATFORM=windows` 로 돌리면 컬러 이모지가 있어 17개 모두 통과한다.
  - Windows 에서 처음 드러나 고친 것(시험과 함께):
    - GPU 화면이 최상위 창을 꽉 채우면 한 번도 그리지 않았다(창은 흰색). windows QPA 는 네이티브 자식 창에 다 덮인 최상위 창의 WM_PAINT 를 빈 영역 · `isExposed` 참인 Expose 로 알리는데, 위젯이 이것을 가려짐(cocoa 의 뜻)으로 보고 화면을 숨겼다. 위 `IsTopLevelExposeVisible`. 시험: `TerminalSurfaceTest::topLevelExposeFollowsQpa`(ctest), `gpuSurfaceFillingWindowDraws` · `gpuSurfaceResizesAndSkipsMinimized`(windows QPA 에서 직접, §4).
    - 예제의 `--grab`(GPU): 창 자체를 잡으면(`QScreen::grabWindow(winId())`, 창 DC 복사) Vulkan 이 표시한 내용이 없어 흰색 한 가지였다(종료 코드 3). 화면에서 창 영역을 잡는다. 시험: `ShowcaseWindowTest::gpuGrabCapturesTheScreen`(windows QPA 에서 직접).
  - 실제 창으로 확인한 것(이 PC: Windows 11, 화면 하나 2560x1440 60Hz 배율 100% 가 NVIDIA GeForce RTX 5080 에 연결, AMD Radeon 내장 GPU 도 있음):
    - 고른 장치: `AMD Radeon(TM) Graphics`(`vulkan-1.dll`). 화면이 연결되지 않은 내장 GPU 다(스펙 부록 C 의 "여러 GPU" 경우, 내장을 먼저 고른다). 그래도 표시는 된다(Windows 가 GPU 사이로 넘긴다). 표시 모드는 FIFO(이 surface 에 MAILBOX 가 없다). 지연 · 성능은 재지 않았다.
    - `TerminalShowcase --grab`: 창 제목 `GPU (Vulkan)`, 컬러 이모지 · 한글 · 상자 문자 · 밑줄 · 선택 · 커서 · IME 조합이 CPU 와 같게 그려진다. `TERMINALVIEWER_VK_DISABLE=1` 은 처음부터 CPU 위젯(창 제목 `CPU (TERMINALVIEWER_VK_DISABLE=1)`). GPU 그림과 이 CPU 그림의 채널 차이는 2/255 이하다(시각이 바뀌는 실시간 줄과 화면에서 잡아 생긴 아래 두 모서리를 빼고).
    - 크기 바꿈(커짐 · 작아짐, 격자 다시 재기), 최소화 동안 그리지 않음 · 되돌리면 다시 그림, `QT_SCALE_FACTOR=1.5`(배율 1.5 로 장치 픽셀 크기 스왑체인 · 글리프): 모두 정상, surface 를 잃거나 CPU 로 바뀌지 않음.
  - 확인하지 못한 것(사람이 해야 한다): MSVC 로 GPU 빌드 · 전체 `OceanTests`, Ocean 의 도킹(QtAds 띄우기 · 다시 붙이기 · 탭 · 분할)과 HWND 재생성(`DetachViewAndWait`), 배율이 다른 화면 사이 이동 · 실행 중 배율 바꿈(화면이 하나), `TerminalRenderDemo` 로 부록 B 기준 1~5 · 7(기준 6 GPU-CPU 차이와 8 Vulkan 없으면 CPU 는 위에서 확인), 화면이 연결된 외장 GPU 를 고를지(부록 C 의 M3), 다른 Windows 기기 · 드라이버.
- macOS 전용 코드(`TerminalSurfaceMac.mm`, Metal 표면, MoltenVK)는 옮긴 뒤 아직 빌드해 보지 않았다. 바꾼 것은 include 경로뿐이다.
