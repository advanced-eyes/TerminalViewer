// TerminalShowcase: TerminalViewer 가 그릴 수 있는 것을 한 화면에 보이는 예제(README §4 "예제").
//
// 사용: TerminalShowcase [--renderer auto|cpu] [--font-px N] [--cols C --rows R] [--min-contrast R] [--grab <파일.png>]
// 키: +/- 글꼴 크기, C 커서 모양, M 최소 대비(끔 <-> 4.5), 휠 · PgUp/PgDn · Home/End 스크롤, Esc/Q 끝내기
// --grab: 첫 프레임을 기다렸다가 창을 PNG 로 저장하고 끝난다.
// 종료 코드: 0 = 정상(창을 닫음 · 저장함 · 도움말), 1 = --grab 이 시간 안에 프레임을 못 받음 · 잡기 실패,
//            2 = 잘못된 인자(--grab 과 상관없이), 3 = --grab 이 한 가지 색뿐인 이미지, 4 = --grab 저장 실패
#include "ShowcaseScene.h"
#include "ShowcaseWindow.h"

#include <TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalTheme.h>
#include <TerminalViewer/Testing/FakeFrameSource.h>
#include <TerminalViewer/Thread/TerminalRenderThread.h>
#include <TerminalViewer/UI/TerminalSurface.h>

#include <QApplication>
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFontDatabase>
#include <QImage>
#include <QTimer>

#include <cstdio>
#include <memory>

