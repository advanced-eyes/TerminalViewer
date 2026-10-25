#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawListBuilder.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/SoftwareCompositor.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalPlatform.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalRenderer.h"
#include "TerminalViewer/Testing/FakeFrameSource.h"
#include "ScopedEnv.h"
#include "TestFonts.h"

#ifdef TERMINALVIEWER_GPU
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanAtlasTexture.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanContext.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanLoader.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanOffscreen.h"
#endif

#include <QFile>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <new>

namespace
{
// GPU 를 만들지 못할 때 TerminalRenderer 가 내는 경고(기대한 경고라 테스트 출력에서 지운다)
QRegularExpression GpuUnavailableWarning()
{
    return QRegularExpression(QStringLiteral("^Terminal renderer: GPU unavailable, drawing with CPU: "));
}
}

#ifdef TERMINALVIEWER_GPU

namespace
{
constexpr int kMaxChannelDiff = 2;   // 성공 기준 6: GPU 와 CPU 의 채널별 차이
constexpr TerminalRgb FRAME_BG{12, 24, 36};
constexpr TerminalRgb FRAME_FG{210, 220, 230};

// 같은 저장 공간(같은 주소)에 GlyphCache 를 다시 만들 수 있게 한다. 테스트가 중간에 실패해 돌아와도 지운다.
class PlacedGlyphCache
{
public:
    PlacedGlyphCache() = default;
    ~PlacedGlyphCache() { Destroy(); }

    PlacedGlyphCache(const PlacedGlyphCache&) = delete;
    PlacedGlyphCache& operator=(const PlacedGlyphCache&) = delete;

    GlyphCache& Emplace(const QFont& font, qreal devicePixelRatio, int atlasSize)
    {
        Destroy();
        m_pCache = new (m_storage) GlyphCache(font, devicePixelRatio, atlasSize, atlasSize);
        return *m_pCache;
    }
    void Destroy()
    {
        if (m_pCache)
            m_pCache->~GlyphCache();
        m_pCache = nullptr;
    }

private:
    alignas(GlyphCache) unsigned char m_storage[sizeof(GlyphCache)];
    GlyphCache* m_pCache = nullptr;
};

// 기록만 하고 제출하지 않는 명령 버퍼(제출 실패 흉내). 범위를 벗어나면 기록을 끝내고 풀을 지운다.
class UnsubmittedCommands
{
public:
    explicit UnsubmittedCommands(VulkanContext& context)
        : m_context(context)
    {
        const vkb::DispatchTable& dispatch = m_context.GetDispatch();
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_context.GetQueueFamily();
        if (dispatch.createCommandPool(&poolInfo, nullptr, &m_pool) != VK_SUCCESS)
            return;
        VkCommandBufferAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocateInfo.commandPool = m_pool;
        allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocateInfo.commandBufferCount = 1;
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        if (dispatch.allocateCommandBuffers(&allocateInfo, &m_cmd) != VK_SUCCESS ||
            dispatch.beginCommandBuffer(m_cmd, &beginInfo) != VK_SUCCESS)
            m_cmd = VK_NULL_HANDLE;
    }
    ~UnsubmittedCommands()
    {
        const vkb::DispatchTable& dispatch = m_context.GetDispatch();
        if (m_cmd != VK_NULL_HANDLE)
        {
            dispatch.endCommandBuffer(m_cmd);
            m_context.ForgetCommandBuffer(m_cmd);
        }
        if (m_pool != VK_NULL_HANDLE)
            dispatch.destroyCommandPool(m_pool, nullptr);
    }

    UnsubmittedCommands(const UnsubmittedCommands&) = delete;
    UnsubmittedCommands& operator=(const UnsubmittedCommands&) = delete;

    VkCommandBuffer Get() const { return m_cmd; }

private:
    VulkanContext& m_context;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    VkCommandBuffer m_cmd = VK_NULL_HANDLE;
};

// 검증을 요청했지만 켜지 못했을 때의 경고를 센다(기대한 경고라 출력에서 지운다. 다른 메시지는 원래 처리기로 넘긴다).
// 로더가 있으면 "레이어 없음", 로더가 없으면(번들 MoltenVK 만 있는 기기) "로더 없음" 이 온다.
class ValidationOffWarnings
{
public:
    ValidationOffWarnings()
    {
        s_iLayerMissing = 0;
        s_iLoaderMissing = 0;
        s_previous = qInstallMessageHandler(&Handle);
    }
    ~ValidationOffWarnings() { qInstallMessageHandler(s_previous); }

    ValidationOffWarnings(const ValidationOffWarnings&) = delete;
    ValidationOffWarnings& operator=(const ValidationOffWarnings&) = delete;

