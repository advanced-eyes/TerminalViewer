#include "TerminalViewer/Testing/FakeFrameSource.h"

#include <algorithm>
#include <random>

namespace
{
constexpr int kPaletteBasicCount = 16;
constexpr int kPaletteCubeEnd = 232;
constexpr int kPaletteCubeSide = 6;
constexpr int kPaletteCubeColors = 216;
constexpr int kPaletteCubeBase = 55;
constexpr int kPaletteCubeStep = 40;
constexpr int kPaletteGrayBase = 8;
constexpr int kPaletteGrayStep = 10;
constexpr uint8_t kAnsiDimLevel = 170;
constexpr uint8_t kAnsiBrightLevel = 255;
constexpr int kMixedKindCount = 9;
constexpr int kMixedStyleColorBase = 120;
constexpr int kMixedStyleColorRange = 136;
constexpr int kMixedVariantCount = 5;
constexpr int kMixedVariantStride = 5;
constexpr int kMixedBackgroundCols = 24;
constexpr int kMixedBackgroundPaletteSplit = 12;
constexpr int kMixedFamilyEmojiCol = 4;
constexpr int kMixedFamilyEmojiMinCols = 5;
constexpr int kMixedSelectionBegin = 1;
constexpr int kMixedSelectionEnd = 9;
constexpr int kByteRange = 256;
constexpr uint32_t kMixedSeedMul = 2654435761u;
constexpr uint32_t kMixedLineMul = 40503u;
constexpr uint32_t kMixedSeedAdd = 17u;
constexpr uint32_t kFloodSeedMul = 2246822519u;
constexpr uint32_t kFloodLineMul = 3266489917u;
constexpr uint32_t kFloodSeedAdd = 1u;
constexpr uint32_t kPrintableFirst = 0x21;
constexpr uint32_t kPrintableCount = 94;

bool IsWide(char32_t cp)
{
    return (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
           (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
           (cp >= 0x1F300 && cp <= 0x1FAFF);
}

TerminalRgb Palette256(int index)
{
    if (index < kPaletteBasicCount)
    {
        const uint8_t level = index < kPaletteGrayBase ? kAnsiDimLevel : kAnsiBrightLevel;
        return TerminalRgb{uint8_t((index & 1) ? level : 0), uint8_t((index & 2) ? level : 0), uint8_t((index & 4) ? level : 0)};
    }
    if (index < kPaletteCubeEnd)
    {
        const int iCube = index - kPaletteBasicCount;
        const auto funcLevel = [](int step) { return uint8_t(step == 0 ? 0 : kPaletteCubeBase + step * kPaletteCubeStep); };
        return TerminalRgb{funcLevel(iCube / (kPaletteCubeSide * kPaletteCubeSide)), funcLevel((iCube / kPaletteCubeSide) % kPaletteCubeSide),
                           funcLevel(iCube % kPaletteCubeSide)};
    }
    const uint8_t gray = uint8_t(kPaletteGrayBase + (index - kPaletteCubeEnd) * kPaletteGrayStep);
    return TerminalRgb{gray, gray, gray};
}
} // namespace

FakeFrameSource::FakeFrameSource(int cols, int rows, int scrollbackLines)
    : m_iCols(std::max(1, cols))
    , m_iRows(std::max(1, rows))
    , m_iScrollback(std::max(0, scrollbackLines))
{
    m_vecLines.resize(m_iScrollback + m_iRows);
    for (Line& line : m_vecLines)
    {
        line.vecCells.assign(m_iCols, TerminalCell{});
    }
    m_iViewportTop = m_iScrollback;
}

void FakeFrameSource::ClearLine(Line& line)
{
    line.vecCells.assign(m_iCols, TerminalCell{});
    line.vecGraphemes.clear();
    line.iSelectBegin = -1;
    line.iSelectEnd = -1;
    line.uVersion = ++m_uNextVersion;
}

void FakeFrameSource::SetTextLocked(int line, int col, const std::u32string& text, const TerminalCell& style)
{
    if (line < 0 || line >= int(m_vecLines.size()))
    {
        return;
    }
    Line& target = m_vecLines[line];
    int iCol = col;
    for (const char32_t codepoint : text)
    {
        const bool bWide = IsWide(codepoint);
        if (iCol < 0 || iCol + (bWide ? 1 : 0) >= m_iCols)
        {
            break;
        }
        TerminalCell cell = style;
        cell.uCodepoint = uint32_t(codepoint);
        cell.eWidth = bWide ? ECellWidth::WIDE : ECellWidth::NARROW;
        target.vecCells[iCol] = cell;
        if (bWide)
        {
            TerminalCell tail = style;
            tail.uCodepoint = 0;
            tail.eWidth = ECellWidth::SPACER_TAIL;
            target.vecCells[iCol + 1] = tail;
        }
        iCol += bWide ? 2 : 1;
    }
    target.uVersion = ++m_uNextVersion;
}

void FakeFrameSource::SetText(int line, int col, const std::u32string& text, const TerminalCell& style)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SetTextLocked(line, col, text, style);
}

void FakeFrameSource::SetCell(int line, int col, const TerminalCell& cell)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (line < 0 || line >= int(m_vecLines.size()) || col < 0 || col >= m_iCols)
    {
        return;
    }
    m_vecLines[line].vecCells[col] = cell;
    m_vecLines[line].uVersion = ++m_uNextVersion;
}

