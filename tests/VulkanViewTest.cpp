#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalRenderer.h"

#include <QTest>

#ifdef TERMINALVIEWER_GPU

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawListBuilder.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanContext.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanView.h"
#include "FakeFrameSource.h"
#include "TestFonts.h"

#include <QElapsedTimer>
#include <QRegularExpression>

#include <atomic>
#include <memory>

#if defined(Q_OS_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CGGeometry.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

namespace
{
constexpr uint32_t kUndefined = 0xFFFFFFFFu;
constexpr int kFrameTimeoutMs = 2000;
constexpr int kFrames = 6;

VkSurfaceCapabilitiesKHR Caps(uint32_t minCount, uint32_t maxCount, VkExtent2D current, VkExtent2D minExtent, VkExtent2D maxExtent)
{
    VkSurfaceCapabilitiesKHR caps{};
    caps.minImageCount = minCount;
    caps.maxImageCount = maxCount;
    caps.currentExtent = current;
    caps.minImageExtent = minExtent;
    caps.maxImageExtent = maxExtent;
    return caps;
}

QFont BaseFont()
{
    QFont font(QStringLiteral("Cascadia Mono"));
    font.setPixelSize(14);
    return font;
}

TerminalNativeHandle CountingHandle(void* pHandle, std::atomic<int>& releases)
{
    TerminalNativeHandle handle;
    handle.pHandle = pHandle;
    handle.funcRelease = [&releases](void* p)
    {
#if defined(Q_OS_MACOS)
        if (p)
            CFRelease(p);
#else
        Q_UNUSED(p)
#endif
        ++releases;
    };
    return handle;
}

#if defined(Q_OS_MACOS)
// 창 없는 CAMetalLayer(+1 retain). MoltenVK 는 레이어로 surface 를 만들므로 offscreen 테스트에서도 스왑체인을 만들 수 있다.
void* CreateMetalLayer(const QSize& size)
{
    Class layerClass = objc_getClass("CAMetalLayer");
    if (!layerClass)
        return nullptr;
    id allocated = reinterpret_cast<id (*)(Class, SEL)>(objc_msgSend)(layerClass, sel_registerName("alloc"));
    id layer = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(allocated, sel_registerName("init"));
    reinterpret_cast<void (*)(id, SEL, CGRect)>(objc_msgSend)(layer, sel_registerName("setBounds:"),
                                                              CGRectMake(0, 0, CGFloat(size.width()), CGFloat(size.height())));
    reinterpret_cast<void (*)(id, SEL, CGFloat)>(objc_msgSend)(layer, sel_registerName("setContentsScale:"), CGFloat(1.0));
    return layer;
}

void ResizeMetalLayer(void* pLayer, const QSize& size)
{
    reinterpret_cast<void (*)(id, SEL, CGRect)>(objc_msgSend)(static_cast<id>(pLayer), sel_registerName("setBounds:"),
                                                              CGRectMake(0, 0, CGFloat(size.width()), CGFloat(size.height())));
}
#endif

// 실패 주입: 이 테스트가 만든 컨텍스트의 함수 표에서 함수 하나를 잠시 바꾼다(범위를 벗어나면 되돌린다).
// 함수 표는 그 컨텍스트 객체의 멤버다(상수가 아니다). 테스트 전용.
template <typename TFunction>
class ScopedVulkanHook
{
public:
    ScopedVulkanHook(TFunction& slot, TFunction replacement)
        : m_slot(slot)
        , m_original(slot)
    {
        m_slot = replacement;
    }
    ~ScopedVulkanHook() { m_slot = m_original; }