namespace
{
constexpr int kExitOk = 0;
constexpr int kExitNoFrame = 1;
constexpr int kExitBadArguments = 2;
constexpr int kExitSingleColor = 3;
constexpr int kExitSaveFailed = 4;
constexpr int kDefaultFontPx = 14;
constexpr int kDefaultCols = 100;
constexpr int kDefaultRows = 30;
constexpr int kMinGrid = 1;
constexpr int kMaxGrid = 1000;
constexpr int kFirstFrameTimeoutMs = 15000;
constexpr int kSettleBeforeGrabMs = 500;   // 격자가 정해진 뒤 다시 채운 프레임 · 남은 조각까지 기다린다
constexpr int kPumpStepMs = 5;
constexpr const char* kFontStyles[] = {"Regular", "Bold", "Italic", "BoldItalic"};
constexpr TerminalRgb kSelectionColor{50, 90, 160};
constexpr TerminalRgb kCursorColor{255, 204, 0};
constexpr int kPaddingLogicalPx = 6;

struct Options
{
    ETerminalRendererMode eMode = ETerminalRendererMode::AUTO;
    int iFontPx = kDefaultFontPx;
    int iCols = kDefaultCols;
    int iRows = kDefaultRows;
    double dMinContrast = ShowcaseWindow::kMinContrastOff;
    QString strGrab;
};

// 라이브러리는 글꼴을 갖지 않는다: 테스트용 Cascadia Mono(OFL-1.1, tests/fonts/TestFonts.qrc)를 이 실행 파일에 넣어 등록한다.
// 한글 등 Cascadia 에 없는 글자는 Qt 글꼴 대체(Windows: Malgun Gothic)가 그린다.
void RegisterFonts()
{
    for (const char* pStyle : kFontStyles)
    {
        const QString strPath = QStringLiteral(":/terminalviewer/testfonts/CascadiaMono-%1.ttf").arg(QLatin1String(pStyle));
        if (QFontDatabase::addApplicationFont(strPath) < 0)
            std::fprintf(stderr, "warning: cannot load font %s\n", qPrintable(strPath));
    }
}

bool ParseInt(const QCommandLineParser& parser, const QString& name, int minimum, int maximum, int* pValue)
{
    if (!parser.isSet(name))
        return true;
    bool bOk = false;
    const int iValue = parser.value(name).toInt(&bOk);
    if (!bOk || iValue < minimum || iValue > maximum)
    {
        std::fprintf(stderr, "error: --%s must be %d..%d (got '%s')\n", qPrintable(name), minimum, maximum, qPrintable(parser.value(name)));
        return false;
    }
    *pValue = iValue;
    return true;
}

// 실수 값(소수점은 '.'). 범위 밖 · 숫자 아님 · nan · inf 는 거짓
bool ParseDouble(const QCommandLineParser& parser, const QString& name, double minimum, double maximum, double* pValue)
{
    if (!parser.isSet(name))
        return true;
    bool bOk = false;
    const double dValue = parser.value(name).toDouble(&bOk);
    if (!bOk || !(dValue >= minimum && dValue <= maximum))
    {
        std::fprintf(stderr, "error: --%s must be %g..%g (got '%s')\n", qPrintable(name), minimum, maximum, qPrintable(parser.value(name)));
        return false;
    }
    *pValue = dValue;
    return true;
}

bool ParseOptions(const QApplication& app, Options* pOptions)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral(
        "TerminalViewer showcase: colors, styles, underlines, wide text, emoji, box drawing, selection, cursor, IME preedit, "
        "scrollback and a live line.\n"
        "Keys: +/- font size, C cursor shape, M minimum contrast (off <-> 4.5), wheel / PgUp / PgDn / Home / End scroll, Esc or Q quit.\n"
        "Exit codes: 0 ok, 1 no frame in time (--grab), 2 bad arguments, 3 single-color image (--grab), 4 cannot save (--grab)."));
    parser.addHelpOption();
    parser.addOption({QStringLiteral("renderer"), QStringLiteral("auto (GPU if possible) or cpu."), QStringLiteral("auto|cpu"),
                      QStringLiteral("auto")});
    parser.addOption({QStringLiteral("font-px"),
                      QStringLiteral("Cascadia Mono pixel size (%1..%2).").arg(ShowcaseWindow::kMinFontPx).arg(ShowcaseWindow::kMaxFontPx),
                      QStringLiteral("N"), QString::number(kDefaultFontPx)});
    parser.addOption({QStringLiteral("cols"), QStringLiteral("Initial window width in cells."), QStringLiteral("C"), QString::number(kDefaultCols)});
    parser.addOption({QStringLiteral("rows"), QStringLiteral("Initial window height in cells."), QStringLiteral("R"), QString::number(kDefaultRows)});
    parser.addOption({QStringLiteral("min-contrast"),
                      QStringLiteral("Minimum contrast ratio of text against its background (WCAG 2.x, %1..%2, %1 = off).")
                          .arg(ShowcaseWindow::kMinContrastOff)
                          .arg(ShowcaseWindow::kMaxMinContrast),
                      QStringLiteral("R"), QString::number(ShowcaseWindow::kMinContrastOff)});
    parser.addOption({QStringLiteral("grab"), QStringLiteral("Wait for the first frame, save the window as PNG and exit."),
                      QStringLiteral("file.png")});
    // process() 는 파싱 오류(모르는 옵션, 값이 빠진 옵션)에 1 로 끝낸다: --grab 의 "시간 안에 프레임 없음"과 같은 값이라
    // 직접 parse 해서 다른 잘못된 인자처럼 2 로 끝낸다.
    if (!parser.parse(QCoreApplication::arguments()))
    {
        std::fprintf(stderr, "error: %s (see --help)\n", qPrintable(parser.errorText()));
        return false;
    }
    // 파싱은 이미 성공했다: --help(-h, -?) · --help-all 은 여기서 도움말을 찍고 0 으로 끝난다(--help-all 의 Qt 옵션 목록은 process 만 찍는다)
    parser.process(app);
    if (!parser.positionalArguments().isEmpty())
    {
        std::fprintf(stderr, "error: unexpected argument '%s' (see --help)\n", qPrintable(parser.positionalArguments().front()));
        return false;
    }
    const QString strRenderer = parser.value(QStringLiteral("renderer"));
    if (strRenderer == QLatin1String("auto"))
        pOptions->eMode = ETerminalRendererMode::AUTO;
    else if (strRenderer == QLatin1String("cpu"))
        pOptions->eMode = ETerminalRendererMode::CPU;
    else
    {
        std::fprintf(stderr, "error: --renderer must be auto or cpu (got '%s')\n", qPrintable(strRenderer));
        return false;
    }
    if (!ParseInt(parser, QStringLiteral("font-px"), ShowcaseWindow::kMinFontPx, ShowcaseWindow::kMaxFontPx, &pOptions->iFontPx) ||
        !ParseInt(parser, QStringLiteral("cols"), kMinGrid, kMaxGrid, &pOptions->iCols) ||
        !ParseInt(parser, QStringLiteral("rows"), kMinGrid, kMaxGrid, &pOptions->iRows) ||
        !ParseDouble(parser, QStringLiteral("min-contrast"), ShowcaseWindow::kMinContrastOff, ShowcaseWindow::kMaxMinContrast,
                     &pOptions->dMinContrast))
        return false;
    pOptions->strGrab = parser.value(QStringLiteral("grab"));
    if (parser.isSet(QStringLiteral("grab")) && pOptions->strGrab.isEmpty())
    {
        std::fprintf(stderr, "error: --grab needs a file name\n");
        return false;
    }
    return true;
}

void PumpFor(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

template <typename Pred>
bool WaitUntil(Pred pred, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!pred())
    {
        if (timer.elapsed() > timeoutMs)
            return false;
        PumpFor(kPumpStepMs);
    }
    return true;
}

