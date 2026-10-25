#include "ShowcaseWindow.h"

#include <TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h>
#include <TerminalViewer/Algorithm/TerminalRenderer/TerminalPlatform.h>
#include <TerminalViewer/Testing/FakeFrameSource.h>
#include <TerminalViewer/Thread/TerminalRenderThread.h>
#include <TerminalViewer/UI/TerminalSurface.h>

#include <QGuiApplication>
#include <QKeyEvent>
#include <QPixmap>
#include <QScreen>
#include <QTime>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace
{
constexpr int kLiveTickMs = 100;             // 실시간 줄(스피너 · 진행 막대 · 시각)
constexpr int kTicksPerImeStep = 5;          // IME 흉내 한 단계 = 0.5초
constexpr int kCursorTickMs = 16;            // 스크롤 뒤 커서 줄 맞추기
constexpr int kFontStepPx = 1;               // +/- 키 한 번
constexpr int kPageOverlapRows = 1;          // PgUp/PgDn 은 한 줄을 겹쳐 남긴다
constexpr double kWheelStepDegrees = 15.0;   // 휠 한 칸(angleDelta 120 = 15도)
constexpr double kRowsPerWheelStep = 3.0;
constexpr int kAngleUnitsPerDegree = 8;
constexpr int kContrastTitleDigits = 4;      // 창 제목의 최소 대비(예: 4.5, 7, 10.25)
#ifdef TERMINALVIEWER_GPU
constexpr const char* kVulkanDisableVariable = "TERMINALVIEWER_VK_DISABLE";   // 1 이면 라이브러리가 Vulkan 을 쓰지 않는다
#endif

const char* GetShapeName(ECursorShape shape)
{
    switch (shape)
    {
    case ECursorShape::BAR:
        return "bar";
    case ECursorShape::BLOCK:
        return "block";
    case ECursorShape::UNDERLINE:
        return "underline";
    case ECursorShape::BLOCK_HOLLOW:
        return "hollow";
    }
    return "?";
}
}   // namespace

ShowcaseWindow::ShowcaseWindow(TerminalRenderThread* pRenderThread, FakeFrameSource* pSource, ETerminalRendererMode mode,
                               const QSize& initialGrid, QWidget* parent)
    : QWidget(parent)
    , m_pRenderThread(pRenderThread)
    , m_pSource(pSource)
    , m_eMode(mode)
    , m_scene()
    , m_pLayout(new QVBoxLayout(this))
    , m_pSurface(nullptr)
    , m_timerLive()
    , m_timerCursor()
    , m_uTick(0)
    , m_iFrames(0)
    , m_bFellBack(false)
{
    m_pLayout->setContentsMargins(0, 0, 0, 0);
    m_pLayout->setSpacing(0);
    // 첫 프레임 전에 흰 창이 보이지 않게 터미널 배경색으로 칠한다
    QPalette palette = this->palette();
    palette.setColor(QPalette::Window, QColor(ShowcaseScene::kBackground.r, ShowcaseScene::kBackground.g, ShowcaseScene::kBackground.b));
    setPalette(palette);
    setAutoFillBackground(true);

    // 처음 창 크기: 그 열 x 행 이 꼭 들어가는 크기(위젯이 붙으면 GridSizeChanged 가 실제 격자를 알린다)
    const qreal dRatio = devicePixelRatioF();
    const CellMetrics metrics = CellMetrics::Compute(m_pRenderThread->GetFont(), dRatio);
    const int iPaddingDevice = qRound(m_pRenderThread->GetTheme().iPaddingLogicalPx * dRatio);
    const int iCols = std::max(1, initialGrid.width());
    const int iRows = std::max(1, initialGrid.height());
    resize(int(std::ceil((iCols * metrics.iCellWidth + 2 * iPaddingDevice) / dRatio)),
           int(std::ceil((iRows * metrics.iCellHeight + 2 * iPaddingDevice) / dRatio)));
    m_scene.Fill(*m_pSource, iCols, iRows);

    connect(m_pRenderThread, &TerminalRenderThread::FrameReady, this, [this](int viewId, const QImage&, const QRect&) { OnFrame(viewId); });
    connect(m_pRenderThread, &TerminalRenderThread::FrameStats, this, [this](const TerminalFrameStats& stats) { OnFrame(stats.iViewId); });
    connect(m_pRenderThread, &TerminalRenderThread::StrategyChanged, this, [this]() { UpdateTitle(); });

    CreateSurface(m_pRenderThread->GetActiveStrategy() == ETerminalRenderStrategy::GPU);

    connect(&m_timerLive, &QTimer::timeout, this, &ShowcaseWindow::OnLiveTick);
    m_timerLive.start(kLiveTickMs);
    connect(&m_timerCursor, &QTimer::timeout, this, &ShowcaseWindow::OnCursorTick);
    m_timerCursor.start(kCursorTickMs);
    UpdateTitle();
}

ShowcaseWindow::~ShowcaseWindow()
{
    m_timerLive.stop();
    m_timerCursor.stop();
    // 위젯(자식)은 QWidget 소멸자가 지운다: TerminalSurface 가 화면을 떼고 공급원을 놓는다
}