    int GetLayerMissingCount() const { return s_iLayerMissing; }
    int GetLoaderMissingCount() const { return s_iLoaderMissing; }

private:
    static void Handle(QtMsgType type, const QMessageLogContext& context, const QString& message)
    {
        if (type == QtWarningMsg && message.startsWith(QStringLiteral("Vulkan: TERMINALVIEWER_VK_VALIDATION=1 but VK_LAYER_KHRONOS_validation is not available")))
        {
            ++s_iLayerMissing;
            return;
        }
        if (type == QtWarningMsg && message.startsWith(QStringLiteral("Vulkan: TERMINALVIEWER_VK_VALIDATION=1 but no Vulkan loader was found")))
        {
            ++s_iLoaderMissing;
            return;
        }
        if (s_previous)
            s_previous(type, context, message);
    }

    static inline int s_iLayerMissing = 0;
    static inline int s_iLoaderMissing = 0;
    static inline QtMessageHandler s_previous = nullptr;
};
}

class VulkanRendererTest : public QObject
{
    Q_OBJECT

private:
    std::unique_ptr<VulkanContext> m_upContext;
    QString m_strCreateError;
    int m_iValidationBefore = 0;

    static QFont BaseFont()
    {
        QFont font(QStringLiteral("Cascadia Mono"));
        font.setPixelSize(14);
        return font;
    }

    static TerminalTheme Theme()
    {
        TerminalTheme theme;
        theme.selectionBg = TerminalRgb{50, 90, 160};
        theme.cursorColor = TerminalRgb{250, 200, 0};
        theme.iPaddingLogicalPx = 4;
        return theme;
    }

    // 목록 격자 + 여백 + 오른쪽·아래 나머지(격자에 안 맞는 띠)
    static QSize TargetSize(const DrawList& list)
    {
        return QSize(list.iCols * list.iCellWidth + 2 * list.iPaddingPx + 3,
                     list.iRows * list.iCellHeight + 2 * list.iPaddingPx + 5);
    }

    // 공급원의 전체 프레임으로 목록을 만든다. 아틀라스가 꽉 차 NEED_FULL_FRAME 이면 다시 캡처해 만든다.
    // theme 을 주지 않으면 Theme()(최소 대비 끔)
    static bool Build(FakeFrameSource& source, GlyphCache& cache, int scrollOffsetPx, DrawList& list,
                      const TerminalTheme& theme = Theme())
    {
        DrawListBuilder builder;
        for (int iAttempt = 0; iAttempt < 4; ++iAttempt)
        {
            TerminalFrame frame;
            source.CaptureFrame(TerminalCaptureRequest{0, scrollOffsetPx > 0 ? 1 : 0, true}, frame);
            DrawListBuildStats stats;
            if (builder.Build(frame, theme, true, true, scrollOffsetPx, cache, list, stats) == EBuildResult::OK)
                return true;
        }
        return false;
    }

    // MIXED: 박스·한글·이모지·선택(가운데 행)·BLOCK 커서·CURLY 밑줄(2행)
    static void FillMixed(FakeFrameSource& source)
    {
        source.SetColors(FRAME_BG, FRAME_FG);
        source.FillPattern(EFakePattern::MIXED, 7);
        TerminalCursor cursor;
        cursor.bVisible = true;
        cursor.eShape = ECursorShape::BLOCK;
        cursor.iCol = 4;
        cursor.iRow = 0;
        source.SetCursor(cursor);
    }

    static QImage RenderCpu(const DrawList& list, const GlyphCache& cache, const QSize& size)
    {
        QImage image;
        SoftwareCompositor::Compose(list, cache.GetAtlas(EAtlasPage::MONO), cache.GetAtlas(EAtlasPage::COLOR), image, size);
        return image;
    }

    // 채널별 최대 차이. 크기·형식이 다르면 256
    static int MaxChannelDiff(const QImage& a, const QImage& b, QPoint* pWhere = nullptr)
    {
        if (a.size() != b.size() || a.format() != b.format() || a.format() != QImage::Format_ARGB32_Premultiplied)
            return 256;
        int iMax = 0;
        for (int iY = 0; iY < a.height(); ++iY)
        {
            const uint32_t* pA = reinterpret_cast<const uint32_t*>(a.constScanLine(iY));
            const uint32_t* pB = reinterpret_cast<const uint32_t*>(b.constScanLine(iY));
            for (int iX = 0; iX < a.width(); ++iX)
            {
                for (int iShift = 0; iShift < 32; iShift += 8)
                {
                    const int iDiff = std::abs(int((pA[iX] >> iShift) & 0xFF) - int((pB[iX] >> iShift) & 0xFF));
                    if (iDiff > iMax)
                    {
                        iMax = iDiff;
                        if (pWhere)
                            *pWhere = QPoint(iX, iY);
                    }
                }
            }
        }
        return iMax;
    }

