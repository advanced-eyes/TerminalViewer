#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalRenderer.h"

#include <QEvent>
#include <QFont>
#include <QImage>
#include <QPointer>
#include <QSize>
#include <QWidget>

class ITerminalFrameSource;
class QWindow;
class TerminalRenderThread;

// 렌더 스레드가 이 화면을 그려도 되는가(스펙 §5 숨은 화면, §15 위험 5): 위젯이 보이고(마지막 show/hide 이벤트) 최상위 창이
// 화면에 드러나 있다. 최소화·다른 창에 완전히 가려짐·다른 Space 의 창은 드러나 있지 않다.
bool IsRenderVisible(bool widgetShown, bool topLevelExposed);
// 최상위 창의 이벤트로 "드러남" 을 갱신한다.
// - Expose: 그 창이 지금 드러났는지(windowExposed: 빈 영역이 아니고 isExposed)를 따른다.
// - Show(창이 다시 보임): 드러남. cocoa 는 숨겼다 다시 보인 창에 노출을 바로 알리지 않을 수 있다.
// - WindowStateChange: 최소화(windowMinimized)면 드러나지 않음, 아니면 드러남. cocoa 는 Dock 에서 돌아온 창에 노출을 다시
//   알리지 않을 때가 있다(이전 레이어 내용을 그대로 보인다).
// 그 밖의 이벤트는 바꾸지 않는다. 드러났다고 본 뒤 실제로는 가려져 있으면 cocoa 가 가려짐을 다시 알린다.
// GPU 자식 창 자신의 노출은 쓰지 않는다(cocoa 는 숨겼다 다시 보인 자식 창에 노출을 다시 알리지 않는다).
bool UpdateTopLevelExposed(bool exposed, QEvent::Type eventType, bool windowExposed, bool windowMinimized);