    ScopedVulkanHook(const ScopedVulkanHook&) = delete;
    ScopedVulkanHook& operator=(const ScopedVulkanHook&) = delete;

private:
    TFunction& m_slot;
    TFunction m_original;
};

vkb::DispatchTable& MutableDispatch(VulkanContext& context)
{
    return const_cast<vkb::DispatchTable&>(context.GetDispatch());
}

vkb::InstanceDispatchTable& MutableInstanceDispatch(VulkanContext& context)
{
    return const_cast<vkb::InstanceDispatchTable&>(context.GetInstanceDispatch());
}

std::atomic<int> s_iInjectedResult{VK_SUCCESS};

VKAPI_ATTR VkResult VKAPI_CALL InjectedCreateSwapchain(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR* pSwapchain)
{
    *pSwapchain = VK_NULL_HANDLE;
    return VkResult(s_iInjectedResult.load());
}

// VK_SUCCESS 면 크기 0 인 능력(최소화된 창 흉내)
VKAPI_ATTR VkResult VKAPI_CALL InjectedSurfaceCaps(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR* pCaps)
{
    *pCaps = VkSurfaceCapabilitiesKHR{};
    pCaps->minImageCount = 2;
    return VkResult(s_iInjectedResult.load());
}

VKAPI_ATTR VkResult VKAPI_CALL InjectedQueueWaitIdle(VkQueue)
{
    return VkResult(s_iInjectedResult.load());
}

VKAPI_ATTR VkResult VKAPI_CALL InjectedWaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)
{
    return VkResult(s_iInjectedResult.load());
}

// 아틀라스 이미지 뷰를 만들지 못하게 한다(아틀라스를 GPU 에 올리지 못한 경우 흉내)
VKAPI_ATTR VkResult VKAPI_CALL InjectedCreateImageView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* pView)
{
    *pView = VK_NULL_HANDLE;
    return VkResult(s_iInjectedResult.load());
}

// "Vulkan:" 로 시작하는 경고 수를 센다(다른 메시지와 함께 원래 처리기로도 넘긴다)
class VulkanWarningCounter
{
public:
    VulkanWarningCounter()
    {
        s_iCount = 0;
        s_previous = qInstallMessageHandler(&Handle);
    }
    ~VulkanWarningCounter() { qInstallMessageHandler(s_previous); }

    VulkanWarningCounter(const VulkanWarningCounter&) = delete;
    VulkanWarningCounter& operator=(const VulkanWarningCounter&) = delete;

    int GetCount() const { return s_iCount; }

private:
    static void Handle(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        if (type == QtWarningMsg && message.startsWith(QStringLiteral("Vulkan:")))
            ++s_iCount;
        if (s_previous)
            s_previous(type, context, message);
    }

    static inline std::atomic<int> s_iCount{0};
    static inline QtMessageHandler s_previous = nullptr;
};
}

class VulkanViewTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<VulkanContext> m_upContext;
    int m_iValidationBefore = 0;

    // 공급원 → 그리기 목록(화면 크기에 맞춘 격자)
    static bool BuildList(FakeFrameSource& source, GlyphCache& cache, DrawList& list)
    {
        DrawListBuilder builder;
        TerminalTheme theme;
        TerminalFrame frame;
        TerminalCaptureRequest request;
        request.bFull = true;
        source.CaptureFrame(request, frame);
        DrawListBuildStats stats;
        return builder.Build(frame, theme, true, true, 0, cache, list, stats) == EBuildResult::OK;
    }

    // 펜스·이미지가 준비될 때까지(타임아웃 0 이라 몇 번 다시) 그려 표시한다
    static bool RenderPresented(VulkanView& view, const DrawList& list, GlyphCache& cache)
    {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < kFrameTimeoutMs)
        {
            if (!view.IsReady())
            {
                QTest::qWait(1);
                continue;
            }
            TerminalRenderOutput out;
            view.Render(list, cache, out);
            if (out.bDeviceLost || out.bSurfaceLost)
                return false;
            if (out.bPresented)
                return true;
            QTest::qWait(1);
        }
        return false;
    }