    // 같은 목록을 GPU(화면 밖)와 CPU 로 그려 비교한다. 최대 차이를 돌려준다(GPU 실패면 -1)
    int CompareWithCpu(VulkanOffscreen& offscreen, const DrawList& list, GlyphCache& cache, const QSize& size,
                       const char* label)
    {
        const QImage imgGpu = offscreen.Render(list, cache, size);
        if (imgGpu.isNull())
            return -1;
        const QImage imgCpu = RenderCpu(list, cache, size);
        QPoint where;
        const int iDiff = MaxChannelDiff(imgGpu, imgCpu, &where);
        qInfo("%s: %d quads, %dx%d, max channel diff %d at (%d,%d) gpu %08x cpu %08x", label, int(list.vecQuads.size()),
              size.width(), size.height(), iDiff, where.x(), where.y(), imgGpu.pixel(where), imgCpu.pixel(where));
        return iDiff;
    }

    static int CountKind(const DrawList& list, EQuadKind kind)
    {
        return int(std::count_if(list.vecQuads.begin(), list.vecQuads.end(), [kind](const DrawQuad& quad) { return quad.eKind == kind; }));
    }

    // 컬러 이모지 글꼴이 있는지(GlyphCacheTest::emojiGoesToColorPage 와 같은 판정). 없으면(예: offscreen QPA) 이모지가 흑백 페이지로 가서
    // 컬러 글리프 쿼드도, 컬러 페이지에 올릴 것도 없다. 시험할 캐시를 건드리지 않게 따로 만든 캐시로 본다.
    static bool HasColorEmoji(qreal devicePixelRatio)
    {
        GlyphCache probe(BaseFont(), devicePixelRatio);
        const GlyphEntry entry = probe.GetGlyph(U"\U0001F600", false, false, 2);
        return !entry.bEmpty && entry.ePage == EAtlasPage::COLOR;
    }

private slots:
    void initTestCase()
    {
        TestFonts::Register();
        m_iValidationBefore = VulkanContext::GetValidationMessageCount();
        m_upContext = VulkanContext::Create(&m_strCreateError);
        if (m_upContext)
            qInfo("Vulkan device: %s (%s, validation %s)", qPrintable(m_upContext->GetDeviceName()),
                  qPrintable(m_upContext->GetLibraryUsed()), m_upContext->IsValidationEnabled() ? "on" : "off");
        else
            qInfo("Vulkan unavailable: %s", qPrintable(m_strCreateError));
    }

    void cleanupTestCase()
    {
        m_upContext.reset();
        // 검증 레이어를 켠 실행(TERMINALVIEWER_VK_VALIDATION=1)에서 경고·오류가 하나도 없어야 한다(Review Focus 4)
        QCOMPARE(VulkanContext::GetValidationMessageCount(), m_iValidationBefore);
    }

    void deviceAndPipeline()
    {
        if (!m_upContext)
            QSKIP(qPrintable(QStringLiteral("Vulkan 장치 없음: ") + m_strCreateError));
        QVERIFY(m_upContext->GetInstance() != VK_NULL_HANDLE);
        QVERIFY(m_upContext->GetDevice() != VK_NULL_HANDLE);
        QVERIFY(m_upContext->GetQueue() != VK_NULL_HANDLE);
        QVERIFY(m_upContext->GetAllocator() != nullptr);
        QVERIFY(m_upContext->GetPipelineLayout() != VK_NULL_HANDLE);
        QVERIFY(m_upContext->GetDescriptorSetLayout() != VK_NULL_HANDLE);
        const VkPipeline pipeline = m_upContext->GetPipeline(VK_FORMAT_B8G8R8A8_UNORM);
        QVERIFY(pipeline != VK_NULL_HANDLE);
        QCOMPARE(m_upContext->GetPipeline(VK_FORMAT_B8G8R8A8_UNORM), pipeline);   // 포맷마다 한 번만 만든다
    }