// 터미널 화면 하나의 그리기 대상(스펙 §10). 렌더 스레드와는 명령으로만 통신한다. 입력은 ③ 이 맡는다.
// GPU: 네이티브 자식 창(WA_NativeWindow, WA_PaintOnScreen, WA_NoSystemBackground, paintEngine = nullptr).
//      처음 보일 때 네이티브 핸들을 만들어 붙이고, 렌더 스레드가 직접 표시한다.
// CPU: 일반 위젯. FrameReady 조각을 보관 이미지에 덮어 쓰고 그린다.
// 공급원(pSource)은 패널이 갖는다. 규칙 하나(GPU·CPU 같음): 주인은 렌더 스레드의 SourceReleased(pSource) 가 올 때 공급원을 놓는다.
//   위젯은 사는 동안 렌더 스레드에 공급원을 잡아 두므로(RetainSource) 위젯이 살아 있는 동안에는 그 신호가 오지 않는다
//   (화면이 떨어진 위젯도 다시 붙을 수 있다). 위젯을 지우면 렌더 스레드가 그 공급원을 더 쓰지 않게 된 뒤 정확히 한 번 온다:
//   한 번도 붙지 않았거나 렌더 스레드가 떼기를 이미 마쳤으면 지우는 자리에서 바로, 아직 붙어 있거나 비동기 떼기가 아직
//   처리 중이면(네이티브 창이 바뀌어 DetachView 를 보낸 직후 등) 렌더 스레드가 떼기를 마친 뒤.
//   (같은 공급원을 다른 위젯이 아직 쓰면 그 위젯까지 사라진 뒤 온다.)
//   GPU 위젯의 소멸자는 최대 500ms 기다리지만 시간을 넘기면 렌더 스레드가 아직 공급원을 쓸 수 있으므로 기다림의 결과에 기대지 않는다.
// ①·③ 에 넘기는 계약(스펙 부록 C):
// - 공급원 하나에 위젯 하나. 두 위젯이 공급원을 나누면 surface 를 잇달아 잃은 셈(3번이면 CPU 로)이 섞이고 한쪽이 표시하면 지워진다.
// - GPU → CPU 바꿔 끼우기(CpuFallbackRequested): 같은 공급원으로 새 CPU 위젯을 먼저 만든 뒤 옛 위젯을 deleteLater 한다.
//   옛 위젯을 먼저 지우면 공급원을 잡은 쪽이 0 이 되어 바꾸는 도중에 SourceReleased 가 올 수 있다.
// - 위젯을 모두 지운 뒤 렌더 스레드를 멈춘다(Application 은 MainWindow 를 먼저 지운다).
class TerminalSurface : public QWidget
{
    Q_OBJECT

public:
    // useGpu: 렌더 스레드의 현재 Strategy 가 GPU 일 때 참으로 만든다(패널(③)이 정함)
    TerminalSurface(TerminalRenderThread* pRenderThread, ITerminalFrameSource* pSource, bool useGpu, QWidget* parent = nullptr);
    ~TerminalSurface() override;      // DetachView (GPU 는 DetachViewAndWait: 바로 뒤에 네이티브 창이 지워진다, 스펙 §10)

public:
    int GetViewId() const;            // 0 = 붙지 않음
    bool IsGpu() const;
    QSize GetGridSize() const;        // 열×행

signals:
    void GridSizeChanged(const QSize& colsRows);
    void CpuFallbackRequested();      // 패널이 CPU 위젯으로 바꿔 끼운다(이 위젯은 deleteLater 로 지운다)

protected:
    QPaintEngine* paintEngine() const override;    // GPU: nullptr
    void paintEvent(QPaintEvent* event) override;  // CPU: 보관한 이미지
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    bool event(QEvent* event) override;            // WinIdChange
    // windowHandle() 의 PlatformSurface, 최상위 창의 Expose·Show·WindowStateChange(드러났는지)
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void Attach();
    void DetachForSurfaceDestruction();
    void OnWinIdChange();
    void OnScreenChanged();
    void OnFrameReady(int viewId, const QImage& imgPatch, const QRect& rectPatch);
    void OnViewNeedsCpuFallback(int viewId);
    void OnViewSurfaceLost(int viewId);
    bool UpdateScale();               // 배율이 바뀌었으면 네이티브 레이어 배율을 맞추고 참
    void SendResize();
    void SendRefreshRate();
    void UpdateGrid();
    void WatchWindows();
    void UpdateRenderVisibility();    // IsRenderVisible 이 바뀌었으면 SetVisible
    void RequestCpuFallback();
    QSize DevicePixelSize() const;

    TerminalRenderThread* m_pRenderThread;
    ITerminalFrameSource* m_pSource;
    bool m_bGpu;
    int m_iViewId;
    void* m_pNativeHandle;            // GPU: 지금 붙인 레이어/HWND(배율 맞추기용. 놓는 것은 렌더 스레드)
    qreal m_dDevicePixelRatio;        // 직전 배율
    QSize m_sizeGrid;
    QImage m_imgFrame;                // CPU: 화면 전체(장치 픽셀, setDevicePixelRatio)
    bool m_bShownOnce;
    bool m_bShown;                    // 마지막 show/hide 이벤트가 show 였다(spontaneous 포함)
    bool m_bTopLevelExposed;          // 최상위 창이 드러나 있다(UpdateTopLevelExposed)
    bool m_bRenderVisible;            // 렌더 스레드에 마지막으로 보낸 보임
    bool m_bAttaching;
    bool m_bFallbackRequested;
    bool m_bReattachPending;              // 네이티브 창이 지워져 뗐다: 새 창이 생기면 다시 붙인다
    QPointer<QWindow> m_pSurfaceWindow;   // PlatformSurface 를 보는 창(이 위젯의 네이티브 창)
    QPointer<QWindow> m_pTopLevelWindow;  // Expose·Show·WindowStateChange 를 보는 최상위 창
    QPointer<QWindow> m_pScreenWindow;    // screenChanged 를 받는 창
    QMetaObject::Connection m_connScreen;
    mutable CellMetrics m_metrics;        // 격자 계산 캐시(글꼴·배율이 같으면 다시 재지 않는다)
    mutable QFont m_fontMetrics;
    mutable qreal m_dMetricsRatio;
};
