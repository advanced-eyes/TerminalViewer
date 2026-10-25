#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawListBuilder.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalContrast.h"
#include "TerminalViewer/Testing/FakeFrameSource.h"
#include "TestFonts.h"

#include <QTest>

#include <algorithm>
#include <iterator>
#include <limits>
#include <random>
#include <set>
#include <utility>

namespace
{
constexpr TerminalRgb RED{255, 0, 0};
constexpr TerminalRgb GREEN{0, 255, 0};
constexpr TerminalRgb BLUE{0, 0, 255};
constexpr TerminalRgb FRAME_BG{10, 20, 30};
constexpr TerminalRgb FRAME_FG{200, 210, 220};
constexpr TerminalRgb SENTINEL_BG{77, 88, 99};   // bBgDefault 칸의 cell.bg: frame.background 와 달라야 구분된다
constexpr TerminalRgb DARK_BLUE{0, 0, 160};      // FRAME_BG 위 대비 1.33
constexpr double MIN_CONTRAST = 4.5;
}

class DrawListBuilderTest : public QObject
{
    Q_OBJECT

private:
    static QFont BaseFont()
    {
        QFont font(QStringLiteral("Cascadia Mono"));
        font.setPixelSize(14);
        return font;
    }

    static TerminalTheme Theme()
    {
        TerminalTheme theme;
        theme.selectionBg = TerminalRgb{50, 60, 70};
        theme.cursorColor = TerminalRgb{250, 250, 0};
        theme.iPaddingLogicalPx = 4;
        return theme;
    }

    static TerminalFrame MakeFrame(int cols, int rows)
    {
        TerminalFrame frame;
        frame.iCols = cols;
        frame.iRows = rows;
        frame.eDirty = EFrameDirty::FULL;
        frame.background = FRAME_BG;
        frame.foreground = FRAME_FG;
        for (int iRow = 0; iRow < rows; ++iRow)
        {
            TerminalRow row;
            row.llId = uint64_t(iRow + 1);
            row.iViewportY = iRow;
            row.bDirty = true;
            row.vecCells.resize(size_t(cols));
            for (TerminalCell& cell : row.vecCells)
            {
                cell.fg = FRAME_FG;
                cell.bg = SENTINEL_BG;
            }
            frame.vecRows.push_back(row);
        }
        return frame;
    }

    // 모든 행이 깨끗하고 cells 가 빈 PARTIAL 프레임
    static TerminalFrame MakePartial(TerminalFrame frame)
    {
        frame.eDirty = EFrameDirty::PARTIAL;
        for (TerminalRow& row : frame.vecRows)
        {
            row.bDirty = false;
            row.vecCells.clear();
            row.vecGraphemes.clear();
        }
        return frame;
    }

    static TerminalCell& Cell(TerminalFrame& frame, int row, int col) { return frame.vecRows[size_t(row)].vecCells[size_t(col)]; }

    static bool BuildOk(DrawList& list, const TerminalFrame& frame, GlyphCache& cache, bool focused = true,
                        bool blink = true, int scroll = 0)
    {
        DrawListBuilder builder;
        return BuildWith(list, builder, frame, cache, focused, blink, scroll);
    }

    static bool BuildWith(DrawList& list, DrawListBuilder& builder, const TerminalFrame& frame, GlyphCache& cache,
                          bool focused = true, bool blink = true, int scroll = 0)
    {
        DrawListBuildStats stats;
        return builder.Build(frame, Theme(), focused, blink, scroll, cache, list, stats) == EBuildResult::OK;
    }

    static std::vector<DrawQuad> Layer(const DrawList& list, int begin, int count)
    {
        return std::vector<DrawQuad>(list.vecQuads.begin() + begin, list.vecQuads.begin() + begin + count);
    }
    static std::vector<DrawQuad> Backgrounds(const DrawList& list) { return Layer(list, 0, list.iBackgroundCount); }
    static std::vector<DrawQuad> Glyphs(const DrawList& list)
    {
        return Layer(list, list.iBackgroundCount, list.iGlyphCount);
    }
    static std::vector<DrawQuad> Decorations(const DrawList& list)
    {
        return Layer(list, list.iBackgroundCount + list.iGlyphCount, list.iDecorationCount);
    }

    static bool SameQuad(const DrawQuad& a, const DrawQuad& b)
    {
        return a.iX == b.iX && a.iY == b.iY && a.uW == b.uW && a.uH == b.uH && a.uAtlasX == b.uAtlasX &&
               a.uAtlasY == b.uAtlasY && a.uColor == b.uColor && a.eKind == b.eKind && a.iRow == b.iRow;
    }
    static bool SameQuads(const DrawList& a, const DrawList& b)
    {
        if (a.vecQuads.size() != b.vecQuads.size())
            return false;
        for (size_t iIndex = 0; iIndex < a.vecQuads.size(); ++iIndex)
            if (!SameQuad(a.vecQuads[iIndex], b.vecQuads[iIndex]))
                return false;
        return a.iBackgroundCount == b.iBackgroundCount && a.iGlyphCount == b.iGlyphCount;
    }

    static TerminalTheme MakeContrastTheme(double ratio)
    {
        TerminalTheme theme = Theme();
        theme.dMinContrastRatio = ratio;
        return theme;
    }

    static bool BuildThemed(DrawList& list, const TerminalFrame& frame, GlyphCache& cache, const TerminalTheme& theme)
    {
        DrawListBuilder builder;
        DrawListBuildStats stats;
        return builder.Build(frame, theme, true, true, 0, cache, list, stats) == EBuildResult::OK;
    }

    static TerminalRgb MixHalf(const TerminalRgb& a, const TerminalRgb& b)
    {
        return TerminalRgb{uint8_t((a.r + b.r + 1) / 2), uint8_t((a.g + b.g + 1) / 2), uint8_t((a.b + b.b + 1) / 2)};
    }

    static TerminalRgb UnpackColor(uint32_t color)   // 불투명 색만(PackColor 의 반대)
    {
        return TerminalRgb{uint8_t(color & 0xFF), uint8_t((color >> 8) & 0xFF), uint8_t((color >> 16) & 0xFF)};
    }

    // 글리프 쿼드의 가운데가 있는 칸(왼쪽 베어링이 음수여도 맞다)
    static int GetGlyphColumn(const DrawQuad& quad, int cellWidth) { return (quad.iX + quad.uW / 2) / cellWidth; }

    // 그 칸의 글리프 쿼드. 없으면 nullptr
    static const DrawQuad* FindGlyphAt(const std::vector<DrawQuad>& quads, int col, int cellWidth)
    {
        for (const DrawQuad& quad : quads)
            if (quad.eKind != EQuadKind::RECT && GetGlyphColumn(quad, cellWidth) == col)
                return &quad;
        return nullptr;
    }

    // 그 칸의 취소선 사각형. 없으면 nullptr
    static const DrawQuad* FindStrikeAt(const std::vector<DrawQuad>& quads, int col, const CellMetrics& metrics)
    {
        for (const DrawQuad& quad : quads)
            if (quad.eKind == EQuadKind::RECT && quad.iY == metrics.iStrikeY && quad.iX == col * metrics.iCellWidth &&
                quad.uH == metrics.iStrikeThickness)
                return &quad;
        return nullptr;
    }

private slots:
    void initTestCase() { TestFonts::Register(); }

    void packColorPremultiplies()
    {
        QCOMPARE(PackColor(TerminalRgb{255, 0, 0}, 128), 0x80000080u);
        QCOMPARE(PackColor(TerminalRgb{1, 2, 3}), 0xFF030201u);
    }

    void quadLayout()
    {
        QCOMPARE(sizeof(DrawQuad), size_t(20));
    }

