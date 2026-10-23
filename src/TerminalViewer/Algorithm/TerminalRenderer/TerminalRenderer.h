#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawList.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"
#include "TerminalViewer/Interface/IAlgorithm.h"
#include "TerminalViewer/Interface/IStrategy.h"

#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

enum class ETerminalRenderStrategy { CPU = 0, GPU = 1 };

struct TerminalNativeHandle           // GPU 용. CPU 는 비워 둔다
{
    void* pHandle = nullptr;          // macOS: retain 한 CAMetalLayer*, Windows: HWND
    std::function<void(void*)> funcRelease;   // surface 를 지운 뒤 렌더 스레드가 부른다
};

struct TerminalRenderInput
{
    int iViewId = 0;
    const DrawList* pDrawList = nullptr;
    GlyphCache* pGlyphCache = nullptr;
};

struct TerminalRenderOutput
{
    bool bPresented = false;     // 그렸다(CPU: 조각을 만들었다)
    bool bNotReady = false;      // GPU: 펜스나 이미지가 아직이다. 같은 DrawList 로 다음에 다시
    bool bDeviceLost = false;    // GPU: 모든 화면을 CPU 로
    bool bSurfaceLost = false;   // GPU: 이 화면의 surface 를 다시 만들어야 함
    int iPresentMode = -1;       // GPU: 표시한 VkPresentModeKHR 값. CPU: -1
    QImage imgPatch;             // CPU: 바뀐 영역만 잘라 낸 조각(장치 픽셀)
    QRect rectPatch;             // CPU: 조각 위치(장치 픽셀)
};

class ITerminalRenderStrategy : public TerminalViewer::IStrategy<TerminalRenderInput, TerminalRenderOutput, ETerminalRenderStrategy>
{
public:
    // handle 을 맡는다(붙이기가 실패해도): 그 화면을 뗄 때, Strategy 가 지워질 때, 붙이기가 실패하면 바로
    // funcRelease 를 한 번 부른다. GPU 는 surface 를 지운 뒤에 부른다.
    virtual bool AttachView(int viewId, const TerminalNativeHandle& handle, const QSize& devicePixelSize) = 0;
    virtual void DetachView(int viewId) = 0;
    virtual void ResizeView(int viewId, const QSize& devicePixelSize) = 0;
    virtual bool IsReady(int viewId) const = 0;      // 이번에 그릴 수 있는지(CPU 는 항상 참)
    // 렌더 스레드가 GlyphCache 를 버리기 전에 부른다(GPU: 그 캐시의 아틀라스 텍스처를 지운다)
    virtual void ForgetGlyphCache(const GlyphCache* pCache);
};

// GPU Strategy 를 만드는 함수. 만들지 못하면 nullptr 과 pError 에 이유
using TerminalGpuStrategyFactory = std::function<std::unique_ptr<ITerminalRenderStrategy>(QString* pError)>;

// 화면마다 붙인 Strategy 로 그린다. GPU 화면과 CPU 화면이 함께 있을 수 있다(surface 를 못 만든 화면만 CPU, 스펙 §11).
class TerminalRenderer : public TerminalViewer::IAlgorithm<TerminalRenderInput, TerminalRenderOutput, ETerminalRenderStrategy>
{
public:
    TerminalRenderer();
    ~TerminalRenderer() override;

public:
    // GPU 를 고르면 그때 GPU Strategy 를 만든다(앱 시작 때 만들지 않는다). 만들지 못하면 지금 Strategy 로 남는다.
    void SetStrategy(ETerminalRenderStrategy type) override;
    // 화면이 붙은 Strategy 로 그린다
    bool Execute(const TerminalRenderInput& in, TerminalRenderOutput& out) override;
    // 지금 Strategy 에 붙인다. 핸들은 그 Strategy 가 맡는다(ITerminalRenderStrategy::AttachView)
    bool AttachView(int viewId, const TerminalNativeHandle& handle, const QSize& devicePixelSize);
    // 정한 Strategy(이미 있어야 한다)에 붙인다. 없으면 핸들을 놓고 거짓
    bool AttachView(int viewId, const TerminalNativeHandle& handle, const QSize& devicePixelSize, ETerminalRenderStrategy strategy);
    void DetachView(int viewId);
    void ResizeView(int viewId, const QSize& devicePixelSize);
    bool IsReady(int viewId) const;
    // 테스트·도구용: GPU Strategy 를 만드는 함수를 바꾼다(빈 함수면 기본 = TerminalRendererGPU::Create)
    void SetGpuFactory(TerminalGpuStrategyFactory factory);
    // GPU 를 버린다(장치 잃음): GPU Strategy 와 거기 붙은 화면을 모두 지우고 지금 Strategy 를 CPU 로 한다. 지운 화면 id
    std::vector<int> DropGpu();
    void ForgetGlyphCache(const GlyphCache* pCache);   // 모든 Strategy 에 알린다

public:
    ETerminalRenderStrategy GetStrategy() const;
    ETerminalRenderStrategy GetViewStrategy(int viewId) const;   // 붙지 않은 화면은 지금 Strategy
    bool HasStrategy(ETerminalRenderStrategy type) const;
    QString GetGpuError() const;     // 마지막으로 GPU Strategy 를 만들지 못한 이유(없으면 빈 문자열)

private:
    ITerminalRenderStrategy* FindViewStrategy(int viewId) const;

    std::unordered_map<ETerminalRenderStrategy, std::unique_ptr<ITerminalRenderStrategy>> m_hashStrategy;
    std::unordered_map<int, ETerminalRenderStrategy> m_hashViewStrategy;
    ETerminalRenderStrategy m_eStrategy = ETerminalRenderStrategy::CPU;
    TerminalGpuStrategyFactory m_funcGpuFactory;
    QString m_strGpuError;
};
