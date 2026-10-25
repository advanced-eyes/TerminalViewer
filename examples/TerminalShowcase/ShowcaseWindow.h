#pragma once

#include "ShowcaseScene.h"

#include <TerminalViewer/Algorithm/TerminalRenderer/TerminalRendererMode.h>

#include <QImage>
#include <QSize>
#include <QTimer>
#include <QWidget>

class FakeFrameSource;
class QVBoxLayout;
class TerminalRenderThread;
class TerminalSurface;

// 쇼케이스 창: TerminalSurface 하나를 품고, 그 위젯이 받지 않는 입력(휠 · 키)을 처리하고, 공급원을 ShowcaseScene 으로 채운다.
// 공급원과 렌더 스레드는 main 이 갖는다. 끄는 순서(main): 이 창을 지운다 -> 렌더 스레드 Stop -> 공급원을 지운다(SourceReleased 뒤).
class ShowcaseWindow : public QWidget
{
    Q_OBJECT

public:
    static constexpr int kMinFontPx = 6;    // 글꼴 크기 범위(+/- 키, main 의 --font-px)
    static constexpr int kMaxFontPx = 48;
    static constexpr double kMinContrastOff = 1.0;       // 최소 대비 범위(main 의 --min-contrast). 1 = 끔
    static constexpr double kMaxMinContrast = 21.0;
    static constexpr double kToggledMinContrast = 4.5;   // M 키: 끔(1) <-> 이 값(WCAG AA)

    // initialGrid: 처음 창 크기(열 x 행). pSource 는 그 크기로 만든 공급원
    ShowcaseWindow(TerminalRenderThread* pRenderThread, FakeFrameSource* pSource, ETerminalRendererMode mode, const QSize& initialGrid,
                   QWidget* parent = nullptr);
    ~ShowcaseWindow() override;

public:
    QImage GrabFrame();             // CPU: 위젯을 그려 받는다. GPU: 화면에서 이 창 영역을 잡는다

public:
    int GetFrameCount() const;      // 지금 위젯의 화면이 그린 프레임 수(CPU FrameReady, 통계를 켰으면 FrameStats 도)
    TerminalSurface* GetSurface() const;
    const ShowcaseScene& GetScene() const;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    void CreateSurface(bool useGpu);
    void OnGridSizeChanged(TerminalSurface* pSurface, const QSize& colsRows);
    void OnCpuFallbackRequested(TerminalSurface* pSurface);
    void OnFrame(int viewId);
    void OnLiveTick();
    void OnCursorTick();
    void ChangeFontSize(int delta);
    void ToggleMinContrast();
    void ScrollByPixels(double pixels);
    void ScrollByRows(double rows);
    void Wake();
    void UpdateTitle();
    QString DescribeCpuReason() const;

    TerminalRenderThread* m_pRenderThread;
    FakeFrameSource* m_pSource;
    ETerminalRendererMode m_eMode;
    ShowcaseScene m_scene;
    QVBoxLayout* m_pLayout;
    TerminalSurface* m_pSurface;
    QTimer m_timerLive;
    QTimer m_timerCursor;
    uint32_t m_uTick;
    int m_iFrames;
    bool m_bFellBack;
};