bool IsSingleColor(const QImage& image)
{
    const QImage imgArgb = image.convertToFormat(QImage::Format_ARGB32);
    const QRgb first = imgArgb.pixel(0, 0);
    for (int iY = 0; iY < imgArgb.height(); ++iY)
    {
        const QRgb* pRow = reinterpret_cast<const QRgb*>(imgArgb.constScanLine(iY));
        for (int iX = 0; iX < imgArgb.width(); ++iX)
        {
            if (pRow[iX] != first)
                return false;
        }
    }
    return true;
}

int GrabToFile(ShowcaseWindow& window, const QString& path)
{
    if (!WaitUntil([&window]() { return window.GetFrameCount() > 0; }, kFirstFrameTimeoutMs))
    {
        std::fprintf(stderr, "error: no frame was drawn within %d ms\n", kFirstFrameTimeoutMs);
        return kExitNoFrame;
    }
    PumpFor(kSettleBeforeGrabMs);
    const QImage imgGrab = window.GrabFrame();
    if (imgGrab.isNull() || imgGrab.width() == 0 || imgGrab.height() == 0)
    {
        std::fprintf(stderr, "error: cannot grab the window\n");
        return kExitNoFrame;
    }
    if (IsSingleColor(imgGrab))
    {
        std::fprintf(stderr, "error: the grabbed image (%dx%d) has a single color\n", imgGrab.width(), imgGrab.height());
        return kExitSingleColor;
    }
    if (!imgGrab.save(path, "PNG"))
    {
        std::fprintf(stderr, "error: cannot save %s\n", qPrintable(path));
        return kExitSaveFailed;
    }
    const bool bGpu = window.GetSurface() && window.GetSurface()->IsGpu();
    std::printf("grab: %s (%dx%d, %s)\n", qPrintable(path), imgGrab.width(), imgGrab.height(), bGpu ? "GPU, screen capture" : "CPU, widget grab");
    std::fflush(stdout);
    return kExitOk;
}
}   // namespace

int main(int argc, char* argv[])
{
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
    // GPU 화면(네이티브 자식 창)의 형제 위젯까지 네이티브 창이 되지 않게 한다
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
    if (qEnvironmentVariableIsEmpty("MVK_CONFIG_LOG_LEVEL"))
        qputenv("MVK_CONFIG_LOG_LEVEL", "1");   // MoltenVK 정보 로그를 끈다(오류만)
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("TerminalShowcase"));

    Options options;
    if (!ParseOptions(app, &options))
        return kExitBadArguments;

    RegisterFonts();
    QFont font(QStringLiteral("Cascadia Mono"));
    font.setPixelSize(options.iFontPx);
    TerminalTheme theme;
    theme.selectionBg = kSelectionColor;
    theme.cursorColor = kCursorColor;
    theme.iPaddingLogicalPx = kPaddingLogicalPx;
    theme.dMinContrastRatio = options.dMinContrast;

    // 공급원은 렌더 스레드가 SourceReleased 를 낸 뒤에 지운다(TerminalRenderThread.h 의 공급원 수명)
    auto upSource = std::make_unique<FakeFrameSource>(options.iCols, options.iRows, ShowcaseScene::kScrollbackLines);
    auto upThread = std::make_unique<TerminalRenderThread>(options.eMode);
    bool bSourceReleased = false;
    QObject::connect(upThread.get(), &TerminalRenderThread::SourceReleased, &app,
                     [&bSourceReleased, pSource = upSource.get()](ITerminalFrameSource* pReleased)
                     {
                         if (pReleased == pSource)
                             bSourceReleased = true;
                     });
    upThread->SetFont(font);
    upThread->SetTheme(theme);
    upThread->SetStatsEnabled(!options.strGrab.isEmpty());   // GPU 화면은 FrameReady 가 없어 첫 프레임을 FrameStats 로 안다
    upThread->Start();

    auto upWindow = std::make_unique<ShowcaseWindow>(upThread.get(), upSource.get(), options.eMode, QSize(options.iCols, options.iRows));
    upWindow->show();
    if (QGuiApplication::platformName() != QLatin1String("offscreen"))
    {
        upWindow->raise();   // offscreen 은 raise 를 지원하지 않는다고 경고한다
        upWindow->activateWindow();
    }

    const int iExit = options.strGrab.isEmpty() ? app.exec() : GrabToFile(*upWindow, options.strGrab);

    // 끄는 순서: 위젯(창) -> 렌더 스레드 -> 공급원
    upWindow.reset();
    upThread->Stop();
    if (!bSourceReleased)
        std::fprintf(stderr, "warning: the render thread did not release the frame source\n");
    upThread.reset();
    upSource.reset();
    return iExit;
}
