#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalRenderer.h"

#include <unordered_map>

// 화면마다 이미지 1장을 유지하고 SoftwareCompositor 로 합성한 뒤 바뀐 영역만 조각으로 낸다. (CPU)
class TerminalRendererCPU : public ITerminalRenderStrategy
{
public:
    ~TerminalRendererCPU() override;   // 남은 화면의 핸들을 놓는다

    ETerminalRenderStrategy GetType() const override;
    bool Execute(const TerminalRenderInput& in, TerminalRenderOutput& out) override;
    bool AttachView(int viewId, const TerminalNativeHandle& handle, const QSize& devicePixelSize) override;
    void DetachView(int viewId) override;
    void ResizeView(int viewId, const QSize& devicePixelSize) override;
    bool IsReady(int viewId) const override;

private:
    struct View
    {
        QSize size;
        QImage imgFrame;   // 화면 전체. 크기가 바뀌면 비워서 다음에 전체를 합성한다
        TerminalNativeHandle handle;   // 쓰지 않지만 맡는다(뗄 때 놓는다)
    };

    std::unordered_map<int, View> m_hashViews;
};