QImage ShowcaseWindow::GrabFrame()
{
    if (!m_pSurface)
        return QImage();
    if (!m_pSurface->IsGpu())
        return grab().toImage();   // CPU 위젯은 보관한 이미지를 paintEvent 로 그린다
    // GPU 화면은 렌더 스레드가 네이티브 자식 창에 바로 표시한다. 창 자체를 잡으면(grabWindow(winId())) 그 내용이 없다
    // (Windows: 창 DC 를 복사해 흰색 한 가지). 화면에서 이 창의 영역을 잡는다(그 위를 다른 창이 덮고 있으면 그것도 찍힌다).
    QScreen* pScreen = windowHandle() ? windowHandle()->screen() : QGuiApplication::primaryScreen();
    if (!pScreen)
        return QImage();
    const QPoint ptWindow = mapToGlobal(QPoint(0, 0)) - pScreen->geometry().topLeft();   // 화면 기준 논리 좌표
    return pScreen->grabWindow(0, ptWindow.x(), ptWindow.y(), width(), height()).toImage();
}

int ShowcaseWindow::GetFrameCount() const
{
    return m_iFrames;
}

TerminalSurface* ShowcaseWindow::GetSurface() const
{
    return m_pSurface;
}

const ShowcaseScene& ShowcaseWindow::GetScene() const
{
    return m_scene;
}

void ShowcaseWindow::keyPressEvent(QKeyEvent* event)
{
    switch (event->key())
    {
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        ChangeFontSize(kFontStepPx);
        break;
    case Qt::Key_Minus:
    case Qt::Key_Underscore:
        ChangeFontSize(-kFontStepPx);
        break;
    case Qt::Key_C:
        m_scene.CycleCursorShape(*m_pSource);
        Wake();
        UpdateTitle();
        break;
    case Qt::Key_M:
        ToggleMinContrast();
        break;
    case Qt::Key_PageUp:
        ScrollByRows(-(m_scene.GetRows() - kPageOverlapRows));
        break;
    case Qt::Key_PageDown:
        ScrollByRows(m_scene.GetRows() - kPageOverlapRows);
        break;
    case Qt::Key_Home:
        ScrollByRows(-(ShowcaseScene::kScrollbackLines + m_scene.GetRows()));
        break;
    case Qt::Key_End:
        ScrollByRows(ShowcaseScene::kScrollbackLines + m_scene.GetRows());
        break;
    case Qt::Key_Escape:
    case Qt::Key_Q:
        close();
        break;
    default:
        QWidget::keyPressEvent(event);
        return;
    }
    event->accept();
}

void ShowcaseWindow::wheelEvent(QWheelEvent* event)
{
    // TerminalSurface 는 휠을 받지 않으므로 부모인 이 창으로 온다. 위로 굴리면(+) 지난 출력 쪽(-)
    if (!event->pixelDelta().isNull())
        ScrollByPixels(-event->pixelDelta().y() * devicePixelRatioF());   // 터치패드(논리 픽셀)
    else
        ScrollByRows(-event->angleDelta().y() / (kWheelStepDegrees * kAngleUnitsPerDegree) * kRowsPerWheelStep);
    event->accept();
}

void ShowcaseWindow::CreateSurface(bool useGpu)
{
    TerminalSurface* pOld = m_pSurface;
    TerminalSurface* pSurface = new TerminalSurface(m_pRenderThread, m_pSource, useGpu, this);
    connect(pSurface, &TerminalSurface::GridSizeChanged, this, [this, pSurface](const QSize& colsRows) { OnGridSizeChanged(pSurface, colsRows); });
    connect(pSurface, &TerminalSurface::CpuFallbackRequested, this, [this, pSurface]() { OnCpuFallbackRequested(pSurface); });
    m_pSurface = pSurface;
    m_iFrames = 0;
    if (pOld)
    {
        m_pLayout->replaceWidget(pOld, pSurface);
        pOld->hide();
    }
    else
    {
        m_pLayout->addWidget(pSurface);
    }
    pSurface->show();
    setFocusProxy(pSurface);   // 창이 활성화되면 위젯이 초점을 받는다(키는 위젯이 받지 않아 이 창으로 올라온다)
    pSurface->setFocus();
}

void ShowcaseWindow::OnGridSizeChanged(TerminalSurface* pSurface, const QSize& colsRows)
{
    if (pSurface != m_pSurface)
        return;   // 바꿔 끼운 옛 위젯
    m_pSource->Resize(colsRows.width(), colsRows.height());
    m_scene.Fill(*m_pSource, colsRows.width(), colsRows.height());
    Wake();
    UpdateTitle();
}

void ShowcaseWindow::OnCpuFallbackRequested(TerminalSurface* pSurface)
{
    if (pSurface != m_pSurface)
        return;
    // 계약(TerminalSurface.h): 같은 공급원으로 새 CPU 위젯을 먼저 만든 뒤 옛 위젯을 deleteLater 한다
    m_bFellBack = true;
    CreateSurface(false);
    pSurface->deleteLater();
    UpdateTitle();
}

