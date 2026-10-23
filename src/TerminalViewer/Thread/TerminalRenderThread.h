#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalFrame.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalTheme.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalRenderer.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalRendererMode.h"

#include <QFont>
#include <QImage>
#include <QMetaType>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QtGlobal>

#include <memory>

struct TerminalFrameStats        // 측정용 (Task 11). 시각은 steady_clock 단조 시각(ns)
{
    int iViewId = 0;
    // llWakeNs: 이 프레임에 합쳐진 첫 Wake 를 부른 시각. Wake 없이 그린 프레임(깜빡임·애니메이션·설정)은 0
    // llPresentCallNs: Strategy 의 Execute(CPU 합성, GPU 제출·표시)가 끝난 시각
    // llExecuteStartNs / llExecuteEndNs: Strategy 의 Execute 를 부르기 직전 / 돌아온 직후. Execute 안(GPU: 제출·vkQueuePresentKHR,
    //   MoltenVK 는 여기서 다음 drawable 을 기다리며 막힐 수 있다)은 End − Start 다. Start − CaptureDone 이 막힘 없는 CPU 시간
    //   (그리기 목록 + 기록 준비)이다. llPresentCallNs 는 llExecuteEndNs 와 같은 값이다(옛 이름).
    // iNotReadyRetries: Execute 가 bNotReady 라 같은 목록으로 다시 부른 횟수. 0 보다 크면 CaptureDone → ExecuteStart 에 기다린 시간이
    //   들어 있으니(재시도 간격) 프레임 CPU 시간으로 쓰지 않는다.
    // iPresentMode: 표시한 VkPresentModeKHR 값. CPU 는 -1
    qint64 llWakeNs = 0, llCaptureDoneNs = 0, llBuildDoneNs = 0, llPresentCallNs = 0;
    qint64 llExecuteStartNs = 0, llExecuteEndNs = 0;
    int iQuadCount = 0, iRebuiltRows = 0;
    int iNotReadyRetries = 0;
    int iPresentMode = -1;
};

Q_DECLARE_METATYPE(TerminalFrameStats)
Q_DECLARE_METATYPE(TerminalTheme)
Q_DECLARE_METATYPE(ETerminalRenderStrategy)
Q_DECLARE_METATYPE(ITerminalFrameSource*)

struct TerminalRenderShared;   // 렌더 스레드와 함께 쓰는 채널·상태 (.cpp)

// 렌더 스레드에서 도는 채널 루프. DoWork 는 Stop 될 때까지 돌아오지 않는다(processEvents 를 쓰지 않는다).
class TerminalRenderThreadWorker : public QObject
{
    Q_OBJECT

public:
    explicit TerminalRenderThreadWorker(QObject* parent = nullptr);
    ~TerminalRenderThreadWorker() override;

    void SetContext(TerminalRenderShared* pShared, ETerminalRendererMode mode, const QFont& font, const TerminalTheme& theme);

    void Stop();

    bool IsThreadAlive() const;

public slots:
    void DoWork();

signals:
    void FrameReady(int viewId, const QImage& imgPatch, const QRect& rectPatch);
    void ViewNeedsCpuFallback(int viewId);
    void ViewSurfaceLost(int viewId);
    void StrategyChanged(ETerminalRenderStrategy strategy);
    void FrameStats(const TerminalFrameStats& stats);
    void ViewDetached(int viewId);
    void WorkFinished(bool success);

private:
    class TerminalRenderThreadWorkerImpl;
    std::unique_ptr<TerminalRenderThreadWorkerImpl> m_pimpl;
};

// 모든 터미널 화면이 함께 쓰는 렌더 스레드 하나(스펙 §5). 명령은 채널로만 보낸다.
// docs/Qt.md QThread 패턴 (Worker + moveToThread + PIMPL)
class TerminalRenderThread : public QObject
{
    Q_OBJECT

public:
    explicit TerminalRenderThread(ETerminalRendererMode mode, QObject* parent = nullptr);
    ~TerminalRenderThread() override;   // Stop

