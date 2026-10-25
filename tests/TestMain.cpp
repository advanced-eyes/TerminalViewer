#include <QApplication>
#include <QTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include <cstdio>
#include <memory>
#include <vector>

constexpr int kClassNameArgument = 1;      // argv[1]: 돌릴 클래스 이름
constexpr int kFirstQTestArgument = 2;     // argv[2] 부터는 QTest 에 넘긴다
constexpr int kUnknownClassExitCode = 1;   // 첫 인자와 맞는 클래스가 없을 때(QTest 가 모르는 함수 이름에 내는 값과 같다)

// 테스트 클래스를 한 실행 파일에서 차례로 돌린다. 첫 인자가 클래스 이름이면 그 클래스만 돈다.
// 맞는 클래스가 없으면 아는 이름을 알리고 실패로 끝난다. 0 으로 끝나면 아무것도 안 돌았는데 통과로 보인다.
// GPU 테스트(VulkanRendererTest, VulkanViewTest, TerminalRenderThreadGpuTest)는 늘 등록한다:
// TERMINALVIEWER_GPU 를 끈 빌드에서는 각 파일이 GPU 없는 판(건너뜀 또는 CPU 로 남는지 확인)으로 컴파일된다.
#define TERMINALVIEWER_TEST_CLASSES(X) \
    X(CellMetricsTest) \
    X(ChannelTest) \
    X(FakeFrameSourceTest) \
    X(GlyphAtlasTest) \
    X(BoxGlyphPainterTest) \
    X(GlyphRasterizerTest) \
    X(GlyphCacheTest) \
    X(TerminalContrastTest) \
    X(DrawListBuilderTest) \
    X(SoftwareCompositorTest) \
    X(TerminalRendererTest) \
    X(ScrollAnimatorTest) \
    X(FrameSchedulerTest) \
    X(TerminalRenderThreadTest) \
    X(VulkanRendererTest) \
    X(VulkanViewTest) \
    X(TerminalRenderThreadGpuTest) \
    X(TerminalSurfaceTest) \
    X(ShowcaseSceneTest) \
    X(ShowcaseWindowTest)

#define TERMINALVIEWER_DECLARE_TEST(Name) QObject* Create##Name();
TERMINALVIEWER_TEST_CLASSES(TERMINALVIEWER_DECLARE_TEST)

// Windows 11 은 보이는 창이 없는 프로세스의 timeBeginPeriod(1)을 약 3초 뒤부터 무시한다(전원 제한. 이 PC 에서 Sleep(1) 이
// 1.9ms -> 15.7ms). 렌더 스레드는 애니메이션 동안 1ms 타이머로 프레임 시각을 맞추므로, 실행 몇 초 뒤에 도는 프레임 간격 시험
// (TerminalRenderThreadTest::refreshRatePacesScrollFrames)이 60Hz · 120Hz 모두 프레임을 반만 그려 실패했다. 창 없이 도는
// 시험 프로세스만 그 제한을 끈다(PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 을 끔). 실제 앱은 창이 보일 때 그린다.
// MinGW 8.1 헤더에 구조체 · 값이 없어 직접 적고, Windows 8 이전에는 없는 함수라 이름으로 찾는다.
void KeepFineTimerResolution()
{
#ifdef Q_OS_WIN
    struct PowerThrottlingState
    {
        ULONG ulVersion;
        ULONG ulControlMask;
        ULONG ulStateMask;
    };
    constexpr ULONG kPowerThrottlingVersion = 1;     // PROCESS_POWER_THROTTLING_CURRENT_VERSION
    constexpr ULONG kIgnoreTimerResolution = 0x4;    // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    constexpr int kProcessPowerThrottling = 4;       // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling
    using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    const auto fpSetProcessInformation = reinterpret_cast<SetProcessInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation")));
    if (!fpSetProcessInformation)
        return;
    PowerThrottlingState state{kPowerThrottlingVersion, kIgnoreTimerResolution, 0};   // 제한을 끈다(StateMask 0)
    fpSetProcessInformation(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof(state));
#endif
}

int main(int argc, char* argv[])
{
    // TerminalSurface 는 위젯이다. ctest 는 QT_QPA_PLATFORM=offscreen 으로 돌린다.
    QApplication app(argc, argv);
    KeepFineTimerResolution();

    const QString strOnly = argc > kClassNameArgument ? QString::fromLocal8Bit(argv[kClassNameArgument]) : QString();
    // 클래스 이름 뒤의 인자는 QTest 에 넘긴다(예: -o out.txt,txt — MinGW Qt 는 콘솔에 결과를 쓰지 않아 파일로 받는다).
    std::vector<char*> vecArgs { argv[0] };
    for (int iArg = kFirstQTestArgument; iArg < argc; ++iArg)
    {
        vecArgs.push_back(argv[iArg]);
    }
    int iFailures = 0;
    int iRunClasses = 0;
#define TERMINALVIEWER_RUN_TEST(Name) \
    if (strOnly.isEmpty() || strOnly == QStringLiteral(#Name)) \
    { \
        std::unique_ptr<QObject> upTest(Create##Name()); \
        iFailures += QTest::qExec(upTest.get(), static_cast<int>(vecArgs.size()), vecArgs.data()); \
        ++iRunClasses; \
    }
    TERMINALVIEWER_TEST_CLASSES(TERMINALVIEWER_RUN_TEST)
    if (!strOnly.isEmpty() && iRunClasses == 0)
    {
        // QTest 처럼 표준 오류에 바로 쓴다. Qt 메시지 처리기를 거치면 콘솔이 없을 때 보이지 않는다.
        std::fprintf(stderr,
                     "Unknown test class: '%s'. The first argument is a test class name; QTest options go after it.\n"
                     "Test classes:\n",
                     argv[kClassNameArgument]);
#define TERMINALVIEWER_PRINT_TEST(Name) std::fprintf(stderr, "  %s\n", #Name);
        TERMINALVIEWER_TEST_CLASSES(TERMINALVIEWER_PRINT_TEST)
        return kUnknownClassExitCode;
    }
    return iFailures;
}
