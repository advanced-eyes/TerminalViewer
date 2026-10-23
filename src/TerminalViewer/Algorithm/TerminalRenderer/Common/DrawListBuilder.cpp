#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawListBuilder.h"

#include <algorithm>
#include <set>
#include <utility>

namespace
{
// 컬러 글리프는 색을 쓰지 않으므로 불투명 흰색을 기록한다.
constexpr uint32_t COLOR_GLYPH_COLOR = 0xFFFFFFFFu;

struct ResolvedColors
{
    TerminalRgb fg;
    TerminalRgb bg;
    bool bHasBg = false;
};

uint8_t MixHalf(uint8_t a, uint8_t b)
{
    return uint8_t((int(a) + int(b) + 1) / 2);
}

// 색 해석 순서: 반전 -> 선택 -> 흐리게
ResolvedColors ResolveColors(const TerminalCell& cell, bool selected, const TerminalFrame& frame,
                             const TerminalTheme& theme)
{
    ResolvedColors colors;
    colors.fg = cell.fg;
    colors.bg = cell.bBgDefault ? frame.background : cell.bg;
    colors.bHasBg = !cell.bBgDefault;
    if (cell.bInverse)
    {
        std::swap(colors.fg, colors.bg);
        colors.bHasBg = true;
    }
    if (selected)
    {
        colors.bg = theme.selectionBg;
        colors.bHasBg = true;
        if (theme.optSelectionFg)
            colors.fg = *theme.optSelectionFg;
    }
    if (cell.bFaint)
        colors.fg = TerminalRgb{MixHalf(colors.fg.r, colors.bg.r), MixHalf(colors.fg.g, colors.bg.g),
                                MixHalf(colors.fg.b, colors.bg.b)};
    return colors;
}

bool IsSelected(const TerminalRow& row, int col)
{
    return row.iSelectBegin >= 0 && col >= row.iSelectBegin && col < row.iSelectEnd;
}

void AddRect(std::vector<DrawQuad>& quads, int x, int y, int w, int h, uint32_t color, int limitX)
{
    if (x < 0)
    {
        w += x;
        x = 0;
    }
    w = std::min(w, limitX - x);
    if (w <= 0 || h <= 0)
        return;
    DrawQuad quad;
    quad.iX = int16_t(x);
    quad.iY = int16_t(y);
    quad.uW = uint16_t(w);
    quad.uH = uint16_t(h);
    quad.uColor = color;
    quad.eKind = EQuadKind::RECT;
    quads.push_back(quad);
}

// entry 의 비트맵을 cellX 에서 시작하는 칸에 놓는다. 격자 가로 범위로 자른다.
void AddGlyphQuad(std::vector<DrawQuad>& quads, const GlyphEntry& entry, int cellX, uint32_t color, int limitX)
{
    if (entry.bEmpty)
        return;
    int iX = cellX + entry.offset.x();
    int iW = entry.atlasRect.width();
    int iAtlasX = entry.atlasRect.left();
    if (iX < 0)
    {
        iAtlasX -= iX;
        iW += iX;
        iX = 0;
    }
    iW = std::min(iW, limitX - iX);
    if (iW <= 0)
        return;
    DrawQuad quad;
    quad.iX = int16_t(iX);
    quad.iY = int16_t(entry.offset.y());
    quad.uW = uint16_t(iW);
    quad.uH = uint16_t(entry.atlasRect.height());
    quad.uAtlasX = uint16_t(iAtlasX);
    quad.uAtlasY = uint16_t(entry.atlasRect.top());
    const bool bColor = entry.ePage == EAtlasPage::COLOR;
    quad.uColor = bColor ? COLOR_GLYPH_COLOR : color;
    quad.eKind = bColor ? EQuadKind::GLYPH_COLOR : EQuadKind::GLYPH_MONO;
    quads.push_back(quad);
}

void AddCellGlyph(std::vector<DrawQuad>& quads, const TerminalCell& cell, const std::vector<std::u32string>& graphemes,
                  int span, uint32_t color, int cellX, GlyphCache& cache, int limitX)
{
    std::u32string strText;
    if (cell.iGrapheme >= 0 && cell.iGrapheme < int(graphemes.size()))
        strText = graphemes[size_t(cell.iGrapheme)];
    else if (cell.uCodepoint != 0)
        strText = std::u32string(1, char32_t(cell.uCodepoint));
    else
        return;
    AddGlyphQuad(quads, cache.GetGlyph(strText, cell.bBold, cell.bItalic, span), cellX, color, limitX);
}

EUnderlineSprite SpriteFor(EUnderline underline)
{
    switch (underline)
    {
    case EUnderline::DOTTED:
        return EUnderlineSprite::DOTTED;
    case EUnderline::DASHED:
        return EUnderlineSprite::DASHED;
    default:
        return EUnderlineSprite::CURLY;
    }
}

void AddUnderline(std::vector<DrawQuad>& quads, EUnderline underline, const TerminalRgb& rgb, int cellX, int span,
                  GlyphCache& cache, int limitX)
{
    const CellMetrics& metrics = cache.GetMetrics();
    const uint32_t uColor = PackColor(rgb);
    const int iWidth = span * metrics.iCellWidth;
    const int iThickness = metrics.iUnderlineThickness;
    switch (underline)
    {
    case EUnderline::NONE:
        break;
    case EUnderline::SINGLE:
        AddRect(quads, cellX, metrics.iUnderlineY, iWidth, iThickness, uColor, limitX);
        break;
    case EUnderline::DOUBLE:
    {
        const int iSecond = std::max(0, std::min(metrics.iUnderlineY + 2 * iThickness, metrics.iCellHeight - iThickness));
        const int iFirst = std::max(0, std::min(metrics.iUnderlineY, iSecond - 2 * iThickness));
        AddRect(quads, cellX, iFirst, iWidth, iThickness, uColor, limitX);
        AddRect(quads, cellX, iSecond, iWidth, iThickness, uColor, limitX);
        break;
    }
    default:
    {
        const GlyphEntry sprite = cache.GetUnderlineSprite(SpriteFor(underline));
        for (int iIndex = 0; iIndex < span; ++iIndex)
            AddGlyphQuad(quads, sprite, cellX + iIndex * metrics.iCellWidth, uColor, limitX);
        break;
    }
    }
}

void BuildRowQuads(const TerminalRow& row, const TerminalFrame& frame, const TerminalTheme& theme, GlyphCache& cache,
                   std::vector<DrawQuad>& background, std::vector<DrawQuad>& glyphs, std::vector<DrawQuad>& decorations)
{
    const CellMetrics& metrics = cache.GetMetrics();
    const int iWidth = metrics.iCellWidth;
    const int iLimitX = frame.iCols * iWidth;
    const int iCount = std::min(int(row.vecCells.size()), frame.iCols);

    bool bRun = false;
    int iRunBegin = 0;
    int iRunEnd = 0;
    TerminalRgb runColor;
    const auto funcFlushRun = [&]()
    {
        if (bRun)
            AddRect(background, iRunBegin * iWidth, 0, (iRunEnd - iRunBegin) * iWidth, metrics.iCellHeight,
                    PackColor(runColor), iLimitX);
        bRun = false;
    };

    for (int iCol = 0; iCol < iCount; ++iCol)
    {
        const TerminalCell& cell = row.vecCells[size_t(iCol)];
        const bool bTail = cell.eWidth == ECellWidth::SPACER_TAIL;
        const bool bSelected = IsSelected(row, iCol);
        const TerminalCell& source = (bTail && iCol > 0 && row.vecCells[size_t(iCol - 1)].eWidth == ECellWidth::WIDE)
                                         ? row.vecCells[size_t(iCol - 1)]
                                         : cell;
        const ResolvedColors colors = ResolveColors(source, bSelected, frame, theme);

        if (colors.bHasBg)
        {
            if (bRun && iRunEnd == iCol && runColor == colors.bg)
                iRunEnd = iCol + 1;
            else
            {
                funcFlushRun();
                bRun = true;
                iRunBegin = iCol;
                iRunEnd = iCol + 1;
                runColor = colors.bg;
            }
        }
        else
            funcFlushRun();

        if (bTail)
            continue;
        const int iSpan = cell.eWidth == ECellWidth::WIDE ? 2 : 1;
        const int iCellX = iCol * iWidth;
        if (!cell.bInvisible)
            AddCellGlyph(glyphs, cell, row.vecGraphemes, iSpan, PackColor(colors.fg), iCellX, cache, iLimitX);
        AddUnderline(decorations, cell.eUnderline, cell.underlineColor, iCellX, iSpan, cache, iLimitX);
        if (cell.bStrikethrough)
            AddRect(decorations, iCellX, metrics.iStrikeY, iSpan * iWidth, metrics.iStrikeThickness,
                    PackColor(colors.fg), iLimitX);
    }
    funcFlushRun();
}

TerminalRgb CursorColor(const TerminalFrame& frame, const TerminalTheme& theme)
{
    return frame.cursor.optColor ? *frame.cursor.optColor : theme.cursorColor;
}

void AddBar(std::vector<DrawQuad>& quads, int x, const CellMetrics& metrics, uint32_t color, int limitX)
{
    AddRect(quads, x, 0, metrics.iLineThickness, metrics.iCellHeight, color, limitX);
}

void AddCursor(std::vector<DrawQuad>& quads, const TerminalFrame& frame, const TerminalTheme& theme, bool focused,
               const TerminalRow* row, GlyphCache& cache)
{
    const CellMetrics& metrics = cache.GetMetrics();
    const TerminalCursor& cursor = frame.cursor;
    const int iLimitX = frame.iCols * metrics.iCellWidth;
    int iCol = cursor.iCol;
    if (iCol < 0 || iCol >= frame.iCols)
        return;

    const TerminalCell* pCell = nullptr;
    if (row && iCol < int(row->vecCells.size()))
    {
        if (row->vecCells[size_t(iCol)].eWidth == ECellWidth::SPACER_TAIL && iCol > 0)
            --iCol;
        pCell = &row->vecCells[size_t(iCol)];
    }
    const bool bWide = cursor.bWide || (pCell && pCell->eWidth == ECellWidth::WIDE);
    const int iSpan = bWide ? 2 : 1;
    const int iX = iCol * metrics.iCellWidth;
    const int iW = iSpan * metrics.iCellWidth;
    const int iH = metrics.iCellHeight;
    const int iT = metrics.iLineThickness;
    const uint32_t uColor = PackColor(CursorColor(frame, theme));

    const ECursorShape eShape = focused ? cursor.eShape : ECursorShape::BLOCK_HOLLOW;
    switch (eShape)
    {
    case ECursorShape::BLOCK:
        AddRect(quads, iX, 0, iW, iH, uColor, iLimitX);
        if (pCell && !pCell->bInvisible)
        {
            const ResolvedColors colors = ResolveColors(*pCell, IsSelected(*row, iCol), frame, theme);
            AddCellGlyph(quads, *pCell, row->vecGraphemes, iSpan, PackColor(colors.bg), iX, cache, iLimitX);
        }
        break;
    case ECursorShape::BAR:
        AddBar(quads, iX, metrics, uColor, iLimitX);
        break;
    case ECursorShape::UNDERLINE:
        AddRect(quads, iX, metrics.iUnderlineY, iW, metrics.iUnderlineThickness, uColor, iLimitX);
        break;
    case ECursorShape::BLOCK_HOLLOW:
        AddRect(quads, iX, 0, iW, iT, uColor, iLimitX);
        AddRect(quads, iX, iH - iT, iW, iT, uColor, iLimitX);
        AddRect(quads, iX, iT, iT, iH - 2 * iT, uColor, iLimitX);
        AddRect(quads, iX + iW - iT, iT, iT, iH - 2 * iT, uColor, iLimitX);
        break;
    }
}

void AddPreedit(std::vector<DrawQuad>& quads, const TerminalFrame& frame, const TerminalTheme& theme,
                GlyphCache& cache)
{
    const CellMetrics& metrics = cache.GetMetrics();
    const TerminalPreedit& preedit = *frame.optPreedit;
    const int iLimitX = frame.iCols * metrics.iCellWidth;
    const int iStart = frame.cursor.iCol;
    const int iCount = int(preedit.vecCells.size());

    std::vector<ResolvedColors> vecColors;
    for (int iIndex = 0; iIndex < iCount; ++iIndex)
    {
        const TerminalCell& cell = preedit.vecCells[size_t(iIndex)];
        const TerminalCell& source = (cell.eWidth == ECellWidth::SPACER_TAIL && iIndex > 0) ? preedit.vecCells[size_t(iIndex - 1)] : cell;
        vecColors.push_back(ResolveColors(source, false, frame, theme));
    }
    for (int iIndex = 0; iIndex < iCount; ++iIndex)
        AddRect(quads, (iStart + iIndex) * metrics.iCellWidth, 0, metrics.iCellWidth, metrics.iCellHeight,
                PackColor(vecColors[size_t(iIndex)].bg), iLimitX);
    for (int iIndex = 0; iIndex < iCount; ++iIndex)
    {
        const TerminalCell& cell = preedit.vecCells[size_t(iIndex)];
        if (cell.eWidth == ECellWidth::SPACER_TAIL || cell.bInvisible)
            continue;
        const int iSpan = cell.eWidth == ECellWidth::WIDE ? 2 : 1;
        AddCellGlyph(quads, cell, {}, iSpan, PackColor(vecColors[size_t(iIndex)].fg), (iStart + iIndex) * metrics.iCellWidth,
                     cache, iLimitX);
    }
    AddRect(quads, iStart * metrics.iCellWidth, metrics.iUnderlineY, iCount * metrics.iCellWidth,
            metrics.iUnderlineThickness, PackColor(frame.foreground), iLimitX);
    AddBar(quads, (iStart + preedit.iCaretCell) * metrics.iCellWidth, metrics, PackColor(CursorColor(frame, theme)),
           iLimitX);
}
}   // namespace