void FakeFrameSource::SetCursor(const TerminalCursor& cursor)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cursor = cursor;
    m_bMetaDirty = true;
}

void FakeFrameSource::SetPreedit(const std::optional<TerminalPreedit>& preedit)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_optPreedit = preedit;
    m_bMetaDirty = true;
}

void FakeFrameSource::SetSelection(int line, int begin, int end)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (line < 0 || line >= int(m_vecLines.size()))
    {
        return;
    }
    m_vecLines[line].iSelectBegin = begin;
    m_vecLines[line].iSelectEnd = end;
    m_vecLines[line].uVersion = ++m_uNextVersion;
}

void FakeFrameSource::SetColors(TerminalRgb background, TerminalRgb foreground)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_background = background;
    m_foreground = foreground;
    m_bMetaDirty = true;
}

void FakeFrameSource::FillMixedLine(int lineNo, uint32_t seed)
{
    Line& target = m_vecLines[lineNo];
    ClearLine(target);
    std::mt19937 rng(seed * kMixedSeedMul + uint32_t(lineNo) * kMixedLineMul + kMixedSeedAdd);
    const auto funcPick = [&rng](int count) { return int(rng() % uint32_t(count)); };
    TerminalCell style;
    style.fg = TerminalRgb{uint8_t(kMixedStyleColorBase + funcPick(kMixedStyleColorRange)), uint8_t(kMixedStyleColorBase + funcPick(kMixedStyleColorRange)),
                           uint8_t(kMixedStyleColorBase + funcPick(kMixedStyleColorRange))};
    switch (lineNo % kMixedKindCount)
    {
    case 0:
        SetTextLocked(lineNo, 0, U"The quick brown fox jumps over 0123456789", style);
        break;
    case 1:
    {
        // 굵게, 기울임, 흐리게, 반전, 취소선
        int iCol = 0;
        for (int iVariant = 0; iVariant < kMixedVariantCount; ++iVariant)
        {
            TerminalCell variantStyle = style;
            variantStyle.bBold = iVariant == 0;
            variantStyle.bItalic = iVariant == 1;
            variantStyle.bFaint = iVariant == 2;
            variantStyle.bInverse = iVariant == 3;
            variantStyle.bStrikethrough = iVariant == 4;
            SetTextLocked(lineNo, iCol, U"Text", variantStyle);
            iCol += kMixedVariantStride;
        }
        break;
    }
    case 2:
    {
        // 밑줄 5종
        const EUnderline arrKinds[] = {EUnderline::SINGLE, EUnderline::DOUBLE, EUnderline::CURLY, EUnderline::DOTTED, EUnderline::DASHED};
        int iCol = 0;
        for (const EUnderline eKind : arrKinds)
        {
            TerminalCell underlineStyle = style;
            underlineStyle.eUnderline = eKind;
            underlineStyle.underlineColor = TerminalRgb{uint8_t(funcPick(kByteRange)), 200, uint8_t(funcPick(kByteRange))};
            SetTextLocked(lineNo, iCol, U"Line", underlineStyle);
            iCol += kMixedVariantStride;
        }
        break;
    }
    case 3:
        SetTextLocked(lineNo, 0, U"한글 테스트 안녕하세요", style);
        break;
    case 4:
    {
        SetTextLocked(lineNo, 0, U"\U0001F600", style);
        // 가족 이모지: 첫 코드포인트 + 나머지는 grapheme
        if (m_iCols > kMixedFamilyEmojiMinCols)
        {
            TerminalCell cell = style;
            cell.uCodepoint = 0x1F468;
            cell.eWidth = ECellWidth::WIDE;
            cell.iGrapheme = 0;
            target.vecGraphemes.push_back(U"\U0001F468‍\U0001F469‍\U0001F467");
            target.vecCells[kMixedFamilyEmojiCol] = cell;
            TerminalCell tail = style;
            tail.uCodepoint = 0;
            tail.eWidth = ECellWidth::SPACER_TAIL;
            target.vecCells[kMixedFamilyEmojiCol + 1] = tail;
        }
        break;
    }
    case 5:
        SetTextLocked(lineNo, 0, U"┌─┐│└┘╭╮╯╰═║", style);
        break;
    case 6:
        SetTextLocked(lineNo, 0, U"█▀▄░▒▓", style);
        break;
    case 7:
        for (int iCol = 0; iCol < std::min(m_iCols, kMixedBackgroundCols); ++iCol)
        {
            TerminalCell cell = style;
            cell.uCodepoint = iCol % 2 ? ' ' : 'x';
            cell.bBgDefault = false;
            cell.bg = (iCol < kMixedBackgroundPaletteSplit)
                          ? Palette256(kPaletteBasicCount + funcPick(kPaletteCubeColors))
                          : TerminalRgb{uint8_t(funcPick(kByteRange)), uint8_t(funcPick(kByteRange)), uint8_t(funcPick(kByteRange))};
            target.vecCells[iCol] = cell;
        }
        break;
    default:
        SetTextLocked(lineNo, 0, U"mixed ABC abc 한글 █", style);
        break;
    }
}