    void defaultBackgroundMakesNoRect()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(4, 2);
        DrawList list;
        QVERIFY(BuildOk(list, frame, cache));
        QCOMPARE(list.iBackgroundCount, 0);
    }

    void redBackgroundMakesRect()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const CellMetrics& metrics = cache.GetMetrics();
        TerminalFrame frame = MakeFrame(6, 1);
        Cell(frame, 0, 2).bBgDefault = false;
        Cell(frame, 0, 2).bg = RED;
        DrawList built;
        QVERIFY(BuildOk(built, frame, cache));
        const auto vecBg = Backgrounds(built);
        QCOMPARE(int(vecBg.size()), 1);
        QCOMPARE(int(vecBg[0].iX), 2 * metrics.iCellWidth);
        QCOMPARE(int(vecBg[0].iY), 0);
        QCOMPARE(int(vecBg[0].uW), metrics.iCellWidth);
        QCOMPARE(int(vecBg[0].uH), metrics.iCellHeight);
        QCOMPARE(vecBg[0].uColor, PackColor(RED));
        QCOMPARE(vecBg[0].eKind, EQuadKind::RECT);
    }

    void adjacentSameBackgroundMerges()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const CellMetrics& metrics = cache.GetMetrics();
        TerminalFrame frame = MakeFrame(8, 1);
        for (int iCol = 1; iCol <= 3; ++iCol)
        {
            Cell(frame, 0, iCol).bBgDefault = false;
            Cell(frame, 0, iCol).bg = GREEN;
        }
        Cell(frame, 0, 4).bBgDefault = false;
        Cell(frame, 0, 4).bg = BLUE;
        DrawList built;
        QVERIFY(BuildOk(built, frame, cache));
        const auto vecBg = Backgrounds(built);
        QCOMPARE(int(vecBg.size()), 2);
        QCOMPARE(int(vecBg[0].uW), 3 * metrics.iCellWidth);
        QCOMPARE(int(vecBg[1].iX), 4 * metrics.iCellWidth);
    }

    void inverseSwapsColors()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(3, 1);
        TerminalCell& cell = Cell(frame, 0, 0);
        cell.uCodepoint = 'A';
        cell.bInverse = true;
        cell.fg = RED;
        DrawList list;
        QVERIFY(BuildOk(list, frame, cache));
        const auto vecBg = Backgrounds(list);
        QCOMPARE(int(vecBg.size()), 1);
        QCOMPARE(vecBg[0].uColor, PackColor(RED));
        const auto vecGlyph = Glyphs(list);
        QCOMPARE(int(vecGlyph.size()), 1);
        QCOMPARE(vecGlyph[0].uColor, PackColor(FRAME_BG));   // 기본 배경이면 frame.background
    }

    void inverseUsesCellBackground()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(3, 1);
        TerminalCell& cell = Cell(frame, 0, 0);
        cell.uCodepoint = 'A';
        cell.bInverse = true;
        cell.fg = RED;
        cell.bg = BLUE;
        cell.bBgDefault = false;
        DrawList list;
        QVERIFY(BuildOk(list, frame, cache));
        QCOMPARE(Backgrounds(list)[0].uColor, PackColor(RED));
        QCOMPARE(Glyphs(list)[0].uColor, PackColor(BLUE));
    }

    void selectionUsesThemeColors()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(4, 1);
        Cell(frame, 0, 1).uCodepoint = 'A';
        Cell(frame, 0, 1).fg = GREEN;
        frame.vecRows[0].iSelectBegin = 1;
        frame.vecRows[0].iSelectEnd = 3;
        TerminalTheme theme = Theme();
        DrawListBuilder builder;
        DrawList list;
        DrawListBuildStats stats;
        QCOMPARE(builder.Build(frame, theme, true, true, 0, cache, list, stats), EBuildResult::OK);
        auto vecBg = Backgrounds(list);
        QCOMPARE(int(vecBg.size()), 1);
        QCOMPARE(int(vecBg[0].uW), 2 * cache.GetMetrics().iCellWidth);
        QCOMPARE(vecBg[0].uColor, PackColor(theme.selectionBg));
        QCOMPARE(Glyphs(list)[0].uColor, PackColor(GREEN));   // 선택 글자색 없음 -> 원래 색

        theme.optSelectionFg = BLUE;
        DrawListBuilder builder2;
        QCOMPARE(builder2.Build(frame, theme, true, true, 0, cache, list, stats), EBuildResult::OK);
        QCOMPARE(Glyphs(list)[0].uColor, PackColor(BLUE));
    }

    void faintMixesHalf()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(3, 1);
        TerminalCell& cell = Cell(frame, 0, 0);
        cell.uCodepoint = 'A';
        cell.bFaint = true;
        cell.fg = TerminalRgb{201, 100, 0};
        cell.bg = TerminalRgb{0, 51, 255};
        cell.bBgDefault = false;
        DrawList built;
        QVERIFY(BuildOk(built, frame, cache));
        const auto vecGlyph = Glyphs(built);
        QCOMPARE(vecGlyph[0].uColor, PackColor(TerminalRgb{101, 76, 128}));   // (201+0+1)/2, (100+51+1)/2, (0+255+1)/2
        // 기본 배경이면 frame.background 와 섞는다
        Cell(frame, 0, 2).uCodepoint = 'B';
        Cell(frame, 0, 2).bFaint = true;
        Cell(frame, 0, 2).fg = RED;
        QVERIFY(BuildOk(built, frame, cache));
        const auto vecGlyph2 = Glyphs(built);
        QCOMPARE(int(vecGlyph2.size()), 2);
        QCOMPARE(vecGlyph2[1].uColor, PackColor(TerminalRgb{133, 10, 15}));
    }

    void invisibleHasBackgroundNoGlyph()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(2, 1);
        TerminalCell& cell = Cell(frame, 0, 0);
        cell.uCodepoint = 'A';
        cell.bInvisible = true;
        cell.bBgDefault = false;
        cell.bg = RED;
        DrawList list;
        QVERIFY(BuildOk(list, frame, cache));
        QCOMPARE(list.iBackgroundCount, 1);
        QCOMPARE(list.iGlyphCount, 0);
    }

    void wideGlyphOccupiesTwoCells()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const int iW = cache.GetMetrics().iCellWidth;
        TerminalFrame frame = MakeFrame(8, 1);
        Cell(frame, 0, 3).uCodepoint = 0xD55C;
        Cell(frame, 0, 3).eWidth = ECellWidth::WIDE;
        Cell(frame, 0, 3).eUnderline = EUnderline::SINGLE;
        Cell(frame, 0, 4).eWidth = ECellWidth::SPACER_TAIL;
        Cell(frame, 0, 4).uCodepoint = 'Z';   // 꼬리 칸을 건너뛰지 않으면 글리프·밑줄이 나온다
        Cell(frame, 0, 4).eUnderline = EUnderline::SINGLE;
        DrawList built;
        QVERIFY(BuildOk(built, frame, cache));
        const auto vecGlyph = Glyphs(built);
        QCOMPARE(int(vecGlyph.size()), 1);
        QVERIFY(vecGlyph[0].iX >= 3 * iW);
        QVERIFY(vecGlyph[0].iX + vecGlyph[0].uW <= 5 * iW);
        const auto vecDeco = Decorations(built);
        QCOMPARE(int(vecDeco.size()), 1);
        QCOMPARE(int(vecDeco[0].uW), 2 * iW);
        QCOMPARE(int(vecDeco[0].iX), 3 * iW);
        const GlyphEntry span2 = cache.GetGlyph(U"\uD55C", false, false, 2);   // cellSpan 2 로 요청했는지
        QCOMPARE(int(vecGlyph[0].uW), span2.atlasRect.width());
        QCOMPARE(int(vecGlyph[0].uAtlasX), span2.atlasRect.left());
        QCOMPARE(int(vecGlyph[0].uAtlasY), span2.atlasRect.top());
        const GlyphEntry span1 = cache.GetGlyph(U"\uD55C", false, false, 1);
        QVERIFY(span1.atlasRect != span2.atlasRect);
    }

    void wideAtLastColumnIsClipped()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const int iW = cache.GetMetrics().iCellWidth;
        TerminalFrame frame = MakeFrame(4, 1);
        Cell(frame, 0, 3).uCodepoint = 0xD55C;
        Cell(frame, 0, 3).eWidth = ECellWidth::WIDE;
        Cell(frame, 0, 3).bBgDefault = false;
        Cell(frame, 0, 3).bg = RED;
        DrawList list;
        QVERIFY(BuildOk(list, frame, cache));
        for (const DrawQuad& quad : list.vecQuads)
            QVERIFY(quad.iX + quad.uW <= 4 * iW);
    }

    void layersAreOrdered()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const CellMetrics& metrics = cache.GetMetrics();
        TerminalFrame frame = MakeFrame(4, 2);
        TerminalCell& cell = Cell(frame, 1, 1);
        cell.uCodepoint = 'A';
        cell.bBgDefault = false;
        cell.bg = RED;
        cell.eUnderline = EUnderline::SINGLE;
        cell.bStrikethrough = true;
        frame.cursor = TerminalCursor{true, false, ECursorShape::BAR, 3, 0, false, std::nullopt};
        DrawList list;
        QVERIFY(BuildOk(list, frame, cache));
        QCOMPARE(list.iBackgroundCount, 1);
        QCOMPARE(list.iGlyphCount, 1);
        QCOMPARE(list.iDecorationCount, 3);
        QCOMPARE(int(list.vecQuads.size()), 5);
        QCOMPARE(list.vecQuads[0].eKind, EQuadKind::RECT);
        QCOMPARE(list.vecQuads[0].uColor, PackColor(RED));
        QCOMPARE(list.vecQuads[1].eKind, EQuadKind::GLYPH_MONO);
        QCOMPARE(int(list.vecQuads[2].iY), metrics.iUnderlineY);   // 행 장식
        QCOMPARE(int(list.vecQuads[3].iY), metrics.iStrikeY);
        QCOMPARE(int(list.vecQuads[4].iX), 3 * metrics.iCellWidth);   // 커서가 마지막
        QCOMPARE(int(list.vecQuads[4].iRow), 0);
    }

    void blockCursorFocused()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const CellMetrics& metrics = cache.GetMetrics();
        TerminalFrame frame = MakeFrame(5, 2);
        Cell(frame, 1, 2).uCodepoint = 'A';
        Cell(frame, 1, 2).fg = GREEN;
        Cell(frame, 1, 2).bBgDefault = false;
        Cell(frame, 1, 2).bg = BLUE;
        frame.cursor = TerminalCursor{true, false, ECursorShape::BLOCK, 2, 1, false, std::nullopt};
        const TerminalTheme theme = Theme();
        DrawList built;
        QVERIFY(BuildOk(built, frame, cache));
        const auto vecDeco = Decorations(built);
        QCOMPARE(int(vecDeco.size()), 2);
        QCOMPARE(vecDeco[0].eKind, EQuadKind::RECT);
        QCOMPARE(vecDeco[0].uColor, PackColor(theme.cursorColor));
        QCOMPARE(int(vecDeco[0].iX), 2 * metrics.iCellWidth);
        QCOMPARE(int(vecDeco[0].uW), metrics.iCellWidth);
        QCOMPARE(int(vecDeco[0].iRow), 1);
        QCOMPARE(vecDeco[1].eKind, EQuadKind::GLYPH_MONO);
        QCOMPARE(vecDeco[1].uColor, PackColor(BLUE));
    }

    void cursorOptColorOverridesTheme()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(5, 1);
        frame.cursor = TerminalCursor{true, false, ECursorShape::BLOCK, 0, 0, false, RED};
        DrawList built;
        QVERIFY(BuildOk(built, frame, cache));
        const auto vecDeco = Decorations(built);
        QCOMPARE(int(vecDeco.size()), 1);
        QCOMPARE(vecDeco[0].uColor, PackColor(RED));
    }

    void barAndUnderlineCursor()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const CellMetrics& metrics = cache.GetMetrics();
        TerminalFrame frame = MakeFrame(5, 1);
        frame.cursor = TerminalCursor{true, false, ECursorShape::BAR, 1, 0, false, std::nullopt};
        DrawList built;
        QVERIFY(BuildOk(built, frame, cache));
        auto vecDeco = Decorations(built);
        QCOMPARE(int(vecDeco.size()), 1);
        QCOMPARE(int(vecDeco[0].uW), metrics.iLineThickness);
        QCOMPARE(int(vecDeco[0].uH), metrics.iCellHeight);
        QCOMPARE(int(vecDeco[0].iX), metrics.iCellWidth);

        frame.cursor.eShape = ECursorShape::UNDERLINE;
        QVERIFY(BuildOk(built, frame, cache));
        vecDeco = Decorations(built);
        QCOMPARE(int(vecDeco.size()), 1);
        QCOMPARE(int(vecDeco[0].iY), metrics.iUnderlineY);
        QCOMPARE(int(vecDeco[0].uW), metrics.iCellWidth);
        QCOMPARE(int(vecDeco[0].uH), metrics.iUnderlineThickness);
    }

    void unfocusedCursorIsHollow()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(5, 1);
        frame.cursor = TerminalCursor{true, false, ECursorShape::BLOCK, 1, 0, false, std::nullopt};
        DrawList built;
        QVERIFY(BuildOk(built, frame, cache, false));
        const auto vecDeco = Decorations(built);
        QCOMPARE(int(vecDeco.size()), 4);
        for (const DrawQuad& quad : vecDeco)
            QCOMPARE(quad.eKind, EQuadKind::RECT);
    }

    void hiddenOrBlinkedOffCursorHasNoQuads()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(5, 1);
        frame.cursor = TerminalCursor{false, false, ECursorShape::BLOCK, 1, 0, false, std::nullopt};
        DrawList list;
        QVERIFY(BuildOk(list, frame, cache));
        QCOMPARE(list.iDecorationCount, 0);
        frame.cursor.bVisible = true;
        QVERIFY(BuildOk(list, frame, cache, true, false));
        QCOMPARE(list.iDecorationCount, 0);
        QVERIFY(BuildOk(list, frame, cache, true, true));
        QCOMPARE(list.iDecorationCount, 1);
    }

    void cursorOnWideCellIsTwoCellsWide()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const int iW = cache.GetMetrics().iCellWidth;
        TerminalFrame frame = MakeFrame(5, 1);
        Cell(frame, 0, 1).uCodepoint = 0xD55C;
        Cell(frame, 0, 1).eWidth = ECellWidth::WIDE;
        Cell(frame, 0, 2).eWidth = ECellWidth::SPACER_TAIL;
        frame.cursor = TerminalCursor{true, false, ECursorShape::BLOCK, 1, 0, true, std::nullopt};
        DrawList built;
        QVERIFY(BuildOk(built, frame, cache));
        const auto vecDeco = Decorations(built);
        QCOMPARE(int(vecDeco[0].uW), 2 * iW);
    }

    void preeditOverlay()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const CellMetrics& metrics = cache.GetMetrics();
        TerminalFrame frame = MakeFrame(8, 1);
        Cell(frame, 0, 2).uCodepoint = 'X';
        frame.cursor = TerminalCursor{true, false, ECursorShape::BLOCK, 2, 0, false, std::nullopt};
        TerminalPreedit preedit;
        TerminalCell cell;
        cell.uCodepoint = 0xD55C;
        cell.eWidth = ECellWidth::WIDE;
        TerminalCell tail;
        tail.eWidth = ECellWidth::SPACER_TAIL;
        preedit.vecCells = {cell, tail};
        preedit.iCaretCell = 2;
        frame.optPreedit = preedit;
        const TerminalTheme theme = Theme();
        DrawList built;
        QVERIFY(BuildOk(built, frame, cache));
        const auto vecDeco = Decorations(built);

        int iOpaque = 0, iGlyph = 0, iUnderline = 0, iBar = 0;
        for (const DrawQuad& quad : vecDeco)
        {
            if (quad.eKind != EQuadKind::RECT)
                ++iGlyph;
            else if (quad.uColor == PackColor(FRAME_BG) && quad.iX >= 2 * metrics.iCellWidth && quad.iX < 4 * metrics.iCellWidth)
                ++iOpaque;
            else if (quad.iY == metrics.iUnderlineY && quad.uW == 2 * metrics.iCellWidth && quad.iX == 2 * metrics.iCellWidth)
                ++iUnderline;
            else if (quad.iX == 4 * metrics.iCellWidth && quad.uW == metrics.iLineThickness && quad.uColor == PackColor(theme.cursorColor))
                ++iBar;
        }
        QVERIFY(iOpaque >= 1);
        QCOMPARE(iGlyph, 1);
        QCOMPARE(iUnderline, 1);
        QCOMPARE(iBar, 1);
    }

    void underlineDecorations()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const CellMetrics& metrics = cache.GetMetrics();
        TerminalFrame frame = MakeFrame(6, 1);
        Cell(frame, 0, 0).eUnderline = EUnderline::SINGLE;
        Cell(frame, 0, 0).underlineColor = RED;
        Cell(frame, 0, 1).eUnderline = EUnderline::DOUBLE;
        Cell(frame, 0, 2).eUnderline = EUnderline::CURLY;
        Cell(frame, 0, 3).eUnderline = EUnderline::DOTTED;
        Cell(frame, 0, 4).eUnderline = EUnderline::DASHED;
        Cell(frame, 0, 5).bStrikethrough = true;
        DrawList built;
        QVERIFY(BuildOk(built, frame, cache));
        const auto vecDeco = Decorations(built);
        QCOMPARE(int(vecDeco.size()), 1 + 2 + 3 + 1);
        QCOMPARE(int(vecDeco[0].iY), metrics.iUnderlineY);
        QCOMPARE(int(vecDeco[0].uH), metrics.iUnderlineThickness);
        QCOMPARE(vecDeco[0].uColor, PackColor(RED));
        QCOMPARE(vecDeco[1].eKind, EQuadKind::RECT);
        QCOMPARE(vecDeco[2].eKind, EQuadKind::RECT);
        QVERIFY(vecDeco[1].iY != vecDeco[2].iY);
        for (int iIndex = 3; iIndex < 6; ++iIndex)
            QCOMPARE(vecDeco[size_t(iIndex)].eKind, EQuadKind::GLYPH_MONO);
        QCOMPARE(int(vecDeco[6].iY), metrics.iStrikeY);
        QCOMPARE(int(vecDeco[6].uH), metrics.iStrikeThickness);
    }

    void rowCacheReusesCleanRows()
    {
        GlyphCache cache(BaseFont(), 1.0);
        FakeFrameSource source(40, 24, 0);
        source.FillPattern(EFakePattern::MIXED, 7);
        TerminalFrame full;
        source.CaptureFrame(TerminalCaptureRequest{0, 0, true}, full);
        DrawListBuilder builder;
        DrawList list;
        DrawListBuildStats stats;
        QCOMPARE(builder.Build(full, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        QCOMPARE(stats.iRebuiltRows, 24);
        QVERIFY(!builder.NeedsFullFrame());

        source.SetText(5, 0, U"changed", TerminalCell{});
        TerminalFrame partial;
        source.CaptureFrame(TerminalCaptureRequest{0, 0, false}, partial);
        QCOMPARE(partial.eDirty, EFrameDirty::PARTIAL);
        DrawList partialList;
        QCOMPARE(builder.Build(partial, Theme(), true, true, 0, cache, partialList, stats), EBuildResult::OK);
        QCOMPARE(stats.iRebuiltRows, 1);
        QCOMPARE(stats.iReusedRows, 23);

        TerminalFrame full2;
        source.CaptureFrame(TerminalCaptureRequest{0, 0, true}, full2);
        DrawListBuilder fresh;
        DrawList freshList;
        QVERIFY(BuildWith(freshList, fresh, full2, cache));
        QVERIFY(SameQuads(partialList, freshList));
    }

    void cleanRowWithNewViewportYUpdatesRow()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(4, 3);
        Cell(frame, 2, 1).bBgDefault = false;
        Cell(frame, 2, 1).bg = RED;
        DrawListBuilder builder;
        DrawList first;
        QVERIFY(BuildWith(first, builder, frame, cache));
        QCOMPARE(int(Backgrounds(first)[0].iRow), 2);

        TerminalFrame next = MakeFrame(4, 3);
        next.eDirty = EFrameDirty::PARTIAL;
        next.vecRows.erase(next.vecRows.begin(), next.vecRows.begin() + 2);   // llId 3 만 남긴다
        next.vecRows[0].iViewportY = 1;
        next.vecRows[0].bDirty = false;
        next.vecRows[0].vecCells.clear();
        DrawList list;
        DrawListBuildStats stats;
        QCOMPARE(builder.Build(next, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        QCOMPARE(stats.iReusedRows, 1);
        QCOMPARE(int(Backgrounds(list)[0].iRow), 1);
        QVERIFY(std::find(list.vecDirtyRows.begin(), list.vecDirtyRows.end(), 1) != list.vecDirtyRows.end());
        QVERIFY(std::find(list.vecDirtyRows.begin(), list.vecDirtyRows.end(), 2) != list.vecDirtyRows.end());
    }

    void partialOnNewOrInvalidatedBuilderNeedsFull()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(4, 2);
        frame.eDirty = EFrameDirty::PARTIAL;
        for (TerminalRow& row : frame.vecRows)
        {
            row.bDirty = false;
            row.vecCells.clear();
        }
        DrawListBuilder builder;
        QVERIFY(builder.NeedsFullFrame());
        DrawList list;
        DrawListBuildStats stats;
        QCOMPARE(builder.Build(frame, Theme(), true, true, 0, cache, list, stats), EBuildResult::NEED_FULL_FRAME);
        QVERIFY(builder.NeedsFullFrame());

        TerminalFrame full = MakeFrame(4, 2);
        QCOMPARE(builder.Build(full, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        QVERIFY(!builder.NeedsFullFrame());
        QCOMPARE(builder.Build(frame, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);

        builder.Invalidate();
        QVERIFY(builder.NeedsFullFrame());
        QCOMPARE(builder.Build(frame, Theme(), true, true, 0, cache, list, stats), EBuildResult::NEED_FULL_FRAME);
        QCOMPARE(builder.Build(full, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        QVERIFY(!builder.NeedsFullFrame());
    }

    void themeChangeDropsCache()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame full = MakeFrame(4, 2);
        TerminalFrame partial = full;
        partial.eDirty = EFrameDirty::PARTIAL;
        for (TerminalRow& row : partial.vecRows)
        {
            row.bDirty = false;
            row.vecCells.clear();
        }
        DrawListBuilder builder;
        DrawList list;
        DrawListBuildStats stats;
        QCOMPARE(builder.Build(full, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        TerminalTheme other = Theme();
        other.selectionBg = RED;
        QCOMPARE(builder.Build(partial, other, true, true, 0, cache, list, stats), EBuildResult::NEED_FULL_FRAME);
    }

    void atlasFullNeedsFullFrame()
    {
        GlyphCache cache(BaseFont(), 1.0, 64, 64);
        DrawListBuilder builder;
        TerminalFrame many = MakeFrame(50, 4);
        char32_t uCodepoint = 0x4E00;
        for (int iRow = 0; iRow < 4; ++iRow)
            for (int iCol = 0; iCol < 50; ++iCol)
                Cell(many, iRow, iCol).uCodepoint = uCodepoint++;
        DrawList list;
        DrawListBuildStats stats;
        const uint32_t uGenerationBefore = cache.GetGeneration();
        QCOMPARE(builder.Build(many, Theme(), true, true, 0, cache, list, stats), EBuildResult::NEED_FULL_FRAME);
        QVERIFY(cache.GetGeneration() != uGenerationBefore);
        QVERIFY(builder.NeedsFullFrame());

        TerminalFrame few = MakeFrame(4, 1);
        for (int iCol = 0; iCol < 3; ++iCol)
            Cell(few, 0, iCol).uCodepoint = 0x4E00 + iCol;
        QCOMPARE(builder.Build(few, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        QVERIFY(!builder.NeedsFullFrame());
        QCOMPARE(list.uAtlasGeneration, cache.GetGeneration());
        for (const DrawQuad& quad : Glyphs(list))
        {
            const QSize size = cache.GetAtlas(quad.eKind == EQuadKind::GLYPH_COLOR ? EAtlasPage::COLOR : EAtlasPage::MONO)
                                   .GetImage().size();
            QVERIFY(quad.uAtlasX + quad.uW <= size.width());
            QVERIFY(quad.uAtlasY + quad.uH <= size.height());
        }
    }

    void positionsAndMetadata()
    {
        GlyphCache cache(BaseFont(), 2.0);
        const CellMetrics& metrics = cache.GetMetrics();
        TerminalFrame frame = MakeFrame(4, 2);
        TerminalRow overscan = frame.vecRows[1];
        overscan.llId = 99;
        overscan.iViewportY = 2;
        overscan.vecCells[0].bBgDefault = false;
        overscan.vecCells[0].bg = RED;
        frame.vecRows.push_back(overscan);
        TerminalRow above = frame.vecRows[1];
        above.llId = 98;
        above.iViewportY = -1;
        above.vecCells[0].bBgDefault = false;
        above.vecCells[0].bg = RED;
        frame.vecRows.push_back(above);
        DrawList list;
        QVERIFY(BuildOk(list, frame, cache, true, true, 5));
        QCOMPARE(list.iBackgroundCount, 2);
        QCOMPARE(int(list.vecQuads[0].iRow), 2);
        QCOMPARE(int(list.vecQuads[1].iRow), -1);
        QCOMPARE(list.iScrollOffsetPx, 5);
        QCOMPARE(list.iPaddingPx, 8);
        QCOMPARE(list.iCellWidth, metrics.iCellWidth);
        QCOMPARE(list.iCellHeight, metrics.iCellHeight);
        QCOMPARE(list.iCols, 4);
        QCOMPARE(list.iRows, 2);
        QCOMPARE(list.clearColor, FRAME_BG);
    }

    void dirtyRowsIncludeCursorMoves()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(4, 6);
        frame.cursor = TerminalCursor{true, false, ECursorShape::BLOCK, 0, 2, false, std::nullopt};
        DrawListBuilder builder;
        DrawList list;
        QVERIFY(BuildWith(list, builder, frame, cache));
        QVERIFY(list.bFullRedraw);

        TerminalFrame partial = frame;
        partial.eDirty = EFrameDirty::PARTIAL;
        for (TerminalRow& row : partial.vecRows)
        {
            row.bDirty = false;
            row.vecCells.clear();
        }
        partial.cursor.iRow = 4;
        QVERIFY(BuildWith(list, builder, partial, cache));
        QVERIFY(!list.bFullRedraw);
        QCOMPARE(list.vecDirtyRows, (std::vector<int>{2, 4}));

        QVERIFY(BuildWith(list, builder, partial, cache, true, true, 3));
        QVERIFY(list.bFullRedraw);
    }
    void overlayComesAfterRowDecorations()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const CellMetrics& metrics = cache.GetMetrics();
        TerminalFrame frame = MakeFrame(4, 2);
        Cell(frame, 0, 0).uCodepoint = 'A';
        Cell(frame, 1, 2).uCodepoint = 'B';
        Cell(frame, 1, 2).eUnderline = EUnderline::SINGLE;   // 커서 행보다 뒤 행의 장식
        frame.cursor = TerminalCursor{true, false, ECursorShape::BLOCK, 0, 0, false, std::nullopt};
        DrawList list;
        QVERIFY(BuildOk(list, frame, cache));
        const auto vecDeco = Decorations(list);
        QCOMPARE(int(vecDeco.size()), 3);
        QCOMPARE(int(vecDeco[0].iRow), 1);
        QCOMPARE(int(vecDeco[0].iY), metrics.iUnderlineY);
        QCOMPARE(vecDeco[1].eKind, EQuadKind::RECT);   // 커서 사각형
        QCOMPARE(vecDeco[1].uColor, PackColor(Theme().cursorColor));
        QCOMPARE(int(vecDeco[1].iRow), 0);
        QCOMPARE(vecDeco[2].eKind, EQuadKind::GLYPH_MONO);   // 커서 글자
        QCOMPARE(int(vecDeco[2].iRow), 0);
    }

    void blockCursorGlyphOnReusedCleanRow()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(4, 2);
        Cell(frame, 1, 2).uCodepoint = 'A';
        Cell(frame, 1, 2).bBgDefault = false;
        Cell(frame, 1, 2).bg = BLUE;
        frame.cursor = TerminalCursor{false, false, ECursorShape::BLOCK, 2, 1, false, std::nullopt};
        DrawListBuilder builder;
        DrawList list;
        QVERIFY(BuildWith(list, builder, frame, cache));
        QCOMPARE(list.iDecorationCount, 0);

        TerminalFrame partial = MakePartial(frame);
        partial.cursor.bVisible = true;
        DrawListBuildStats stats;
        QCOMPARE(builder.Build(partial, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        QCOMPARE(stats.iReusedRows, 2);
        const auto vecDeco = Decorations(list);
        QCOMPARE(int(vecDeco.size()), 2);
        QCOMPARE(vecDeco[1].eKind, EQuadKind::GLYPH_MONO);
        QCOMPARE(vecDeco[1].uColor, PackColor(BLUE));
        const GlyphEntry entry = cache.GetGlyph(U"A", false, false, 1);
        QCOMPARE(int(vecDeco[1].uAtlasX), entry.atlasRect.left());
        QCOMPARE(int(vecDeco[1].uAtlasY), entry.atlasRect.top());
    }

    void droppedCacheForcesFullRedraw()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame full = MakeFrame(4, 2);
        TerminalFrame allDirtyPartial = full;
        allDirtyPartial.eDirty = EFrameDirty::PARTIAL;   // 모든 행이 더럽다 -> 캐시가 없어도 빌드는 된다
        DrawListBuilder builder;
        DrawList list;
        DrawListBuildStats stats;

        QCOMPARE(builder.Build(full, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        QVERIFY(list.bFullRedraw);
        QCOMPARE(builder.Build(allDirtyPartial, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        QVERIFY(!list.bFullRedraw);

        builder.Invalidate();
        QVERIFY(builder.NeedsFullFrame());
        QCOMPARE(builder.Build(allDirtyPartial, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        QVERIFY(list.bFullRedraw);
        QVERIFY(builder.NeedsFullFrame());
        QCOMPARE(builder.Build(allDirtyPartial, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        QVERIFY(!list.bFullRedraw);

        QCOMPARE(builder.Build(full, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        QVERIFY(!builder.NeedsFullFrame());
        TerminalTheme other = Theme();
        other.cursorColor = RED;
        QCOMPARE(builder.Build(allDirtyPartial, other, true, true, 0, cache, list, stats), EBuildResult::OK);
        QVERIFY(list.bFullRedraw);   // 테마가 바뀌어 캐시를 버렸다
        QVERIFY(builder.NeedsFullFrame());
    }

    void fullFrameAfterPreviousBuildIsFullRedraw()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame full = MakeFrame(4, 2);
        DrawListBuilder builder;
        DrawList list;
        QVERIFY(BuildWith(list, builder, full, cache));
        QVERIFY(BuildWith(list, builder, full, cache));
        QVERIFY(list.bFullRedraw);
    }

    void differentGlyphCacheDropsRowCache()
    {
        GlyphCache cacheA(BaseFont(), 1.0);
        GlyphCache cacheB(BaseFont(), 1.0);   // 셀 크기가 같은 다른 캐시
        QCOMPARE(cacheA.GetMetrics().iCellWidth, cacheB.GetMetrics().iCellWidth);
        TerminalFrame full = MakeFrame(4, 2);
        DrawListBuilder builder;
        DrawList list;
        DrawListBuildStats stats;
        QCOMPARE(builder.Build(full, Theme(), true, true, 0, cacheA, list, stats), EBuildResult::OK);
        QCOMPARE(builder.Build(MakePartial(full), Theme(), true, true, 0, cacheB, list, stats),
                 EBuildResult::NEED_FULL_FRAME);
        QVERIFY(builder.NeedsFullFrame());
    }

    void evictedRowIdsAreForgotten()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame full = MakeFrame(4, 2);
        DrawListBuilder builder;
        DrawList list;
        DrawListBuildStats stats;
        QCOMPARE(builder.Build(full, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);

        TerminalFrame onlyFirst = MakePartial(full);   // id 2 가 프레임에서 빠진다
        onlyFirst.vecRows.pop_back();
        QCOMPARE(builder.Build(onlyFirst, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);
        QCOMPARE(stats.iReusedRows, 1);

        QCOMPARE(builder.Build(MakePartial(full), Theme(), true, true, 0, cache, list, stats),
                 EBuildResult::NEED_FULL_FRAME);
        QVERIFY(builder.NeedsFullFrame());
    }

    void atlasGenerationChangeBetweenBuildsNeedsFullFrame()
    {
        GlyphCache cache(BaseFont(), 1.0, 64, 64);
        TerminalFrame full = MakeFrame(4, 2);
        Cell(full, 0, 0).uCodepoint = 'A';
        DrawListBuilder builder;
        DrawList list;
        DrawListBuildStats stats;
        QCOMPARE(builder.Build(full, Theme(), true, true, 0, cache, list, stats), EBuildResult::OK);

        const uint32_t uGeneration = cache.GetGeneration();
        for (char32_t uCodepoint = 0x4E00; cache.GetGeneration() == uGeneration && uCodepoint < 0x4E00 + 400; ++uCodepoint)
            cache.GetGlyph(std::u32string(1, uCodepoint), false, false, 2);   // 다른 사용자가 아틀라스를 가득 채운다
        QVERIFY(cache.GetGeneration() != uGeneration);
        QCOMPARE(builder.Build(MakePartial(full), Theme(), true, true, 0, cache, list, stats),
                 EBuildResult::NEED_FULL_FRAME);
        QVERIFY(builder.NeedsFullFrame());
    }

    void preeditRowsAreDirty()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame full = MakeFrame(6, 5);
        TerminalPreedit preedit;
        TerminalCell cell;
        cell.uCodepoint = 'a';
        preedit.vecCells = {cell};
        full.optPreedit = preedit;
        full.cursor = TerminalCursor{false, false, ECursorShape::BLOCK, 1, 3, false, std::nullopt};
        DrawListBuilder builder;
        DrawList list;
        QVERIFY(BuildWith(list, builder, full, cache));

        TerminalFrame same = MakePartial(full);
        QVERIFY(BuildWith(list, builder, same, cache));
        QCOMPARE(list.vecDirtyRows, (std::vector<int>{3}));

        TerminalFrame gone = MakePartial(full);
        gone.optPreedit.reset();
        QVERIFY(BuildWith(list, builder, gone, cache));
        QCOMPARE(list.vecDirtyRows, (std::vector<int>{3}));   // 사라진 조합 글자의 행

        QVERIFY(BuildWith(list, builder, gone, cache));
        QVERIFY(list.vecDirtyRows.empty());
    }

    void underlineSpritesAreDistinct()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(3, 1);
        Cell(frame, 0, 0).eUnderline = EUnderline::CURLY;
        Cell(frame, 0, 1).eUnderline = EUnderline::DOTTED;
        Cell(frame, 0, 2).eUnderline = EUnderline::DASHED;
        DrawList list;
        QVERIFY(BuildOk(list, frame, cache));
        const auto vecDeco = Decorations(list);
        QCOMPARE(int(vecDeco.size()), 3);
        std::set<std::pair<int, int>> setRects;
        for (const DrawQuad& quad : vecDeco)
        {
            QCOMPARE(quad.eKind, EQuadKind::GLYPH_MONO);
            setRects.insert({quad.uAtlasX, quad.uAtlasY});
        }
        QCOMPARE(int(setRects.size()), 3);
        QCOMPARE(int(vecDeco[0].uAtlasX), cache.GetUnderlineSprite(EUnderlineSprite::CURLY).atlasRect.left());
        QCOMPARE(int(vecDeco[1].uAtlasX), cache.GetUnderlineSprite(EUnderlineSprite::DOTTED).atlasRect.left());
        QCOMPARE(int(vecDeco[2].uAtlasX), cache.GetUnderlineSprite(EUnderlineSprite::DASHED).atlasRect.left());
    }

    void barWidthAtDpr2()
    {
        GlyphCache cache(BaseFont(), 2.0);
        QCOMPARE(cache.GetMetrics().iLineThickness, 2);
        TerminalFrame frame = MakeFrame(4, 1);
        frame.cursor = TerminalCursor{true, false, ECursorShape::BAR, 1, 0, false, std::nullopt};
        DrawList list;
        QVERIFY(BuildOk(list, frame, cache));
        const auto vecDeco = Decorations(list);
        QCOMPARE(int(vecDeco.size()), 1);
        QCOMPARE(int(vecDeco[0].uW), 2);
    }

    // ---- 최소 대비(TerminalTheme::dMinContrastRatio) ----

    void minContrastIsPartOfTheTheme()
    {
        const TerminalTheme theme;
        QCOMPARE(theme.dMinContrastRatio, 1.0);   // 기본은 끔
        TerminalTheme other = theme;
        other.dMinContrastRatio = MIN_CONTRAST;
        QVERIFY(theme != other);
        other.dMinContrastRatio = 1.0;
        QVERIFY(theme == other);
        other.dMinContrastRatio = std::numeric_limits<double>::quiet_NaN();   // 잘못된 값도 자기 자신과는 같다(매 프레임 캐시를 버리지 않게)
        const TerminalTheme copy = other;
        QVERIFY(other == copy);
        QVERIFY(other != theme);
    }

    // 끈 값(1 이하, NaN)은 기본 테마와 같은 목록을 만든다: 낮은 대비의 글자 · 흐리게 · 반전 · 선택 · 블록 커서 · 조합 글자 · 밑줄 · 취소선
    void minContrastOffKeepsEveryColor()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(10, 2);
        const auto funcSet = [&](int col, char32_t codepoint) -> TerminalCell&
        {
            TerminalCell& cell = Cell(frame, 0, col);
            cell.uCodepoint = codepoint;
            cell.fg = DARK_BLUE;
            cell.underlineColor = DARK_BLUE;
            return cell;
        };
        funcSet(0, 'A');
        funcSet(1, 'B').bFaint = true;
        funcSet(2, 'C').bInverse = true;
        funcSet(3, 'D').bStrikethrough = true;
        funcSet(4, 'E').eUnderline = EUnderline::CURLY;
        funcSet(5, 'F');
        frame.vecRows[0].iSelectBegin = 5;
        frame.vecRows[0].iSelectEnd = 6;
        Cell(frame, 1, 0).uCodepoint = 'G';
        Cell(frame, 1, 0).fg = DARK_BLUE;
        frame.cursor = TerminalCursor{true, false, ECursorShape::BLOCK, 0, 1, false, TerminalRgb{20, 30, 40}};
        DrawList listDefault;
        QVERIFY(BuildOk(listDefault, frame, cache));
        QCOMPARE(Glyphs(listDefault)[0].uColor, PackColor(DARK_BLUE));
        for (const double dRatio : {1.0, 0.5, 0.0, -1.0, std::numeric_limits<double>::quiet_NaN()})
        {
            DrawList list;
            QVERIFY(BuildThemed(list, frame, cache, MakeContrastTheme(dRatio)));
            QVERIFY2(SameQuads(list, listDefault), qPrintable(QString::number(dRatio)));
        }
        TerminalPreedit preedit;
        TerminalCell preeditCell;
        preeditCell.uCodepoint = 'p';
        preeditCell.fg = DARK_BLUE;
        preedit.vecCells = {preeditCell};
        frame.optPreedit = preedit;
        QVERIFY(BuildOk(listDefault, frame, cache));
        DrawList list;
        QVERIFY(BuildThemed(list, frame, cache, MakeContrastTheme(1.0)));
        QVERIFY(SameQuads(list, listDefault));
    }

    // 켜면 글자색(글리프 · 취소선)을 고친다. 밑줄 색은 따로 정한 색이라 그대로다. 이미 충분한 칸은 그대로다
    void minContrastAdjustsTextAndStrike()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const CellMetrics& metrics = cache.GetMetrics();
        TerminalFrame frame = MakeFrame(4, 1);
        Cell(frame, 0, 0).uCodepoint = 'A';
        Cell(frame, 0, 0).fg = DARK_BLUE;
        TerminalCell& decorated = Cell(frame, 0, 1);
        decorated.uCodepoint = 'B';
        decorated.fg = DARK_BLUE;
        decorated.bStrikethrough = true;
        decorated.eUnderline = EUnderline::SINGLE;
        decorated.underlineColor = DARK_BLUE;
        Cell(frame, 0, 2).uCodepoint = 'C';   // FRAME_FG: 이미 충분하다
        const TerminalRgb expected = EnsureContrast(DARK_BLUE, FRAME_BG, MIN_CONTRAST);
        QVERIFY(expected != DARK_BLUE);
        QVERIFY(GetContrastRatio(expected, FRAME_BG) >= MIN_CONTRAST);
        QVERIFY(GetContrastRatio(FRAME_FG, FRAME_BG) >= MIN_CONTRAST);

        DrawList list;
        QVERIFY(BuildThemed(list, frame, cache, MakeContrastTheme(MIN_CONTRAST)));
        const auto vecGlyph = Glyphs(list);
        QCOMPARE(int(vecGlyph.size()), 3);
        QCOMPARE(vecGlyph[0].uColor, PackColor(expected));
        QCOMPARE(vecGlyph[1].uColor, PackColor(expected));
        QCOMPARE(vecGlyph[2].uColor, PackColor(FRAME_FG));
        const auto vecDeco = Decorations(list);
        QCOMPARE(int(vecDeco.size()), 2);
        QCOMPARE(int(vecDeco[0].iY), metrics.iUnderlineY);
        QCOMPARE(vecDeco[0].uColor, PackColor(DARK_BLUE));   // 밑줄: 고치지 않는다
        QCOMPARE(int(vecDeco[1].iY), metrics.iStrikeY);
        QCOMPARE(vecDeco[1].uColor, PackColor(expected));    // 취소선: 글자색을 따른다
    }

    // 순서: 반전 -> 선택 -> 대비 -> 흐리게. 흐린 글자는 고친 글자색을 배경과 반씩 섞어 여전히 보통 글자보다 흐리다
    void minContrastComesBeforeFaint()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(3, 1);
        Cell(frame, 0, 0).uCodepoint = 'A';
        Cell(frame, 0, 0).fg = DARK_BLUE;
        Cell(frame, 0, 1).uCodepoint = 'B';
        Cell(frame, 0, 1).fg = DARK_BLUE;
        Cell(frame, 0, 1).bFaint = true;
        DrawList list;
        QVERIFY(BuildThemed(list, frame, cache, MakeContrastTheme(MIN_CONTRAST)));
        const auto vecGlyph = Glyphs(list);
        QCOMPARE(int(vecGlyph.size()), 2);
        const TerminalRgb normal = EnsureContrast(DARK_BLUE, FRAME_BG, MIN_CONTRAST);
        const TerminalRgb faint = MixHalf(normal, FRAME_BG);
        QCOMPARE(vecGlyph[0].uColor, PackColor(normal));
        QCOMPARE(vecGlyph[1].uColor, PackColor(faint));
        QVERIFY(GetContrastRatio(faint, FRAME_BG) < GetContrastRatio(normal, FRAME_BG));
        QVERIFY(GetContrastRatio(faint, FRAME_BG) > GetContrastRatio(MixHalf(DARK_BLUE, FRAME_BG), FRAME_BG));   // 끈 때보다는 읽힌다
        // 흐린 글자의 대비가 원래 충분하면(고칠 것 없음) 끈 때와 같다
        Cell(frame, 0, 2).uCodepoint = 'C';
        Cell(frame, 0, 2).bFaint = true;
        QVERIFY(BuildThemed(list, frame, cache, MakeContrastTheme(MIN_CONTRAST)));
        QCOMPARE(Glyphs(list)[2].uColor, PackColor(MixHalf(FRAME_FG, FRAME_BG)));
    }

    // 선택 칸은 선택 배경(과 선택 글자색)으로 대비를 잰다
    void minContrastUsesSelectionColors()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const int iW = cache.GetMetrics().iCellWidth;
        const TerminalRgb gray{140, 150, 160};   // FRAME_BG 위 6.2, 선택 배경(50,60,70) 위 3.7
        TerminalFrame frame = MakeFrame(3, 1);
        Cell(frame, 0, 0).uCodepoint = 'A';
        Cell(frame, 0, 0).fg = gray;
        Cell(frame, 0, 1).uCodepoint = 'B';
        Cell(frame, 0, 1).fg = gray;
        frame.vecRows[0].iSelectBegin = 1;
        frame.vecRows[0].iSelectEnd = 2;
        TerminalTheme theme = MakeContrastTheme(MIN_CONTRAST);
        QVERIFY(GetContrastRatio(gray, FRAME_BG) >= MIN_CONTRAST);
        QVERIFY(GetContrastRatio(gray, theme.selectionBg) < MIN_CONTRAST);
        DrawList list;
        QVERIFY(BuildThemed(list, frame, cache, theme));
        QCOMPARE(FindGlyphAt(Glyphs(list), 0, iW)->uColor, PackColor(gray));
        QCOMPARE(FindGlyphAt(Glyphs(list), 1, iW)->uColor, PackColor(EnsureContrast(gray, theme.selectionBg, MIN_CONTRAST)));
        QVERIFY(FindGlyphAt(Glyphs(list), 1, iW)->uColor != PackColor(gray));
        QCOMPARE(Backgrounds(list)[0].uColor, PackColor(theme.selectionBg));   // 배경은 그대로

        const TerminalRgb selectionFg{60, 70, 80};
        theme.optSelectionFg = selectionFg;
        QVERIFY(BuildThemed(list, frame, cache, theme));
        QCOMPARE(FindGlyphAt(Glyphs(list), 1, iW)->uColor, PackColor(EnsureContrast(selectionFg, theme.selectionBg, MIN_CONTRAST)));
    }

    // 반전한 칸은 바꾼 뒤의 글자색(원래 배경색)을 바꾼 뒤의 배경(원래 글자색)에 맞춘다
    void minContrastAfterInverse()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(2, 1);
        TerminalCell& cell = Cell(frame, 0, 0);
        cell.uCodepoint = 'A';
        cell.fg = TerminalRgb{40, 40, 40};
        cell.bg = TerminalRgb{20, 20, 20};
        cell.bBgDefault = false;
        cell.bInverse = true;
        DrawList list;
        QVERIFY(BuildThemed(list, frame, cache, MakeContrastTheme(MIN_CONTRAST)));
        QCOMPARE(Backgrounds(list)[0].uColor, PackColor(TerminalRgb{40, 40, 40}));
        const TerminalRgb expected = EnsureContrast(TerminalRgb{20, 20, 20}, TerminalRgb{40, 40, 40}, MIN_CONTRAST);
        QVERIFY(expected != (TerminalRgb{20, 20, 20}));
        QCOMPARE(Glyphs(list)[0].uColor, PackColor(expected));
    }

    // 블록 커서 위 글자(칸의 배경색으로 그린다)도 커서 색에 대해 같은 규칙을 따른다
    void minContrastBlockCursorGlyph()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const CellMetrics& metrics = cache.GetMetrics();
        const TerminalRgb cursorColor{40, 40, 50};   // FRAME_BG 와 대비 1.3
        TerminalFrame frame = MakeFrame(3, 1);
        Cell(frame, 0, 1).uCodepoint = 'A';
        frame.cursor = TerminalCursor{true, false, ECursorShape::BLOCK, 1, 0, false, cursorColor};
        DrawList list;
        QVERIFY(BuildThemed(list, frame, cache, MakeContrastTheme(1.0)));
        auto vecDeco = Decorations(list);
        QCOMPARE(int(vecDeco.size()), 2);
        QCOMPARE(vecDeco[1].uColor, PackColor(FRAME_BG));   // 끄면 칸의 배경색 그대로

        QVERIFY(BuildThemed(list, frame, cache, MakeContrastTheme(MIN_CONTRAST)));
        vecDeco = Decorations(list);
        QCOMPARE(int(vecDeco.size()), 2);
        QCOMPARE(vecDeco[0].uColor, PackColor(cursorColor));   // 커서 사각형은 그대로
        const TerminalRgb expected = EnsureContrast(FRAME_BG, cursorColor, MIN_CONTRAST);
        QVERIFY(expected != FRAME_BG);
        QCOMPARE(vecDeco[1].eKind, EQuadKind::GLYPH_MONO);
        QCOMPARE(vecDeco[1].uColor, PackColor(expected));
        QCOMPARE(FindGlyphAt(Glyphs(list), 1, metrics.iCellWidth)->uColor, PackColor(FRAME_FG));   // 행의 글자는 FRAME_BG 위라 충분

        // 막대 커서는 글자를 다시 그리지 않는다
        frame.cursor.eShape = ECursorShape::BAR;
        QVERIFY(BuildThemed(list, frame, cache, MakeContrastTheme(MIN_CONTRAST)));
        QCOMPARE(int(Decorations(list).size()), 1);
    }

    // 숨김 칸(글자 없음, 취소선은 원래 색), 상자 · 블록 문자, Powerline 모양은 고치지 않는다
    void minContrastSkipsInvisibleAndShapes()
    {
        GlyphCache cache(BaseFont(), 1.0);
        const CellMetrics& metrics = cache.GetMetrics();
        const int iW = metrics.iCellWidth;
        TerminalFrame frame = MakeFrame(8, 1);
        const char32_t arrCodepoints[] = {U'A', U'─', U'▀', U'█', U'\uE0B0', U'B'};   // U+E0B0: Powerline 구분선
        for (int iCol = 0; iCol < int(std::size(arrCodepoints)); ++iCol)
        {
            TerminalCell& cell = Cell(frame, 0, iCol);
            cell.uCodepoint = arrCodepoints[iCol];
            cell.fg = DARK_BLUE;
            cell.bStrikethrough = true;
        }
        Cell(frame, 0, 0).bInvisible = true;
        Cell(frame, 0, 2).bBgDefault = false;   // 반칸 화소: 위 DARK_BLUE, 아래 (0,0,120)
        Cell(frame, 0, 2).bg = TerminalRgb{0, 0, 120};
        DrawList list;
        QVERIFY(BuildThemed(list, frame, cache, MakeContrastTheme(MIN_CONTRAST)));
        const auto vecGlyph = Glyphs(list);
        const auto vecDeco = Decorations(list);
        QVERIFY(!FindGlyphAt(vecGlyph, 0, iW));   // 숨김: 글리프 없음
        QCOMPARE(FindStrikeAt(vecDeco, 0, metrics)->uColor, PackColor(DARK_BLUE));
        for (int iCol = 1; iCol <= 3; ++iCol)
        {
            QVERIFY2(FindGlyphAt(vecGlyph, iCol, iW), qPrintable(QString::number(iCol)));
            QCOMPARE(FindGlyphAt(vecGlyph, iCol, iW)->uColor, PackColor(DARK_BLUE));
            QCOMPARE(FindStrikeAt(vecDeco, iCol, metrics)->uColor, PackColor(DARK_BLUE));
        }
        QCOMPARE(FindStrikeAt(vecDeco, 4, metrics)->uColor, PackColor(DARK_BLUE));   // Powerline(글꼴에 없으면 글리프는 비어도 취소선은 있다)
        const TerminalRgb expected = EnsureContrast(DARK_BLUE, FRAME_BG, MIN_CONTRAST);
        QCOMPARE(FindGlyphAt(vecGlyph, 5, iW)->uColor, PackColor(expected));   // 보통 글자는 고친다
        QCOMPARE(FindStrikeAt(vecDeco, 5, metrics)->uColor, PackColor(expected));
    }

    // 컬러 글리프(이모지 RGBA)는 글자색을 쓰지 않는다(불투명 흰색 기록)
    void minContrastLeavesColorGlyphs()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(4, 1);
        Cell(frame, 0, 0).uCodepoint = 0x1F600;
        Cell(frame, 0, 0).eWidth = ECellWidth::WIDE;
        Cell(frame, 0, 0).fg = DARK_BLUE;
        Cell(frame, 0, 1).eWidth = ECellWidth::SPACER_TAIL;
        DrawList list;
        QVERIFY(BuildThemed(list, frame, cache, MakeContrastTheme(MIN_CONTRAST)));
        const auto vecGlyph = Glyphs(list);
        QCOMPARE(int(vecGlyph.size()), 1);
        if (vecGlyph[0].eKind != EQuadKind::GLYPH_COLOR)
        {
            // 컬러 이모지 글꼴이 없으면(offscreen) 흑백 글리프다: 글자처럼 고친다
            QCOMPARE(vecGlyph[0].uColor, PackColor(EnsureContrast(DARK_BLUE, FRAME_BG, MIN_CONTRAST)));
            QSKIP("no color emoji font (QT_QPA_PLATFORM=windows runs the color glyph check)");
        }
        QCOMPARE(vecGlyph[0].uColor, 0xFFFFFFFFu);
    }

    void minContrastAppliesToPreedit()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame frame = MakeFrame(6, 1);
        frame.cursor = TerminalCursor{true, false, ECursorShape::BLOCK, 1, 0, false, std::nullopt};
        TerminalPreedit preedit;
        TerminalCell cell;
        cell.uCodepoint = 'p';
        cell.fg = DARK_BLUE;
        preedit.vecCells = {cell};
        preedit.iCaretCell = 1;
        frame.optPreedit = preedit;
        DrawList list;
        QVERIFY(BuildThemed(list, frame, cache, MakeContrastTheme(MIN_CONTRAST)));
        int iGlyphs = 0;
        for (const DrawQuad& quad : Decorations(list))
        {
            if (quad.eKind == EQuadKind::RECT)
                continue;
            ++iGlyphs;
            QCOMPARE(quad.uColor, PackColor(EnsureContrast(DARK_BLUE, FRAME_BG, MIN_CONTRAST)));
        }
        QCOMPARE(iGlyphs, 1);
    }

    // 값만 바꿔도 테마가 달라 행 캐시를 버린다(렌더 스레드의 SetTheme 도 같은 비교로 다시 그린다)
    void minContrastChangeDropsRowCache()
    {
        GlyphCache cache(BaseFont(), 1.0);
        TerminalFrame full = MakeFrame(3, 2);
        Cell(full, 0, 0).uCodepoint = 'A';
        Cell(full, 0, 0).fg = DARK_BLUE;
        DrawListBuilder builder;
        DrawList list;
        DrawListBuildStats stats;
        QCOMPARE(builder.Build(full, MakeContrastTheme(1.0), true, true, 0, cache, list, stats), EBuildResult::OK);
        QCOMPARE(builder.Build(MakePartial(full), MakeContrastTheme(1.0), true, true, 0, cache, list, stats), EBuildResult::OK);
        QCOMPARE(builder.Build(MakePartial(full), MakeContrastTheme(MIN_CONTRAST), true, true, 0, cache, list, stats),
                 EBuildResult::NEED_FULL_FRAME);
        QVERIFY(builder.NeedsFullFrame());
        QCOMPARE(builder.Build(full, MakeContrastTheme(MIN_CONTRAST), true, true, 0, cache, list, stats), EBuildResult::OK);
        QVERIFY(list.bFullRedraw);
        QCOMPARE(Glyphs(list)[0].uColor, PackColor(EnsureContrast(DARK_BLUE, FRAME_BG, MIN_CONTRAST)));
        QCOMPARE(builder.Build(MakePartial(full), MakeContrastTheme(MIN_CONTRAST), true, true, 0, cache, list, stats), EBuildResult::OK);
        QCOMPARE(builder.Build(MakePartial(full), MakeContrastTheme(7.0), true, true, 0, cache, list, stats),
                 EBuildResult::NEED_FULL_FRAME);
    }

    // 같은 입력이면 같은 목록: 새 빌더 · 캐시가 찬 빌더 · 다시 만든 빌더. 글자마다 EnsureContrast 와 같다
    void minContrastIsDeterministic()
    {
        GlyphCache cache(BaseFont(), 1.0);
        constexpr int kCols = 60;
        constexpr int kRows = 12;
        TerminalFrame frame = MakeFrame(kCols, kRows);
        std::mt19937 rng(12345);
        const TerminalRgb arrBackgrounds[] = {FRAME_BG, {40, 40, 40}, {250, 250, 250}, {0, 0, 120}};
        for (int iRow = 0; iRow < kRows; ++iRow)
        {
            for (int iCol = 0; iCol < kCols; ++iCol)
            {
                TerminalCell& cell = Cell(frame, iRow, iCol);
                cell.uCodepoint = uint32_t('a' + rng() % 26);
                cell.fg = TerminalRgb{uint8_t(rng() % 96), uint8_t(rng() % 96), uint8_t(rng() % 256)};
                const TerminalRgb& bg = arrBackgrounds[rng() % std::size(arrBackgrounds)];
                cell.bBgDefault = bg == FRAME_BG;
                cell.bg = cell.bBgDefault ? SENTINEL_BG : bg;
            }
        }
        DrawListBuilder builder;
        DrawList first;
        DrawListBuildStats stats;
        const TerminalTheme theme = MakeContrastTheme(MIN_CONTRAST);
        QCOMPARE(builder.Build(frame, theme, true, true, 0, cache, first, stats), EBuildResult::OK);
        DrawList second;
        QCOMPARE(builder.Build(frame, theme, true, true, 0, cache, second, stats), EBuildResult::OK);
        DrawList fresh;
        QVERIFY(BuildThemed(fresh, frame, cache, theme));
        QVERIFY(SameQuads(first, second));
        QVERIFY(SameQuads(first, fresh));

        int iAdjusted = 0;
        const auto vecGlyph = Glyphs(first);
        for (const DrawQuad& quad : vecGlyph)
        {
            const int iCol = GetGlyphColumn(quad, cache.GetMetrics().iCellWidth);
            const TerminalCell& cell = frame.vecRows[size_t(quad.iRow)].vecCells[size_t(iCol)];
            const TerminalRgb bg = cell.bBgDefault ? FRAME_BG : cell.bg;
            const TerminalRgb expected = EnsureContrast(cell.fg, bg, MIN_CONTRAST);
            QCOMPARE(quad.uColor, PackColor(expected));
            QVERIFY(GetContrastRatio(UnpackColor(quad.uColor), bg) >= MIN_CONTRAST);
            iAdjusted += expected != cell.fg ? 1 : 0;
        }
        QVERIFY(int(vecGlyph.size()) > kCols * kRows / 2);
        QVERIFY(iAdjusted > int(vecGlyph.size()) / 4);
    }
};

QObject* CreateDrawListBuilderTest() { return new DrawListBuilderTest(); }

#include "DrawListBuilderTest.moc"