    void Start();
    // 지금 그리는 화면 하나를 마치고 끝난다(4초가 지나면 스레드를 강제로 끝낸다: CaptureFrame 이 엔진 잠금에 오래 막히면 안 된다).
    // 돌아오기 전에 잡혀 있지 않은(RetainSource) 공급원마다 SourceReleased 를 낸다.
    // 화면 id 는 Stop → Start 뒤에 쓸 수 없다(위젯을 모두 지운 뒤 멈춘다, 스펙 부록 C)
    void Stop();
    // 테스트·도구용: GPU Strategy 를 만드는 함수를 바꾼다(기본 = TerminalRendererGPU::Create). Start 전에 부른다
    void SetGpuStrategyFactory(TerminalGpuStrategyFactory factory);

    // 아래는 아무 스레드에서나 부른다(채널에 넣기만). 화면 id 는 AttachView 가 바로 돌려준다
    // 공급원 수명(GPU·CPU 같은 규칙): 주인은 pSource 를 SourceReleased(pSource) 가 올 때까지 살려 둔다. 그 뒤에는 렌더 스레드도
    //   그 공급원을 잡은 쪽(TerminalSurface)도 더는 쓰지 않는다. 위젯 없이 AttachView 만 쓰는 쪽(테스트·도구)은 Stop() 이
    //   돌아온 뒤에도 놓아도 된다. 화면 하나로 보면 ViewDetached(id) 가 왔거나 DetachViewAndWait 가 참을 돌려준 뒤에는
    //   그 id 가 공급원을 쓰지 않는다(같은 공급원을 다른 id·잡은 쪽이 아직 쓸 수 있다). DetachViewAndWait 가 시간을 넘겨
    //   거짓이면 렌더 스레드가 아직 쓴다.
    // handle: 네이티브 핸들을 가진 화면은 모드가 AUTO 면 GPU 로 그린다(처음 그런 화면이 붙을 때 GPU 를 만든다. 앱 시작 때 만들지 않는다).
    //         GPU 를 쓸 수 없으면 그 화면에 ViewNeedsCpuFallback 을 낸다. CPU 모드에서는 핸들을 쓰지 않고 CPU 로 그린다.
    //         핸들은 렌더 스레드가 맡아 정확히 한 번 funcRelease 를 부른다(GPU 화면은 surface 를 지운 뒤).
    int AttachView(ITerminalFrameSource* pSource, const TerminalNativeHandle& handle, const QSize& devicePixelSize, qreal devicePixelRatio);
    void DetachView(int viewId);
    // 네이티브 창 파괴 직전 전용(macOS·Windows, 스펙 §10). 엔진·세션 잠금을 잡은 채 부르지 않는다: 렌더 스레드가 그 잠금을
    // 기다리는 CaptureFrame 안에 있으면 시간이 다 될 때까지 GUI 가 멈춘다(스펙 부록 C)
    bool DetachViewAndWait(int viewId, int timeoutMs);
    void ResizeView(int viewId, const QSize& devicePixelSize, qreal devicePixelRatio);
    void SetVisible(int viewId, bool visible);
    void SetFocus(int viewId, bool focused);
    void Wake(int viewId);
    void ScrollPixels(int viewId, double pixels);   // 장치 픽셀. + = 아래(최근) 쪽
    // 그 화면의 주사율(애니메이션 프레임 간격). GUI 스레드가 QScreen::refreshRate() 로 얻어 넘긴다. 0 이하는 무시, 기본 60Hz
    void SetRefreshRate(int viewId, double hz);
    void SetFont(const QFont& font);
    void SetTheme(const TerminalTheme& theme);
    void SetStatsEnabled(bool enabled);
    // 공급원을 잡는다/놓는다(GUI 스레드). 잡힌 공급원은 붙은 화면이 없어도(떨어진 위젯은 다시 붙을 수 있다), Stop 뒤에도
    // SourceReleased 를 내지 않는다. TerminalSurface 가 사는 동안 자기 공급원을 잡는다. 놓을 때 붙은 화면도 다른 잡은 쪽도
    // 없으면 그 자리에서 SourceReleased 를 낸다.
    void RetainSource(ITerminalFrameSource* pSource);
    void ReleaseSource(ITerminalFrameSource* pSource);

public:
    // 새 화면을 GPU 위젯으로 만들지 정할 때 본다. AUTO 면 GPU 를 쓸 수 없게 될 때까지(시도하기 전에도) GPU,
    // CPU 모드·GPU 를 끈 빌드·네이티브 화면을 만들 수 없는 QPA(offscreen, Linux)·Vulkan 라이브러리를 열 수 없는 환경
    // (TERMINALVIEWER_VK_DISABLE=1 포함, 만들 때 열고 닫아 본다)·GPU 실패 뒤에는 CPU.
    // (SetGpuStrategyFactory 로 GPU Strategy 를 넣어 준 테스트·도구는 QPA 와 상관없이 GPU)
    ETerminalRenderStrategy GetActiveStrategy() const;
    QString GetGpuError() const;     // GPU 를 쓸 수 없게 된 이유(시작 실패·장치 잃음). 없으면 빈 문자열
    QFont GetFont() const;           // 마지막으로 SetFont 한 글꼴(격자 크기 계산용)
    TerminalTheme GetTheme() const;  // 마지막으로 SetTheme 한 테마(여백)

signals:
    // 화면별 신호(FrameReady·FrameStats·ViewNeedsCpuFallback·ViewSurfaceLost)는 DetachView 를 부른 뒤에는 그 id 로 오지 않는다.
    void FrameReady(int viewId, const QImage& imgPatch, const QRect& rectPatch);   // CPU
    // 이 화면은 CPU 위젯으로 바꿔 달라: GPU 를 쓸 수 없음, surface 를 만들 수 없음, 또는 같은 공급원이 표시 없이 surface 를
    // 3번 잇달아 잃었음(그때는 ViewSurfaceLost 대신 이것이 온다)
    void ViewNeedsCpuFallback(int viewId);
    // 새 네이티브 핸들로 다시 붙여 달라(그때까지 그 화면은 그리지 않는다). 같은 공급원이 표시 없이 잇달아 잃은 1·2번째만 온다
    void ViewSurfaceLost(int viewId);
    // GetActiveStrategy 가 바뀌었다. CPU 는 GPU 를 쓸 수 없게 되었다는 뜻이다(이유는 GetGpuError).
    // GPU 를 쓸 수 없게 되면 GetActiveStrategy 가 이미 CPU 였어도(네이티브 화면이 없는 QPA) 낸다
    void StrategyChanged(ETerminalRenderStrategy strategy);
    void FrameStats(const TerminalFrameStats& stats);
    // 렌더 스레드가 그 화면을 다 뗐다(Strategy 에서 떼고 funcRelease 를 불렀고 공급원을 더 쓰지 않는다).
    // DetachView 를 처리할 때마다(그 화면이 없었어도) 그리고 Stop 때 남아 있던 화면마다 낸다.
    // 같은 id 로 여러 번 올 수 있다(받는 쪽은 반복을 무시한다). 공급원을 지워도 되는 때는 이 신호가 아니라 SourceReleased 다.
    void ViewDetached(int viewId);
    // 글꼴이나 여백이 바뀌었다(SetFont·SetTheme 를 부른 스레드에서 낸다). 화면은 격자 크기를 다시 잰다
    void CellLayoutChanged();
    // 그 공급원을 쓰는 화면도(붙은 수) 잡은 쪽도(RetainSource 수) 없어졌다: 렌더 스레드와 위젯 모두 더 쓰지 않으므로 주인은
    // 이제 지워도 된다. 수가 0 이 될 때마다 한 번(다시 붙이거나 잡으면 새로 센다). 내는 스레드:
    // 마지막 ViewDetached 를 받을 때는 이 객체의 스레드(GUI), ReleaseSource·Stop 에서는 그것을 부른 스레드에서 바로.
    void SourceReleased(ITerminalFrameSource* pSource);

private:
    class TerminalRenderThreadImpl;
    std::unique_ptr<TerminalRenderThreadImpl> m_pimpl;
};