void FakeFrameSource::FillPattern(EFakePattern pattern, uint32_t seed)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (int i = 0; i < int(m_vecLines.size()); ++i)
    {
        Line& target = m_vecLines[i];
        switch (pattern)
        {
        case EFakePattern::BLANK:
            ClearLine(target);
            break;
        case EFakePattern::MIXED:
            FillMixedLine(i, seed);
            break;
        case EFakePattern::FLOOD:
        {
            ClearLine(target);
            std::mt19937 rng(seed * kFloodSeedMul + uint32_t(i) * kFloodLineMul + kFloodSeedAdd);
            for (int iCol = 0; iCol < m_iCols; ++iCol)
            {
                TerminalCell cell;
                cell.uCodepoint = uint32_t(kPrintableFirst + rng() % kPrintableCount);
                cell.fg = TerminalRgb{uint8_t(rng()), uint8_t(rng()), uint8_t(rng())};
                cell.bg = TerminalRgb{uint8_t(rng()), uint8_t(rng()), uint8_t(rng())};
                cell.bBgDefault = false;
                target.vecCells[iCol] = cell;
            }
            break;
        }
        }
    }
    if (pattern == EFakePattern::MIXED)
    {
        // 화면 가운데쯤 한 줄을 선택 상태로 둔다.
        const int iSelected = m_iViewportTop + m_iRows / 2;
        m_vecLines[iSelected].iSelectBegin = kMixedSelectionBegin;
        m_vecLines[iSelected].iSelectEnd = std::min(m_iCols, kMixedSelectionEnd);
    }
}