bool DrawListBuilder::Context::operator==(const Context& other) const
{
    return theme == other.theme && background == other.background && foreground == other.foreground &&
           iCols == other.iCols && pCache == other.pCache && metrics.iCellWidth == other.metrics.iCellWidth &&
           metrics.iCellHeight == other.metrics.iCellHeight && metrics.iBaseline == other.metrics.iBaseline &&
           metrics.iUnderlineY == other.metrics.iUnderlineY &&
           metrics.iUnderlineThickness == other.metrics.iUnderlineThickness &&
           metrics.iStrikeY == other.metrics.iStrikeY && metrics.iStrikeThickness == other.metrics.iStrikeThickness &&
           metrics.iLineThickness == other.metrics.iLineThickness &&
           metrics.dDevicePixelRatio == other.metrics.dDevicePixelRatio;
}

void DrawListBuilder::DropCache()
{
    m_hashRows.clear();
    m_bNeedFull = true;
    m_bHasPrevBuild = false;
}

void DrawListBuilder::Invalidate()
{
    DropCache();
    m_bHasContext = false;
}

bool DrawListBuilder::NeedsFullFrame() const
{
    return m_bNeedFull;
}

EBuildResult DrawListBuilder::Build(const TerminalFrame& frame, const TerminalTheme& theme, bool focused,
                                    bool cursorBlinkOn, int scrollOffsetPx, GlyphCache& cache, DrawList& out,
                                    DrawListBuildStats& stats)
{
    const CellMetrics& metrics = cache.GetMetrics();
    const Context context{theme, frame.background, frame.foreground, frame.iCols, &cache, metrics};
    if (!m_bHasContext || !(context == m_context))
    {
        DropCache();
        m_context = context;
        m_bHasContext = true;
    }
    if (cache.GetGeneration() != m_uCacheGeneration)
    {
        DropCache();
        m_uCacheGeneration = cache.GetGeneration();
    }
    const uint32_t uStartGeneration = cache.GetGeneration();
    const auto funcFail = [this]()
    {
        DropCache();
        return EBuildResult::NEED_FULL_FRAME;
    };

    std::unordered_map<uint64_t, CachedRow> hashNew;
    std::set<int> setDirty;
    DrawListBuildStats localStats;
    for (const TerminalRow& row : frame.vecRows)
    {
        const bool bRebuild = row.bDirty || frame.eDirty == EFrameDirty::FULL;
        if (!bRebuild)
        {
            const auto itOld = m_hashRows.find(row.llId);
            if (itOld == m_hashRows.end())
                return funcFail();
            CachedRow reused = std::move(itOld->second);
            if (reused.iViewportY != row.iViewportY)   // 옮겨진 행: 옛 자리와 새 자리 모두 다시 합성
            {
                setDirty.insert(reused.iViewportY);
                setDirty.insert(row.iViewportY);
            }
            reused.iViewportY = row.iViewportY;
            hashNew[row.llId] = std::move(reused);
            ++localStats.iReusedRows;
            continue;
        }
        CachedRow cached;
        BuildRowQuads(row, frame, theme, cache, cached.vecBackground, cached.vecGlyph, cached.vecDecoration);
        cached.vecCells = row.vecCells;
        cached.vecGraphemes = row.vecGraphemes;
        cached.iViewportY = row.iViewportY;
        hashNew[row.llId] = std::move(cached);
        setDirty.insert(row.iViewportY);
        ++localStats.iRebuiltRows;
    }

    // 커서·조합 글자는 행 캐시에 넣지 않고 매 프레임 장식 층 끝에 붙인다.
    std::vector<DrawQuad> vecOverlay;
    int iCursorRow = -1;
    int iPreeditRow = -1;
    const auto funcFindRow = [&](int viewportY) -> const TerminalRow*
    {
        for (const TerminalRow& row : frame.vecRows)
            if (row.iViewportY == viewportY)
                return &row;
        return nullptr;
    };
    if (frame.optPreedit)
    {
        AddPreedit(vecOverlay, frame, theme, cache);
        iPreeditRow = frame.cursor.iRow;
    }
    else if (frame.cursor.bVisible && cursorBlinkOn)
    {
        const TerminalRow* pRow = funcFindRow(frame.cursor.iRow);
        TerminalRow rowCopy;
        if (pRow)
        {
            const auto itRow = hashNew.find(pRow->llId);
            if (itRow != hashNew.end())
            {
                rowCopy.vecCells = itRow->second.vecCells;
                rowCopy.vecGraphemes = itRow->second.vecGraphemes;
                rowCopy.iSelectBegin = pRow->iSelectBegin;
                rowCopy.iSelectEnd = pRow->iSelectEnd;
            }
        }
        AddCursor(vecOverlay, frame, theme, focused, pRow ? &rowCopy : nullptr, cache);
        iCursorRow = frame.cursor.iRow;
    }
    if (cache.GetGeneration() != uStartGeneration)
        return funcFail();

    DrawList list;
    for (const TerminalRow& row : frame.vecRows)
    {
        const auto itRow = hashNew.find(row.llId);
        if (itRow == hashNew.end())
            continue;
        for (DrawQuad quad : itRow->second.vecBackground)
        {
            quad.iRow = int16_t(row.iViewportY);
            list.vecQuads.push_back(quad);
        }
    }
    list.iBackgroundCount = int(list.vecQuads.size());
    for (const TerminalRow& row : frame.vecRows)
    {
        const auto itRow = hashNew.find(row.llId);
        if (itRow == hashNew.end())
            continue;
        for (DrawQuad quad : itRow->second.vecGlyph)
        {
            quad.iRow = int16_t(row.iViewportY);
            list.vecQuads.push_back(quad);
        }
    }
    list.iGlyphCount = int(list.vecQuads.size()) - list.iBackgroundCount;
    for (const TerminalRow& row : frame.vecRows)
    {
        const auto itRow = hashNew.find(row.llId);
        if (itRow == hashNew.end())
            continue;
        for (DrawQuad quad : itRow->second.vecDecoration)
        {
            quad.iRow = int16_t(row.iViewportY);
            list.vecQuads.push_back(quad);
        }
    }
    const int iOverlayRow = frame.optPreedit ? iPreeditRow : iCursorRow;
    for (DrawQuad quad : vecOverlay)
    {
        quad.iRow = int16_t(iOverlayRow);
        list.vecQuads.push_back(quad);
    }
    list.iDecorationCount = int(list.vecQuads.size()) - list.iBackgroundCount - list.iGlyphCount;

    list.clearColor = frame.background;
    list.iCols = frame.iCols;
    list.iRows = frame.iRows;
    list.iPaddingPx = qRound(theme.iPaddingLogicalPx * metrics.dDevicePixelRatio);
    list.iCellWidth = metrics.iCellWidth;
    list.iCellHeight = metrics.iCellHeight;
    list.iScrollOffsetPx = scrollOffsetPx;
    list.uAtlasGeneration = cache.GetGeneration();
    list.bFullRedraw = frame.eDirty == EFrameDirty::FULL || !m_bHasPrevBuild || scrollOffsetPx != m_iPrevScrollOffsetPx;
    for (int iRow : {iCursorRow, m_iPrevCursorRow, iPreeditRow, m_iPrevPreeditRow})
        if (iRow >= 0)
            setDirty.insert(iRow);
    list.vecDirtyRows.assign(setDirty.begin(), setDirty.end());

    m_hashRows = std::move(hashNew);
    m_iPrevCursorRow = iCursorRow;
    m_iPrevPreeditRow = iPreeditRow;
    m_iPrevScrollOffsetPx = scrollOffsetPx;
    m_bHasPrevBuild = true;
    if (frame.eDirty == EFrameDirty::FULL)
        m_bNeedFull = false;
    stats = localStats;
    out = std::move(list);
    return EBuildResult::OK;
}
