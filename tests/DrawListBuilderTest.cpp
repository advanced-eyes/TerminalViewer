#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawListBuilder.h"

#include "TerminalViewer/Testing/FakeFrameSource.h"
#include "TestFonts.h"

#include <QTest>

#include <algorithm>
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
};

QObject* CreateDrawListBuilderTest() { return new DrawListBuilderTest(); }

#include "DrawListBuilderTest.moc"
