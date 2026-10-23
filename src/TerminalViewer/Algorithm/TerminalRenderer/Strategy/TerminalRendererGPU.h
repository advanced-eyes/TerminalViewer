#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalRenderer.h"

#include <QString>

#include <memory>
#include <unordered_map>

class VulkanContext;
class VulkanView;

// Vulkan 으로 그려 화면별 스왑체인에 표시한다(스펙 §8). 렌더 스레드에서 만들고 쓰고 지운다.
// 화면마다 VulkanView 하나. 핸들은 VulkanView 가 맡아 surface 를 지운 뒤 놓는다.
class TerminalRendererGPU : public ITerminalRenderStrategy
{
public:
    static std::unique_ptr<TerminalRendererGPU> Create(QString* pError);   // VulkanContext 를 못 만들면 nullptr
    ~TerminalRendererGPU() override;   // 화면(surface)을 먼저 지우고 장치를 지운다

    ETerminalRenderStrategy GetType() const override;
    bool Execute(const TerminalRenderInput& in, TerminalRenderOutput& out) override;
    // surface 를 만들지 못하거나 이 장치로 표시할 수 없으면 거짓(핸들은 이미 놓았다). 그 화면은 CPU 로 바꾼다(스펙 §11)
    bool AttachView(int viewId, const TerminalNativeHandle& handle, const QSize& devicePixelSize) override;
    void DetachView(int viewId) override;
    void ResizeView(int viewId, const QSize& devicePixelSize) override;
    bool IsReady(int viewId) const override;
    void ForgetGlyphCache(const GlyphCache* pCache) override;

private:
    explicit TerminalRendererGPU(std::unique_ptr<VulkanContext> upContext);

    std::unique_ptr<VulkanContext> m_upContext;
    std::unordered_map<int, std::unique_ptr<VulkanView>> m_hashViews;
};