    void emptyListMatchesCpuExactly()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        GlyphCache cache(BaseFont(), 1.0);
        DrawList list;
        list.clearColor = TerminalRgb{10, 20, 30};
        list.iCols = 10;
        list.iRows = 4;
        list.iCellWidth = 8;
        list.iCellHeight = 17;
        list.iPaddingPx = 4;
        const QSize size(93, 79);
        VulkanOffscreen offscreen(*m_upContext);
        const QImage imgGpu = offscreen.Render(list, cache, size);
        QVERIFY(!imgGpu.isNull());
        QCOMPARE(imgGpu.format(), QImage::Format_ARGB32_Premultiplied);
        QCOMPARE(imgGpu.size(), size);
        const QImage imgCpu = RenderCpu(list, cache, size);
        QCOMPARE(MaxChannelDiff(imgGpu, imgCpu), 0);
        QCOMPARE(imgGpu, imgCpu);
        QCOMPARE(imgGpu.pixel(0, 0), qRgb(10, 20, 30));
        m_upContext->ReleaseAtlasTexture(&cache);
    }

    void mixedMatchesCpu()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        bool bNoColorEmoji = false;
        for (const qreal dRatio : {1.0, 2.0})
        {
            GlyphCache cache(BaseFont(), dRatio);
            FakeFrameSource source(40, 9, 4);
            FillMixed(source);
            DrawList list;
            QVERIFY(Build(source, cache, 0, list));
            // MIXED 가 세 종류 쿼드를 모두 만든다(사각형, 흑백 글리프, 컬러 이모지)
            QVERIFY(CountKind(list, EQuadKind::RECT) > 0);
            QVERIFY(CountKind(list, EQuadKind::GLYPH_MONO) > 0);
            // 컬러 이모지 글꼴이 없으면 컬러 쿼드가 없다: 그것만 빼고 GPU 와 CPU 비교는 그대로 한 뒤 끝에서 건너뛴다
            if (HasColorEmoji(dRatio))
                QVERIFY(CountKind(list, EQuadKind::GLYPH_COLOR) > 0);
            else
                bNoColorEmoji = true;
            VulkanOffscreen offscreen(*m_upContext);
            const int iDiff = CompareWithCpu(offscreen, list, cache, TargetSize(list), dRatio > 1.0 ? "MIXED x2" : "MIXED x1");
            QVERIFY2(iDiff >= 0, "GPU 그리기 실패");
            QVERIFY2(iDiff <= kMaxChannelDiff, qPrintable(QStringLiteral("max diff %1").arg(iDiff)));
            m_upContext->ReleaseAtlasTexture(&cache);
        }
        if (bNoColorEmoji)
            QSKIP("no color emoji font (GPU and CPU were compared without color glyph quads)");
    }

    // 최소 대비를 켠 목록(고친 글자색)도 GPU 와 CPU 가 같다. 두 경로는 같은 DrawList 색을 쓴다
    void minContrastMatchesCpu()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        GlyphCache cache(BaseFont(), 1.0);
        FakeFrameSource source(40, 9, 4);
        FillMixed(source);
        TerminalCell lowContrast;
        lowContrast.fg = TerminalRgb{20, 30, 120};   // FRAME_BG 위 어두운 파랑
        // 줄 4 + 8: 스크롤백 4줄 아래 뷰포트의 마지막 줄(8)
        source.SetText(4 + 8, 0, U"low contrast text", lowContrast);
        lowContrast.bFaint = true;
        source.SetText(4 + 8, 20, U"faint", lowContrast);
        TerminalTheme theme = Theme();
        theme.dMinContrastRatio = 4.5;
        DrawList listOff;
        QVERIFY(Build(source, cache, 0, listOff));
        DrawList listOn;
        QVERIFY(Build(source, cache, 0, listOn, theme));
        QCOMPARE(listOn.vecQuads.size(), listOff.vecQuads.size());
        int iChanged = 0;
        for (size_t uQuad = 0; uQuad < listOn.vecQuads.size(); ++uQuad)
            iChanged += listOn.vecQuads[uQuad].uColor != listOff.vecQuads[uQuad].uColor ? 1 : 0;
        QVERIFY2(iChanged >= 17, qPrintable(QString::number(iChanged)));   // 고친 글자가 있다(적어도 위 줄의 글자들)
        VulkanOffscreen offscreen(*m_upContext);
        const int iDiff = CompareWithCpu(offscreen, listOn, cache, TargetSize(listOn), "MIXED min contrast 4.5");
        QVERIFY2(iDiff >= 0, "GPU render failed");
        QVERIFY2(iDiff <= kMaxChannelDiff, qPrintable(QStringLiteral("max diff %1").arg(iDiff)));
        m_upContext->ReleaseAtlasTexture(&cache);
    }

    void scrollOffsetMatchesCpu()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        GlyphCache cache(BaseFont(), 1.0);
        FakeFrameSource source(40, 9, 4);
        FillMixed(source);
        DrawList list;
        QVERIFY(Build(source, cache, 7, list));
        QCOMPARE(list.iScrollOffsetPx, 7);
        VulkanOffscreen offscreen(*m_upContext);
        const int iDiff = CompareWithCpu(offscreen, list, cache, TargetSize(list), "MIXED scroll 7px");
        QVERIFY2(iDiff >= 0, "GPU 그리기 실패");
        QVERIFY2(iDiff <= kMaxChannelDiff, qPrintable(QStringLiteral("max diff %1").arg(iDiff)));

        // 같은 오프스크린으로 다른 오프셋을 다시 그려도(바뀐 아틀라스 없음) 맞는다
        DrawList again;
        QVERIFY(Build(source, cache, 0, again));
        const int iDiffAgain = CompareWithCpu(offscreen, again, cache, TargetSize(again), "MIXED scroll 0 again");
        QVERIFY(iDiffAgain >= 0 && iDiffAgain <= kMaxChannelDiff);
        m_upContext->ReleaseAtlasTexture(&cache);
    }

    // Review Focus 3: 아틀라스가 커지고(64 → 128), 꽉 차서 비워진(Reset) 뒤 다시 만든 목록도 CPU 와 같다
    void atlasGrowAndResetMatchesCpu()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        GlyphCache cache(BaseFont(), 1.0, 64, 128);
        const GlyphAtlas& mono = cache.GetAtlas(EAtlasPage::MONO);
        VulkanOffscreen offscreen(*m_upContext);
        FakeFrameSource source(24, 4, 0);
        source.SetColors(FRAME_BG, FRAME_FG);
        TerminalCell style;
        style.fg = FRAME_FG;

        // 1) 몇 글자: 64 에 들어간다
        source.SetText(0, 0, U"abc", style);
        DrawList first;
        QVERIFY(Build(source, cache, 0, first));
        QCOMPARE(mono.GetSize(), 64);
        int iDiff = CompareWithCpu(offscreen, first, cache, TargetSize(first), "atlas 64");
        QVERIFY(iDiff >= 0 && iDiff <= kMaxChannelDiff);

        // 2) 글자를 많이: 128 로 커진다(비우지 않음)
        const std::u32string strAscii = U"ABCDEFGHIJKLMNOPQRSTUVWXdefghijklmnopqrstuvwxyz0123456789!@#$%^&*()[]{}<>?/";
        for (int iRow = 0; iRow < 4; ++iRow)
            source.SetText(iRow, 0, strAscii.substr(size_t(iRow) * 19, 19), style);
        DrawList grown;
        QVERIFY(Build(source, cache, 0, grown));
        QCOMPARE(mono.GetSize(), 128);
        QCOMPARE(cache.GetGeneration(), 0u);
        iDiff = CompareWithCpu(offscreen, grown, cache, TargetSize(grown), "atlas grown 128");
        QVERIFY(iDiff >= 0 && iDiff <= kMaxChannelDiff);

        // 3) 다른 넓은 글자를 많이: 꽉 차서 비워지고(세대 증가) 다시 만든 목록
        const std::u32string strWide = U"가나다라마바사아자차카타파하거너더러머버서어저처커터퍼허고노도로모보소오조초코토포호";
        for (int iRow = 0; iRow < 4; ++iRow)
            source.SetText(iRow, 0, strWide.substr(size_t(iRow) * 11, 11), style);
        DrawList reset;
        QVERIFY(Build(source, cache, 0, reset));
        QVERIFY2(cache.GetGeneration() > 0u, "아틀라스가 비워지지 않았다");
        QCOMPARE(mono.GetSize(), 128);
        iDiff = CompareWithCpu(offscreen, reset, cache, TargetSize(reset), "atlas after reset");
        QVERIFY(iDiff >= 0 && iDiff <= kMaxChannelDiff);
        m_upContext->ReleaseAtlasTexture(&cache);
    }

    // 바뀐 영역을 다른 장치(예: 장치를 잃고 다시 만든 VulkanContext)가 이미 가져간 GlyphCache 도
    // 처음 쓰는 텍스처는 아틀라스 전체를 올린다
    void secondContextUploadsWholeAtlas()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        QString strError;
        std::unique_ptr<VulkanContext> upOther = VulkanContext::Create(&strError);
        QVERIFY2(upOther, qPrintable(strError));
        GlyphCache cache(BaseFont(), 1.0);
        FakeFrameSource source(40, 9, 4);
        FillMixed(source);
        DrawList list;
        QVERIFY(Build(source, cache, 0, list));
        {
            VulkanOffscreen first(*m_upContext);
            const int iDiff = CompareWithCpu(first, list, cache, TargetSize(list), "first context");
            QVERIFY(iDiff >= 0 && iDiff <= kMaxChannelDiff);
        }
        {
            VulkanOffscreen second(*upOther);
            const int iDiff = CompareWithCpu(second, list, cache, TargetSize(list), "second context, rects already taken");
            QVERIFY(iDiff >= 0 && iDiff <= kMaxChannelDiff);
        }
        m_upContext->ReleaseAtlasTexture(&cache);
    }

    // ReleaseAtlasTexture 를 잊은 텍스처도, 같은 주소에 새로 만든 GlyphCache 를 만나면 처음부터 올린다
    // (그 새 아틀라스의 바뀐 영역을 다른 장치가 먼저 가져갔어도)
    void newCacheAtSameAddressUploadsFresh()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        QString strError;
        std::unique_ptr<VulkanContext> upOther = VulkanContext::Create(&strError);
        QVERIFY2(upOther, qPrintable(strError));
        PlacedGlyphCache placed;
        TerminalCell style;
        style.fg = FRAME_FG;

        GlyphCache& first = placed.Emplace(BaseFont(), 1.0, 256);
        const void* pFirstAddress = &first;
        FakeFrameSource sourceA(20, 2, 0);
        sourceA.SetText(0, 0, U"first cache AAAA", style);
        DrawList listA;
        QVERIFY(Build(sourceA, first, 0, listA));
        {
            VulkanOffscreen offscreen(*m_upContext);
            const int iDiff = CompareWithCpu(offscreen, listA, first, TargetSize(listA), "first cache");
            QVERIFY(iDiff >= 0 && iDiff <= kMaxChannelDiff);
        }

        GlyphCache& second = placed.Emplace(BaseFont(), 1.0, 256);   // 앞의 것을 지우고 같은 주소에. ReleaseAtlasTexture 를 부르지 않는다
        QCOMPARE(static_cast<const void*>(&second), pFirstAddress);
        FakeFrameSource sourceB(20, 2, 0);
        sourceB.SetText(0, 0, U"zzzz second #####", style);
        DrawList listB;
        QVERIFY(Build(sourceB, second, 0, listB));
        {
            VulkanOffscreen other(*upOther);   // 새 아틀라스의 바뀐 영역을 먼저 가져간다
            const int iDiff = CompareWithCpu(other, listB, second, TargetSize(listB), "second cache, other context");
            QVERIFY(iDiff >= 0 && iDiff <= kMaxChannelDiff);
        }
        {
            VulkanOffscreen offscreen(*m_upContext);
            const int iDiff = CompareWithCpu(offscreen, listB, second, TargetSize(listB), "second cache, same address");
            QVERIFY(iDiff >= 0 && iDiff <= kMaxChannelDiff);
        }
        m_upContext->ReleaseAtlasTexture(&second);
    }

    // 바뀐 영역만 올리는 경로: 커지지도 비워지지도 않은 아틀라스에 새 글자를 더하면 두 페이지 모두 바뀐 영역만 올린다
    void partialUploadMatchesCpu()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        // 컬러 이모지 글꼴이 없으면 이모지도 흑백 페이지로 가서 컬러 페이지는 바뀐 영역이 없다: 컬러 페이지의 바뀐 영역 검사만 빼고 끝에서 건너뛴다
        const bool bColorEmoji = HasColorEmoji(1.0);
        GlyphCache cache(BaseFont(), 1.0, 512, 512);
        VulkanOffscreen offscreen(*m_upContext);
        FakeFrameSource source(32, 3, 0);
        source.SetColors(FRAME_BG, FRAME_FG);
        TerminalCell style;
        style.fg = FRAME_FG;

        source.SetText(0, 0, U"abc 가나 \U0001F600", style);
        DrawList first;
        QVERIFY(Build(source, cache, 0, first));
        int iDiff = CompareWithCpu(offscreen, first, cache, TargetSize(first), "partial: first (full upload)");
        QVERIFY(iDiff >= 0 && iDiff <= kMaxChannelDiff);
        VulkanAtlasTexture& texture = m_upContext->GetAtlasTexture(&cache);
        const AtlasUploadStats monoBefore = texture.GetUploadStats(EAtlasPage::MONO);
        const AtlasUploadStats colorBefore = texture.GetUploadStats(EAtlasPage::COLOR);
        QCOMPARE(monoBefore.iFullUploads, 1);
        QCOMPARE(colorBefore.iFullUploads, 1);
        const uint32_t uGeneration = cache.GetGeneration();

        // 새 ASCII·한글(흑백 페이지)과 새 이모지(컬러 페이지)
        source.SetText(1, 0, U"XYZ#% 다라마 \U0001F680\U0001F431", style);
        source.SetText(2, 0, U"qwerty 바사", style);
        DrawList second;
        QVERIFY(Build(source, cache, 0, second));
        QCOMPARE(cache.GetAtlas(EAtlasPage::MONO).GetSize(), 512);
        QCOMPARE(cache.GetAtlas(EAtlasPage::COLOR).GetSize(), 512);
        QCOMPARE(cache.GetGeneration(), uGeneration);
        iDiff = CompareWithCpu(offscreen, second, cache, TargetSize(second), "partial: new glyphs");
        QVERIFY(iDiff >= 0 && iDiff <= kMaxChannelDiff);

        const AtlasUploadStats monoAfter = texture.GetUploadStats(EAtlasPage::MONO);
        const AtlasUploadStats colorAfter = texture.GetUploadStats(EAtlasPage::COLOR);
        QCOMPARE(monoAfter.iFullUploads, monoBefore.iFullUploads);   // 전체를 다시 올리지 않았다
        QCOMPARE(colorAfter.iFullUploads, colorBefore.iFullUploads);
        QCOMPARE(monoAfter.iPartialUploads, monoBefore.iPartialUploads + 1);
        if (bColorEmoji)
            QCOMPARE(colorAfter.iPartialUploads, colorBefore.iPartialUploads + 1);
        QVERIFY(monoAfter.iPartialRects - monoBefore.iPartialRects >= 2);   // 여러 영역을 스테이징 하나에 이어 붙인다
        if (bColorEmoji)
            QVERIFY(colorAfter.iPartialRects - colorBefore.iPartialRects >= 2);
        m_upContext->ReleaseAtlasTexture(&cache);
        if (!bColorEmoji)
            QSKIP("no color emoji font (the color page partial upload was not checked)");
    }

    // 올리기를 기록한 명령 버퍼를 제출하지 못하면(바뀐 영역은 이미 꺼냈다) MarkForFullUpload 로 다음에 전체를 올린다
    void lostSubmissionRecoversWithFullUpload()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        GlyphCache cache(BaseFont(), 1.0, 512, 512);
        VulkanOffscreen offscreen(*m_upContext);
        FakeFrameSource source(32, 3, 0);
        source.SetColors(FRAME_BG, FRAME_FG);
        TerminalCell style;
        style.fg = FRAME_FG;
        source.SetText(0, 0, U"first line \U0001F600", style);
        DrawList first;
        QVERIFY(Build(source, cache, 0, first));
        int iDiff = CompareWithCpu(offscreen, first, cache, TargetSize(first), "lost: first");
        QVERIFY(iDiff >= 0 && iDiff <= kMaxChannelDiff);

        source.SetText(1, 0, U"lost glyphs QWJK 바사 \U0001F680", style);
        DrawList second;
        QVERIFY(Build(source, cache, 0, second));
        VulkanAtlasTexture& texture = m_upContext->GetAtlasTexture(&cache);
        {
            UnsubmittedCommands lost(*m_upContext);
            QVERIFY(lost.Get() != VK_NULL_HANDLE);
            texture.RecordUpload(lost.Get(), cache);   // 바뀐 영역을 꺼내 기록했지만
            texture.MarkForFullUpload();               // 제출에 실패했다
        }
        const AtlasUploadStats monoBefore = texture.GetUploadStats(EAtlasPage::MONO);
        const AtlasUploadStats colorBefore = texture.GetUploadStats(EAtlasPage::COLOR);
        iDiff = CompareWithCpu(offscreen, second, cache, TargetSize(second), "lost: after failed submit");
        QVERIFY(iDiff >= 0 && iDiff <= kMaxChannelDiff);
        QCOMPARE(texture.GetUploadStats(EAtlasPage::MONO).iFullUploads, monoBefore.iFullUploads + 1);
        QCOMPARE(texture.GetUploadStats(EAtlasPage::COLOR).iFullUploads, colorBefore.iFullUploads + 1);
        m_upContext->ReleaseAtlasTexture(&cache);
    }

    // TERMINALVIEWER_VK_VALIDATION=1 이어도 인스턴스에 검증 레이어를 켜지 못했으면 "검증 꺼짐" 으로 알린다
    void validationReportsOffWhenLayerMissing()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        QTemporaryDir emptyLayerDir;
        QVERIFY(emptyLayerDir.isValid());
        ScopedEnv validation("TERMINALVIEWER_VK_VALIDATION", "1");
        ScopedEnv layerPath("VK_LAYER_PATH", QFile::encodeName(emptyLayerDir.path()));   // 검증 레이어 매니페스트를 못 찾게 한다
        QString strError;
        std::unique_ptr<VulkanContext> upContext;
        int iLayerMissing = 0;
        int iLoaderMissing = 0;
        {
            ValidationOffWarnings warnings;
            upContext = VulkanContext::Create(&strError);
            iLayerMissing = warnings.GetLayerMissingCount();
            iLoaderMissing = warnings.GetLoaderMissingCount();
        }
        QVERIFY2(upContext, qPrintable(strError));
        QVERIFY(!upContext->IsValidationEnabled());
        QCOMPARE(iLayerMissing + iLoaderMissing, 1);   // 둘 중 하나가 한 번
    }

    // Review Focus 4: 화면 밖 그리기 직후 장치를 지워도 크래시·검증 오류가 없다
    void destroyContextRightAfterRender()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        QString strError;
        std::unique_ptr<VulkanContext> upContext = VulkanContext::Create(&strError);
        QVERIFY2(upContext, qPrintable(strError));
        GlyphCache cache(BaseFont(), 1.0);
        FakeFrameSource source(40, 9, 4);
        FillMixed(source);
        DrawList list;
        QVERIFY(Build(source, cache, 0, list));
        {
            VulkanOffscreen offscreen(*upContext);
            QVERIFY(!offscreen.Render(list, cache, TargetSize(list)).isNull());
            QVERIFY(!offscreen.Render(list, cache, TargetSize(list) + QSize(10, 10)).isNull());
        }
        upContext.reset();   // 아틀라스 텍스처·파이프라인·VMA·장치·인스턴스를 역순으로 지운다
        QCOMPARE(VulkanContext::GetValidationMessageCount(), m_iValidationBefore);
    }

    void rendererSwitchesToGpu()
    {
        if (!m_upContext)
            QSKIP("Vulkan 장치 없음");
        TerminalRenderer renderer;
        QCOMPARE(renderer.GetStrategy(), ETerminalRenderStrategy::CPU);
        renderer.SetStrategy(ETerminalRenderStrategy::GPU);
        QCOMPARE(renderer.GetStrategy(), ETerminalRenderStrategy::GPU);
        // 빈 핸들(surface 를 만들 수 없음)은 GPU Strategy 가 받지 않는다
        QVERIFY(!renderer.AttachView(1, {}, QSize(100, 100)));
        renderer.SetStrategy(ETerminalRenderStrategy::CPU);
        QCOMPARE(renderer.GetStrategy(), ETerminalRenderStrategy::CPU);
        QVERIFY(renderer.AttachView(1, {}, QSize(100, 100)));
    }

    void rendererFallsBackWhenVulkanDisabled()
    {
        ScopedEnv disable("TERMINALVIEWER_VK_DISABLE", "1");
        TerminalRenderer renderer;
        QTest::ignoreMessage(QtWarningMsg, GpuUnavailableWarning());
        renderer.SetStrategy(ETerminalRenderStrategy::GPU);
        QCOMPARE(renderer.GetStrategy(), ETerminalRenderStrategy::CPU);
        QVERIFY(!renderer.GetGpuError().isEmpty());
    }

    void disabledByEnvironment()
    {
        ScopedEnv disable("TERMINALVIEWER_VK_DISABLE", "1");
        QString strLibrary;
        bool bValidation = true;
        QVERIFY(LoadVulkanLibrary(&strLibrary, &bValidation) == nullptr);
        QVERIFY(!bValidation);
        QVERIFY(strLibrary.contains(QStringLiteral("TERMINALVIEWER_VK_DISABLE")));

        QString strError;
        std::unique_ptr<VulkanContext> upContext = VulkanContext::Create(&strError);
        QVERIFY(!upContext);
        QVERIFY(!strError.isEmpty());
        QVERIFY(strError.contains(QStringLiteral("TERMINALVIEWER_VK_DISABLE")));
    }

    // AUTO 가 GPU 를 시도하기 전에 CPU 로 알릴지 정하는 검사(GUI 스레드에서 부른다): TERMINALVIEWER_VK_DISABLE=1 이면 거짓,
    // 아니면 로더가 라이브러리를 열 수 있는지와 같다(라이브러리를 열고 닫기만 한다)
    void gpuLibraryProbeFollowsLoader()
    {
        {
            ScopedEnv disable("TERMINALVIEWER_VK_DISABLE", "1");
            QVERIFY(!IsGpuLibraryAvailable());
        }
        QString strLibrary;
        bool bValidation = false;
        QCOMPARE(IsGpuLibraryAvailable(), LoadVulkanLibrary(&strLibrary, &bValidation) != nullptr);
    }
};

#else   // TERMINALVIEWER_GPU

// GPU 를 끈 빌드(-DTERMINALVIEWER_GPU=OFF): Vulkan 코드가 빠진다. GPU 를 골라도 CPU 로 남는다.
class VulkanRendererTest : public QObject
{
    Q_OBJECT

private slots:
    void gpuNotBuilt()
    {
        TerminalRenderer renderer;
        QTest::ignoreMessage(QtWarningMsg, GpuUnavailableWarning());
        renderer.SetStrategy(ETerminalRenderStrategy::GPU);
        QCOMPARE(renderer.GetStrategy(), ETerminalRenderStrategy::CPU);
        QVERIFY(!renderer.GetGpuError().isEmpty());
    }

    void gpuLibraryProbeIsOffWithoutGpu()
    {
        QVERIFY(!IsGpuLibraryAvailable());   // AUTO 는 처음부터 CPU 로 알린다
    }
};

#endif  // TERMINALVIEWER_GPU

QObject* CreateVulkanRendererTest() { return new VulkanRendererTest(); }

#include "VulkanRendererTest.moc"