void ShowcaseWindow::OnFrame(int viewId)
{
    if (m_pSurface && viewId != 0 && viewId == m_pSurface->GetViewId())
        ++m_iFrames;
}

void ShowcaseWindow::OnLiveTick()
{
    ++m_uTick;
    const std::u32string strClock = QTime::currentTime().toString(QStringLiteral("HH:mm:ss.zzz")).toStdU32String();
    m_scene.UpdateLive(*m_pSource, m_uTick, strClock);
    m_scene.SetImeStep(*m_pSource, ShowcaseScene::kInitialImeStep + int(m_uTick / kTicksPerImeStep));
    Wake();
}

void ShowcaseWindow::OnCursorTick()
{
    // 렌더 스레드가 스크롤하며 뷰포트를 옮긴다: 프롬프트 줄이 화면 밖으로 나가면 커서 · 조합을 숨긴다
    if (m_scene.SyncCursor(*m_pSource))
        Wake();
}

void ShowcaseWindow::ChangeFontSize(int delta)
{
    QFont font = m_pRenderThread->GetFont();
    const int iPx = std::clamp(font.pixelSize() + delta, kMinFontPx, kMaxFontPx);
    if (iPx == font.pixelSize())
        return;
    font.setPixelSize(iPx);
    // CellLayoutChanged -> 위젯이 격자를 다시 잰다 -> GridSizeChanged -> 공급원 Resize + 다시 채우기 + Wake
    m_pRenderThread->SetFont(font);
    UpdateTitle();
}

void ShowcaseWindow::ToggleMinContrast()
{
    // 켜져 있으면(1 보다 크면) 끄고, 꺼져 있으면 kToggledMinContrast. 렌더 스레드는 테마가 바뀌면 행 캐시를 버리고 전체를 다시 그린다
    TerminalTheme theme = m_pRenderThread->GetTheme();
    theme.dMinContrastRatio = theme.dMinContrastRatio > kMinContrastOff ? kMinContrastOff : kToggledMinContrast;
    m_pRenderThread->SetTheme(theme);
    UpdateTitle();
}

void ShowcaseWindow::ScrollByPixels(double pixels)
{
    if (m_pSurface && m_pSurface->GetViewId() != 0 && pixels != 0.0)
        m_pRenderThread->ScrollPixels(m_pSurface->GetViewId(), pixels);
}

void ShowcaseWindow::ScrollByRows(double rows)
{
    const CellMetrics metrics = CellMetrics::Compute(m_pRenderThread->GetFont(), devicePixelRatioF());
    ScrollByPixels(rows * metrics.iCellHeight);   // 장치 픽셀
}

void ShowcaseWindow::Wake()
{
    if (m_pSurface && m_pSurface->GetViewId() != 0)
        m_pRenderThread->Wake(m_pSurface->GetViewId());
}

void ShowcaseWindow::UpdateTitle()
{
    const bool bGpu = m_pSurface && m_pSurface->IsGpu();
    const QString strReason = bGpu ? QString() : DescribeCpuReason();
    const double dMinContrast = m_pRenderThread->GetTheme().dMinContrastRatio;
    const QString strContrast =
        dMinContrast > kMinContrastOff ? QString::number(dMinContrast, 'g', kContrastTitleDigits) : QStringLiteral("off");
    setWindowTitle(QStringLiteral("TerminalShowcase - %1%2 - %3x%4 - %5 px - cursor %6 - contrast %7")
                       .arg(bGpu ? QStringLiteral("GPU (Vulkan)") : QStringLiteral("CPU"),
                            strReason.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(strReason))
                       .arg(m_scene.GetCols())
                       .arg(m_scene.GetRows())
                       .arg(m_pRenderThread->GetFont().pixelSize())
                       .arg(QLatin1String(GetShapeName(m_scene.GetCursor().eShape)))
                       .arg(strContrast));
}

QString ShowcaseWindow::DescribeCpuReason() const
{
    const QString strError = m_pRenderThread->GetGpuError();
    if (!strError.isEmpty())
        return QStringLiteral("GPU unavailable: %1").arg(strError);
    if (m_eMode == ETerminalRendererMode::CPU)
        return QStringLiteral("--renderer cpu");
#ifndef TERMINALVIEWER_GPU
    return QStringLiteral("built with TERMINALVIEWER_GPU=OFF");
#else
    // 개발용 스위치로 Vulkan 을 껐다(라이브러리와 같은 판정, README §5): 라이브러리가 없다고 하지 않는다
    if (qEnvironmentVariableIntValue(kVulkanDisableVariable) == 1)
        return QStringLiteral("%1=1").arg(QLatin1String(kVulkanDisableVariable));
    if (!IsNativeSurfaceSupported())
        return QStringLiteral("QPA '%1' has no native GPU surface").arg(QGuiApplication::platformName());
    if (!IsGpuLibraryAvailable())
        return QStringLiteral("Vulkan library not found");
    return m_bFellBack ? QStringLiteral("fell back to CPU") : QString();
#endif
}
