#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/BoxGlyphPainter.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphAtlas.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphRasterizer.h"

#include <QFont>
#include <QRect>

#include <cstdint>
#include <map>
#include <string>
#include <tuple>

enum class EAtlasPage : uint8_t { MONO = 0, COLOR = 1 };

struct GlyphEntry
{
    bool bEmpty = true;       // 그릴 것 없음(공백 등)
    EAtlasPage ePage = EAtlasPage::MONO;
    QRect atlasRect;          // 아틀라스 좌표
    QPoint offset;            // RasterGlyph::offset 과 같은 뜻
};

class GlyphCache
{
public:
    GlyphCache(const QFont& font, qreal devicePixelRatio,
               int atlasInitialSize = GlyphAtlas::kDefaultInitialSize, int atlasMaxSize = GlyphAtlas::kDefaultMaxSize);

public:
    // 없으면 만든다. 박스·블록(1칸, 굵게·기울임 무시)은 BoxGlyphPainter, 나머지는 GlyphRasterizer.
    // 아틀라스가 꽉 차면 두 페이지를 모두 Reset 하고 캐시를 비운 뒤 넣는다(GetGeneration 증가).
    GlyphEntry GetGlyph(const std::u32string& text, bool bold, bool italic, int cellSpan);
    GlyphEntry GetUnderlineSprite(EUnderlineSprite sprite);

public:
    const CellMetrics& GetMetrics() const;
    qreal GetDevicePixelRatio() const;
    GlyphAtlas& GetAtlas(EAtlasPage page);
    const GlyphAtlas& GetAtlas(EAtlasPage page) const;
    uint32_t GetGeneration() const;   // 어느 페이지든 Reset 되면 증가
    int GetRasterizeCount() const;    // 테스트용: 실제 래스터화 횟수

private:
    using GlyphKey = std::tuple<std::u32string, bool, bool, int>;

    GlyphEntry Store(const QImage& image, bool bColor, const QPoint& offset);

    int m_iAtlasInitialSize;
    int m_iAtlasMaxSize;
    QFont m_font;
    CellMetrics m_metrics;
    GlyphRasterizer m_rasterizer;
    GlyphAtlas m_monoAtlas;
    GlyphAtlas m_colorAtlas;
    std::map<GlyphKey, GlyphEntry> m_mapGlyphs;
    std::map<EUnderlineSprite, GlyphEntry> m_mapUnderlines;
    uint32_t m_uiGeneration = 0;
    int m_iRasterizeCount = 0;
};