void FakeFrameSource::Resize(int cols, int rows)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_iCols = std::max(1, cols);
    m_iRows = std::max(1, rows);
    const size_t uTotal = size_t(m_iScrollback + m_iRows);
    m_vecLines.resize(uTotal);
    for (Line& line : m_vecLines)
    {
        line.vecCells.resize(m_iCols, TerminalCell{});
        line.uVersion = ++m_uNextVersion;
    }
    m_iViewportTop = std::min(m_iViewportTop, int(uTotal) - m_iRows);
    m_bNeedFull = true;
}

int FakeFrameSource::GetViewportTop() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_iViewportTop;
}

int FakeFrameSource::GetCaptureCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_iCaptureCount;
}

TerminalCaptureRequest FakeFrameSource::GetLastRequest() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_lastRequest;
}

int FakeFrameSource::ScrollViewport(int rows)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const int iMaxTop = int(m_vecLines.size()) - m_iRows;
    const int iNewTop = std::clamp(m_iViewportTop + rows, 0, iMaxTop);
    const int iMoved = iNewTop - m_iViewportTop;
    m_iViewportTop = iNewTop;
    if (iMoved != 0)
    {
        m_bAllDirty = true;
    }
    return iMoved;
}

void FakeFrameSource::CaptureFrame(const TerminalCaptureRequest& request, TerminalFrame& frame)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_iCaptureCount;
    m_lastRequest = request;

    const bool bFull = request.bFull || m_bNeedFull;
    const int iFirst = std::max(0, m_iViewportTop - std::max(0, request.iOverscanAbove));
    const int iLast = std::min(int(m_vecLines.size()) - 1, m_iViewportTop + m_iRows - 1 + std::max(0, request.iOverscanBelow));

    frame.iCols = m_iCols;
    frame.iRows = m_iRows;
    frame.background = m_background;
    frame.foreground = m_foreground;
    frame.cursor = m_cursor;
    frame.optPreedit = m_optPreedit;
    frame.vecRows.clear();

    std::unordered_map<int, uint32_t> hashSent;
    int iDirtyRows = 0;
    for (int iLine = iFirst; iLine <= iLast; ++iLine)
    {
        const Line& source = m_vecLines[iLine];
        const auto it = m_hashSentVersions.find(iLine);
        const bool bDirty = bFull || m_bAllDirty || it == m_hashSentVersions.end() || it->second != source.uVersion;
        TerminalRow row;
        row.llId = (uint64_t(uint32_t(iLine)) << 32) | source.uVersion;
        row.iViewportY = iLine - m_iViewportTop;
        row.bDirty = bDirty;
        row.iSelectBegin = source.iSelectBegin;
        row.iSelectEnd = source.iSelectEnd;
        if (bDirty || bFull)
        {
            row.vecCells = source.vecCells;
            row.vecGraphemes = source.vecGraphemes;
        }
        iDirtyRows += bDirty ? 1 : 0;
        hashSent[iLine] = source.uVersion;
        frame.vecRows.push_back(std::move(row));
    }
    m_hashSentVersions = std::move(hashSent);

    if (bFull)
    {
        frame.eDirty = EFrameDirty::FULL;
    }
    else if (iDirtyRows == 0 && !m_bMetaDirty)
    {
        frame.eDirty = EFrameDirty::NONE;
    }
    else
    {
        frame.eDirty = EFrameDirty::PARTIAL;
    }
    m_bNeedFull = false;
    m_bAllDirty = false;
    m_bMetaDirty = false;
}
