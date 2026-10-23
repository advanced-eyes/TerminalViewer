#include <QApplication>
#include <QTest>

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
    X(DrawListBuilderTest) \
    X(SoftwareCompositorTest) \
    X(TerminalRendererTest) \
    X(ScrollAnimatorTest) \
    X(FrameSchedulerTest) \
    X(TerminalRenderThreadTest) \
    X(VulkanRendererTest) \
    X(VulkanViewTest) \
    X(TerminalRenderThreadGpuTest) \
    X(TerminalSurfaceTest)

#define TERMINALVIEWER_DECLARE_TEST(Name) QObject* Create##Name();
TERMINALVIEWER_TEST_CLASSES(TERMINALVIEWER_DECLARE_TEST)

int main(int argc, char* argv[])
{
    // TerminalSurface 는 위젯이다. ctest 는 QT_QPA_PLATFORM=offscreen 으로 돌린다.
    QApplication app(argc, argv);

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