private slots:
    void initTestCase()
    {
        TestFonts::Register();
        QString strError;
        m_upContext = VulkanContext::Create(&strError);
        if (!m_upContext)
            qInfo("VulkanViewTest: no Vulkan device (%s); device tests are skipped", qPrintable(strError));
        m_iValidationBefore = VulkanContext::GetValidationMessageCount();
    }

    void cleanupTestCase()
    {
        m_upContext.reset();
    }

    void extentUsesCurrentExtentWhenDefined()
    {
        const VkSurfaceCapabilitiesKHR caps = Caps(2, 3, {640, 480}, {1, 1}, {4096, 4096});
        const std::optional<VkExtent2D> optExtent = ChooseSwapchainExtent(caps, QSize(800, 600));
        QVERIFY(optExtent);
        QCOMPARE(optExtent->width, 640u);
        QCOMPARE(optExtent->height, 480u);
    }

    void extentClampsRequestWhenUndefined()
    {
        const VkSurfaceCapabilitiesKHR caps = Caps(2, 3, {kUndefined, kUndefined}, {100, 50}, {1000, 700});
        std::optional<VkExtent2D> optExtent = ChooseSwapchainExtent(caps, QSize(800, 600));
        QVERIFY(optExtent);
        QCOMPARE(optExtent->width, 800u);
        QCOMPARE(optExtent->height, 600u);
        optExtent = ChooseSwapchainExtent(caps, QSize(5000, 20));
        QVERIFY(optExtent);
        QCOMPARE(optExtent->width, 1000u);   // max 로 자름
        QCOMPARE(optExtent->height, 50u);    // min 으로 자름
    }

    void extentIsEmptyForZeroSizes()
    {
        // 요청이 0×0 이면 그리지 않는다(currentExtent 가 정해져 있어도)
        QVERIFY(!ChooseSwapchainExtent(Caps(2, 3, {kUndefined, kUndefined}, {1, 1}, {4096, 4096}), QSize(0, 0)));
        QVERIFY(!ChooseSwapchainExtent(Caps(2, 3, {640, 480}, {1, 1}, {4096, 4096}), QSize(0, 0)));
        QVERIFY(!ChooseSwapchainExtent(Caps(2, 3, {640, 480}, {1, 1}, {4096, 4096}), QSize(100, 0)));
        // surface 가 최소화 등으로 최대 크기 0 을 알리면 그리지 않는다(Windows)
        QVERIFY(!ChooseSwapchainExtent(Caps(2, 3, {0, 0}, {0, 0}, {0, 0}), QSize(800, 600)));
        QVERIFY(!ChooseSwapchainExtent(Caps(2, 3, {kUndefined, kUndefined}, {0, 0}, {0, 0}), QSize(800, 600)));
    }

    void presentModePrefersMailbox()
    {
        QCOMPARE(ChoosePresentMode({VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR}), VK_PRESENT_MODE_MAILBOX_KHR);
        QCOMPARE(ChoosePresentMode({VK_PRESENT_MODE_FIFO_KHR}), VK_PRESENT_MODE_FIFO_KHR);
        QCOMPARE(ChoosePresentMode({VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR}), VK_PRESENT_MODE_FIFO_KHR);
    }

    void imageCountFollowsModeWithinCaps()
    {
        QCOMPARE(ChooseImageCount(Caps(2, 8, {}, {}, {}), VK_PRESENT_MODE_MAILBOX_KHR), 3u);
        QCOMPARE(ChooseImageCount(Caps(2, 8, {}, {}, {}), VK_PRESENT_MODE_FIFO_KHR), 2u);
        QCOMPARE(ChooseImageCount(Caps(4, 8, {}, {}, {}), VK_PRESENT_MODE_FIFO_KHR), 4u);      // caps.min 4 → 4
        QCOMPARE(ChooseImageCount(Caps(1, 2, {}, {}, {}), VK_PRESENT_MODE_MAILBOX_KHR), 2u);   // caps.max 2 → 2
        QCOMPARE(ChooseImageCount(Caps(1, 0, {}, {}, {}), VK_PRESENT_MODE_MAILBOX_KHR), 3u);   // max 0 = 한도 없음
    }

    void viewWithoutHandleHasNoSurface()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        std::atomic<int> iReleases{0};
        {
            VulkanView view(*m_upContext, CountingHandle(nullptr, iReleases), QSize(100, 100));
            QVERIFY(!view.HasSurface());
            QVERIFY(!view.IsReady());
            GlyphCache cache(BaseFont(), 1.0);
            DrawList list;
            TerminalRenderOutput out;
            view.Render(list, cache, out);
            QVERIFY(!out.bPresented);
            QCOMPARE(iReleases.load(), 0);
        }
        QCOMPARE(iReleases.load(), 1);   // 만들지 못한 화면도 지울 때 핸들을 한 번 놓는다
    }

    void gpuStrategyRejectsViewWithoutSurface()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        std::atomic<int> iReleases{0};
        TerminalRenderer renderer;
        renderer.SetStrategy(ETerminalRenderStrategy::GPU);
        QCOMPARE(renderer.GetStrategy(), ETerminalRenderStrategy::GPU);
        QVERIFY(!renderer.AttachView(1, CountingHandle(nullptr, iReleases), QSize(100, 100), ETerminalRenderStrategy::GPU));
        QCOMPARE(iReleases.load(), 1);   // 붙이기가 실패하면 Strategy 가 바로 놓는다
        QVERIFY(!renderer.IsReady(1));
    }

    void rendersAndResizesMetalLayer()
    {
#if !defined(Q_OS_MACOS)
        QSKIP("CAMetalLayer 는 macOS 전용");
#else
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        const QSize size(240, 120);
        void* pLayer = CreateMetalLayer(size);
        QVERIFY(pLayer);
        std::atomic<int> iReleases{0};
        GlyphCache cache(BaseFont(), 1.0);
        FakeFrameSource source(20, 5, 0);
        source.FillPattern(EFakePattern::MIXED, 3);
        DrawList list;
        QVERIFY(BuildList(source, cache, list));
        QVERIFY(!list.vecQuads.empty());
        {
            VulkanView view(*m_upContext, CountingHandle(pLayer, iReleases), size);
            QVERIFY(view.HasSurface());
            QVERIFY(view.IsReady());
            QCOMPARE(view.GetPresentMode(), VK_PRESENT_MODE_FIFO_KHR);   // MoltenVK 에는 MAILBOX 가 없다
            for (int i = 0; i < kFrames; ++i)
                QVERIFY2(RenderPresented(view, list, cache), qPrintable(QStringLiteral("frame %1").arg(i)));
            QCOMPARE(view.GetSwapchainExtent().width, uint32_t(size.width()));
            QCOMPARE(view.GetSwapchainExtent().height, uint32_t(size.height()));
            QCOMPARE(view.GetSwapchainCreateCount(), 1);
            QCOMPARE(view.UsesPresentScaling(), m_upContext->HasPresentScaling());

            view.Resize(size);   // 같은 크기: 다시 만들지 않는다
            QVERIFY(RenderPresented(view, list, cache));
            QCOMPARE(view.GetSwapchainCreateCount(), 1);

            const QSize bigger(300, 160);
            ResizeMetalLayer(pLayer, bigger);
            view.Resize(bigger);
            for (int i = 0; i < kFrames; ++i)
                QVERIFY(RenderPresented(view, list, cache));
            QCOMPARE(view.GetSwapchainCreateCount(), 2);
            QCOMPARE(view.GetSwapchainExtent().width, uint32_t(bigger.width()));
            QCOMPARE(view.GetSwapchainExtent().height, uint32_t(bigger.height()));

            view.Resize(QSize(0, 0));   // 크기 0: 그리지 않는다
            QVERIFY(!view.IsReady());
            TerminalRenderOutput out;
            view.Render(list, cache, out);
            QVERIFY(!out.bPresented);
            QCOMPARE(iReleases.load(), 0);
        }
        QCOMPARE(iReleases.load(), 1);   // surface 를 지운 뒤 한 번 놓는다
        QCOMPARE(VulkanContext::GetValidationMessageCount(), m_iValidationBefore);
#endif
    }

    void gpuStrategyDrawsAttachedLayer()
    {
#if !defined(Q_OS_MACOS)
        QSKIP("CAMetalLayer 는 macOS 전용");
#else
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        const QSize size(200, 100);
        std::atomic<int> iReleases{0};
        GlyphCache cache(BaseFont(), 1.0);
        FakeFrameSource source(20, 5, 0);
        source.FillPattern(EFakePattern::MIXED, 9);
        DrawList list;
        QVERIFY(BuildList(source, cache, list));
        {
            TerminalRenderer renderer;
            renderer.SetStrategy(ETerminalRenderStrategy::GPU);
            QVERIFY(renderer.HasStrategy(ETerminalRenderStrategy::GPU));
            QVERIFY(renderer.AttachView(7, CountingHandle(CreateMetalLayer(size), iReleases), size, ETerminalRenderStrategy::GPU));
            QCOMPARE(renderer.GetViewStrategy(7), ETerminalRenderStrategy::GPU);
            QVERIFY(renderer.AttachView(8, {}, size, ETerminalRenderStrategy::CPU));   // CPU 화면도 함께 붙는다
            QCOMPARE(renderer.GetViewStrategy(8), ETerminalRenderStrategy::CPU);
            bool bPresented = false;
            QElapsedTimer timer;
            timer.start();
            while (!bPresented && timer.elapsed() < kFrameTimeoutMs)
            {
                if (!renderer.IsReady(7))
                {
                    QTest::qWait(1);
                    continue;
                }
                TerminalRenderOutput out;
                renderer.Execute(TerminalRenderInput{7, &list, &cache}, out);
                QVERIFY(!out.bDeviceLost && !out.bSurfaceLost);
                QVERIFY(out.imgPatch.isNull());   // GPU 는 조각을 내지 않는다
                bPresented = out.bPresented;
            }
            QVERIFY(bPresented);
            TerminalRenderOutput cpuOut;
            QVERIFY(renderer.Execute(TerminalRenderInput{8, &list, &cache}, cpuOut));
            QVERIFY(!cpuOut.imgPatch.isNull());
            renderer.ForgetGlyphCache(&cache);   // 아틀라스 텍스처를 지운다(다음 그리기는 다시 올린다)
            renderer.DetachView(7);
            QCOMPARE(iReleases.load(), 1);
        }
        QCOMPARE(iReleases.load(), 1);
        QCOMPARE(VulkanContext::GetValidationMessageCount(), m_iValidationBefore);
#endif
    }

    void metalLayerUsesPresentScalingWhenOffered()
    {
#if !defined(Q_OS_MACOS)
        QSKIP("present scaling 은 macOS 에서만 쓴다");
#else
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        if (!m_upContext->HasPresentScaling())
            QSKIP("이 MoltenVK 는 VK_EXT_swapchain_maintenance1 을 알리지 않는다(늘어남을 받아들인다)");
        const QSize size(160, 90);
        std::atomic<int> iReleases{0};
        GlyphCache cache(BaseFont(), 1.0);
        DrawList list;
        VulkanView view(*m_upContext, CountingHandle(CreateMetalLayer(size), iReleases), size);
        QVERIFY(RenderPresented(view, list, cache));
        QVERIFY(view.UsesPresentScaling());   // ONE_TO_ONE, gravity X = MIN, Y = MAX 로 만들었다
#endif
    }

    // 스왑체인을 다시 만들다 실패하면 bNotReady(1ms 마다 다시)로 삼키지 않고 surface 잃음/장치 잃음으로 알린다(스펙 §11)
    void recreateFailureReportsSurfaceOrDeviceLoss_data()
    {
        QTest::addColumn<int>("result");
        QTest::addColumn<bool>("surfaceLost");
        QTest::newRow("surface lost") << int(VK_ERROR_SURFACE_LOST_KHR) << true;
        QTest::newRow("native window in use") << int(VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) << true;
        QTest::newRow("device lost") << int(VK_ERROR_DEVICE_LOST) << false;
        QTest::newRow("out of device memory") << int(VK_ERROR_OUT_OF_DEVICE_MEMORY) << false;
        QTest::newRow("out of host memory") << int(VK_ERROR_OUT_OF_HOST_MEMORY) << false;
        QTest::newRow("unknown error") << int(VK_ERROR_INITIALIZATION_FAILED) << false;
    }

    void recreateFailureReportsSurfaceOrDeviceLoss()
    {
#if !defined(Q_OS_MACOS)
        QSKIP("CAMetalLayer 는 macOS 전용");
#else
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        QFETCH(int, result);
        QFETCH(bool, surfaceLost);
        const QSize size(120, 80);
        std::atomic<int> iReleases{0};
        GlyphCache cache(BaseFont(), 1.0);
        DrawList list;
        VulkanView view(*m_upContext, CountingHandle(CreateMetalLayer(size), iReleases), size);
        QVERIFY(view.HasSurface());
        s_iInjectedResult = result;
        ScopedVulkanHook<PFN_vkCreateSwapchainKHR> hook(MutableDispatch(*m_upContext).fp_vkCreateSwapchainKHR, &InjectedCreateSwapchain);
        VulkanWarningCounter warnings;
        for (int iTry = 0; iTry < 3; ++iTry)
        {
            QVERIFY(view.IsReady());
            TerminalRenderOutput out;
            view.Render(list, cache, out);
            QVERIFY(!out.bNotReady);   // 바쁜 재시도가 되지 않는다
            QVERIFY(!out.bPresented);
            QCOMPARE(out.bSurfaceLost, surfaceLost);
            QCOMPARE(out.bDeviceLost, !surfaceLost);
        }
        QCOMPARE(warnings.GetCount(), 1);   // 로그는 화면마다 한 번
#endif
    }

    void surfaceQueryFailureReportsSurfaceLost()
    {
#if !defined(Q_OS_MACOS)
        QSKIP("CAMetalLayer 는 macOS 전용");
#else
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        const QSize size(120, 80);
        std::atomic<int> iReleases{0};
        GlyphCache cache(BaseFont(), 1.0);
        DrawList list;
        VulkanView view(*m_upContext, CountingHandle(CreateMetalLayer(size), iReleases), size);
        s_iInjectedResult = int(VK_ERROR_SURFACE_LOST_KHR);
        ScopedVulkanHook<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR> hook(
            MutableInstanceDispatch(*m_upContext).fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, &InjectedSurfaceCaps);
        TerminalRenderOutput out;
        view.Render(list, cache, out);
        QVERIFY(out.bSurfaceLost);
        QVERIFY(!out.bNotReady && !out.bDeviceLost && !out.bPresented);
#endif
    }

    void zeroSizedSurfaceWaitsQuietly()
    {
#if !defined(Q_OS_MACOS)
        QSKIP("CAMetalLayer 는 macOS 전용");
#else
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        // surface 가 크기 0 을 알리면(최소화) 오류가 아니다: 그리지 않고 다음에 다시 본다(숨김 명령이 오면 렌더 스레드가 멈춘다)
        const QSize size(120, 80);
        std::atomic<int> iReleases{0};
        GlyphCache cache(BaseFont(), 1.0);
        DrawList list;
        VulkanView view(*m_upContext, CountingHandle(CreateMetalLayer(size), iReleases), size);
        s_iInjectedResult = int(VK_SUCCESS);
        ScopedVulkanHook<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR> hook(
            MutableInstanceDispatch(*m_upContext).fp_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, &InjectedSurfaceCaps);
        VulkanWarningCounter warnings;
        for (int iTry = 0; iTry < 3; ++iTry)
        {
            TerminalRenderOutput out;
            view.Render(list, cache, out);
            QVERIFY(out.bNotReady);
            QVERIFY(!out.bSurfaceLost && !out.bDeviceLost && !out.bPresented);
        }
        QCOMPARE(warnings.GetCount(), 0);
#endif
    }

    void waitFailureWhileRecreatingReportsDeviceLost_data()
    {
        QTest::addColumn<bool>("queueWait");
        QTest::newRow("vkQueueWaitIdle") << true;
        QTest::newRow("vkWaitForFences") << false;
    }

    void waitFailureWhileRecreatingReportsDeviceLost()
    {
#if !defined(Q_OS_MACOS)
        QSKIP("CAMetalLayer 는 macOS 전용");
#else
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        QFETCH(bool, queueWait);
        const QSize size(120, 80);
        void* pLayer = CreateMetalLayer(size);
        std::atomic<int> iReleases{0};
        GlyphCache cache(BaseFont(), 1.0);
        DrawList list;
        VulkanView view(*m_upContext, CountingHandle(pLayer, iReleases), size);
        QVERIFY(RenderPresented(view, list, cache));   // 옛 스왑체인이 있다
        const QSize bigger(150, 100);
        ResizeMetalLayer(pLayer, bigger);
        view.Resize(bigger);
        s_iInjectedResult = int(VK_ERROR_DEVICE_LOST);
        TerminalRenderOutput out;
        {
            vkb::DispatchTable& dispatch = MutableDispatch(*m_upContext);
            std::unique_ptr<ScopedVulkanHook<PFN_vkQueueWaitIdle>> upQueueHook;
            std::unique_ptr<ScopedVulkanHook<PFN_vkWaitForFences>> upFenceHook;
            if (queueWait)
                upQueueHook = std::make_unique<ScopedVulkanHook<PFN_vkQueueWaitIdle>>(dispatch.fp_vkQueueWaitIdle, &InjectedQueueWaitIdle);
            else
                upFenceHook = std::make_unique<ScopedVulkanHook<PFN_vkWaitForFences>>(dispatch.fp_vkWaitForFences, &InjectedWaitForFences);
            // 펜스는 기다리지 않고 보므로 앞 프레임이 끝날 때까지는 bNotReady 다. 그 뒤 다시 만들 때 주입한 실패를 만난다.
            QVERIFY(QTest::qWaitFor([&]
            {
                out = TerminalRenderOutput();
                if (view.IsReady())
                    view.Render(list, cache, out);
                return out.bDeviceLost || out.bSurfaceLost || out.bPresented;
            }, kFrameTimeoutMs));
        }
        QVERIFY(out.bDeviceLost);
        QVERIFY(!out.bNotReady && !out.bSurfaceLost && !out.bPresented);
#endif
    }

    void fenceWaitTimeoutWhileRecreatingRetries()
    {
#if !defined(Q_OS_MACOS)
        QSKIP("CAMetalLayer 는 macOS 전용");
#else
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        // 펜스가 시간 안에 안 끝나는 것은 잠깐 막힌 것일 수 있다(MoltenVK 가 ~1초 멈춘 적이 있다, 부록 A S1).
        // 장치 잃음으로 보지 않고 bNotReady(재시도 간격으로 다시)로 알리며 옛 스왑체인을 그대로 둔다.
        const QSize size(120, 80);
        void* pLayer = CreateMetalLayer(size);
        std::atomic<int> iReleases{0};
        GlyphCache cache(BaseFont(), 1.0);
        DrawList list;
        VulkanView view(*m_upContext, CountingHandle(pLayer, iReleases), size);
        QVERIFY(RenderPresented(view, list, cache));
        QCOMPARE(view.GetSwapchainCreateCount(), 1);
        const QSize bigger(150, 100);
        ResizeMetalLayer(pLayer, bigger);
        view.Resize(bigger);
        s_iInjectedResult = int(VK_TIMEOUT);
        VulkanWarningCounter warnings;
        {
            ScopedVulkanHook<PFN_vkWaitForFences> hook(MutableDispatch(*m_upContext).fp_vkWaitForFences, &InjectedWaitForFences);
            for (int iTry = 0; iTry < 3; ++iTry)
            {
                QVERIFY(QTest::qWaitFor([&] { return view.IsReady(); }, kFrameTimeoutMs));
                TerminalRenderOutput out;
                view.Render(list, cache, out);
                QVERIFY(out.bNotReady);
                QVERIFY(!out.bDeviceLost && !out.bSurfaceLost && !out.bPresented);
            }
            QCOMPARE(view.GetSwapchainCreateCount(), 1);                          // 옛 스왑체인을 지우지 않았다
            QCOMPARE(view.GetSwapchainExtent().width, uint32_t(size.width()));
        }
        QVERIFY(warnings.GetCount() <= 1);                                         // 로그는 많아야 한 번
        QVERIFY(RenderPresented(view, list, cache));                               // 막힘이 풀리면 다시 만든다
        QCOMPARE(view.GetSwapchainCreateCount(), 2);
        QCOMPARE(view.GetSwapchainExtent().width, uint32_t(bigger.width()));
#endif
    }

    void atlasNotOnGpuIsNotReportedAsPresented()
    {
#if !defined(Q_OS_MACOS)
        QSKIP("CAMetalLayer 는 macOS 전용");
#else
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        // 아틀라스를 GPU 에 올리지 못하면(이미지·스테이징 메모리 실패) 획득한 이미지를 지우기만 해 돌려준다. 그것을 "표시했다" 로
        // 알리지 않고 bNotReady 로 알려 렌더 스레드가 같은 목록으로 다시 그리게 한다(다음 차례에 아틀라스를 처음부터 올린다).
        const QSize size(200, 100);
        std::atomic<int> iReleases{0};
        FakeFrameSource source(20, 5, 0);
        source.FillPattern(EFakePattern::MIXED, 5);
        GlyphCache firstCache(BaseFont(), 1.0);
        DrawList firstList;
        QVERIFY(BuildList(source, firstCache, firstList));
        VulkanView view(*m_upContext, CountingHandle(CreateMetalLayer(size), iReleases), size);
        QVERIFY(RenderPresented(view, firstList, firstCache));   // 스왑체인을 만들었다(뒤에서 이미지 뷰를 다시 만들지 않는다)

        GlyphCache cache(BaseFont(), 1.0);   // 새 캐시: 아틀라스 텍스처를 새로 만든다
        DrawList list;
        QVERIFY(BuildList(source, cache, list));
        QVERIFY(!list.vecQuads.empty());
        TerminalRenderOutput out;
        {
            s_iInjectedResult = int(VK_ERROR_OUT_OF_DEVICE_MEMORY);
            ScopedVulkanHook<PFN_vkCreateImageView> hook(MutableDispatch(*m_upContext).fp_vkCreateImageView, &InjectedCreateImageView);
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("^Vulkan: failed to create the mono atlas image")));
            QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("^Vulkan: failed to create the color atlas image")));
            QVERIFY(QTest::qWaitFor([&] { return view.IsReady(); }, kFrameTimeoutMs));
            view.Render(list, cache, out);
        }
        QVERIFY(!out.bPresented);
        QVERIFY(out.bNotReady);
        QVERIFY(!out.bDeviceLost && !out.bSurfaceLost);
        QVERIFY(RenderPresented(view, list, cache));   // 다시 그리면 아틀라스를 올리고 표시한다
        m_upContext->ReleaseAtlasTexture(&cache);
        m_upContext->ReleaseAtlasTexture(&firstCache);
        QCOMPARE(VulkanContext::GetValidationMessageCount(), m_iValidationBefore);
#endif
    }
};

#else   // TERMINALVIEWER_GPU

// GPU 를 끈 빌드(-DTERMINALVIEWER_GPU=OFF): VulkanView 가 빠진다.
class VulkanViewTest : public QObject
{
    Q_OBJECT

private slots:
    void gpuNotBuilt()
    {
        QSKIP("TERMINALVIEWER_GPU=OFF: VulkanView 없음");
    }
};

#endif  // TERMINALVIEWER_GPU

QObject* CreateVulkanViewTest() { return new VulkanViewTest(); }

#include "VulkanViewTest.moc"
