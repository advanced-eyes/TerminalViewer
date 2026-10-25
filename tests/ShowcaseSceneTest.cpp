#include "ShowcaseScene.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/BoxGlyphPainter.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalContrast.h"
#include "TerminalViewer/Testing/FakeFrameSource.h"

#include <QString>
#include <QTest>

#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace
{
constexpr int kCols = 100;
constexpr int kRows = 30;
constexpr int kScrollback = ShowcaseScene::kScrollbackLines;
constexpr int kLabelColumns = 11;   // 라벨 칸(라벨 + 여백). 그 뒤가 절의 내용

TerminalCaptureRequest MakeFullRequest()
{
    TerminalCaptureRequest request;
    request.iOverscanAbove = kScrollback;   // 스크롤백 맨 위까지 모두
    request.bFull = true;
    return request;
}

TerminalCaptureRequest MakeChangesRequest()
{
    return TerminalCaptureRequest();
}

QString ToQString(const std::u32string& text)
{
    return QString::fromUcs4(text.data(), int(text.size()));
}

std::u32string GetRowText(const TerminalRow& row)
{
    std::u32string strText;
    for (const TerminalCell& cell : row.vecCells)
    {
        if (cell.eWidth == ECellWidth::SPACER_TAIL)
            continue;
        if (cell.iGrapheme >= 0 && cell.iGrapheme < int(row.vecGraphemes.size()))
            strText += row.vecGraphemes[size_t(cell.iGrapheme)];
        else
            strText += cell.uCodepoint ? char32_t(cell.uCodepoint) : U' ';
    }
    return strText;
}

const TerminalRow* FindRow(const TerminalFrame& frame, int viewportTop, int line)
{
    for (const TerminalRow& row : frame.vecRows)
    {
        if (row.iViewportY == line - viewportTop)
            return &row;
    }
    return nullptr;
}

int CountDirtyRows(const TerminalFrame& frame)
{
    int iCount = 0;
    for (const TerminalRow& row : frame.vecRows)
        iCount += row.bDirty ? 1 : 0;
    return iCount;
}

std::tuple<int, int, int> MakeColorKey(const TerminalRgb& rgb)
{
    return std::make_tuple(int(rgb.r), int(rgb.g), int(rgb.b));
}

bool IsSameCell(const TerminalCell& a, const TerminalCell& b)
{
    return a.uCodepoint == b.uCodepoint && a.iGrapheme == b.iGrapheme && a.fg == b.fg && a.bg == b.bg &&
           a.underlineColor == b.underlineColor && a.bBgDefault == b.bBgDefault && a.bBold == b.bBold && a.bItalic == b.bItalic &&
           a.bFaint == b.bFaint && a.bInverse == b.bInverse && a.bInvisible == b.bInvisible && a.bStrikethrough == b.bStrikethrough &&
           a.eUnderline == b.eUnderline && a.eWidth == b.eWidth;
}

// 공급원이 돌려준 줄이 장면이 쓴 내용과 같은가. 다르면 첫 차이를 적은 문장
QString CompareWithScene(const ShowcaseScene& scene, const TerminalFrame& frame, int viewportTop)
{
    const std::vector<ShowcaseLine>& vecLines = scene.GetLines();
    for (int iLine = 0; iLine < int(vecLines.size()); ++iLine)
    {
        const TerminalRow* pRow = FindRow(frame, viewportTop, iLine);
        if (!pRow)
            return QStringLiteral("line %1 not captured").arg(iLine);
        const ShowcaseLine& line = vecLines[size_t(iLine)];
        if (int(line.vecCells.size()) > frame.iCols)
            return QStringLiteral("line %1 has %2 cells for %3 columns").arg(iLine).arg(line.vecCells.size()).arg(frame.iCols);
        if (pRow->vecGraphemes != line.vecGraphemes)
            return QStringLiteral("line %1 graphemes differ").arg(iLine);
        for (int iCol = 0; iCol < frame.iCols; ++iCol)
        {
            const TerminalCell expected = iCol < int(line.vecCells.size()) ? line.vecCells[size_t(iCol)] : TerminalCell();
            if (!IsSameCell(pRow->vecCells[size_t(iCol)], expected))
                return QStringLiteral("line %1 col %2 differs: '%3'").arg(iLine).arg(iCol).arg(ToQString(GetRowText(*pRow)));
        }
    }
    return QString();
}

// 2칸 글자의 짝: WIDE 다음은 SPACER_TAIL, SPACER_TAIL 앞은 WIDE, 마지막 열에 WIDE 없음, 글자 인덱스가 범위 안
QString CheckWidePairs(const TerminalRow& row, int cols)
{
    for (int iCol = 0; iCol < int(row.vecCells.size()); ++iCol)
    {
        const TerminalCell& cell = row.vecCells[size_t(iCol)];
        if (cell.eWidth == ECellWidth::WIDE && (iCol + 1 >= cols || row.vecCells[size_t(iCol + 1)].eWidth != ECellWidth::SPACER_TAIL))
            return QStringLiteral("WIDE without tail at col %1: '%2'").arg(iCol).arg(ToQString(GetRowText(row)));
        if (cell.eWidth == ECellWidth::SPACER_TAIL && (iCol == 0 || row.vecCells[size_t(iCol - 1)].eWidth != ECellWidth::WIDE))
            return QStringLiteral("tail without WIDE at col %1: '%2'").arg(iCol).arg(ToQString(GetRowText(row)));
        if (cell.iGrapheme >= int(row.vecGraphemes.size()))
            return QStringLiteral("grapheme index %1 out of range at col %2").arg(cell.iGrapheme).arg(iCol);
    }
    return QString();
}

struct Fixture
{
    Fixture(int cols, int rows)
        : source(cols, rows, kScrollback)
        , scene()
        , frame()
    {
        scene.Fill(source, cols, rows);
        source.CaptureFrame(MakeFullRequest(), frame);
    }

    const TerminalRow& GetRow(EShowcaseSection section, int offset = 0) const
    {
        static const TerminalRow s_emptyRow;
        const int iLine = scene.GetSectionLine(section);
        const TerminalRow* pRow = iLine < 0 ? nullptr : FindRow(frame, source.GetViewportTop(), iLine + offset);
        return pRow ? *pRow : s_emptyRow;
    }

    FakeFrameSource source;
    ShowcaseScene scene;
    TerminalFrame frame;
};

struct LabelCase
{
    EShowcaseSection eSection;
    const char32_t* pLabel;
};

constexpr LabelCase kLabels[] = {
    {EShowcaseSection::ANSI_FG, U"ansi fg"},     {EShowcaseSection::ANSI_BG, U"ansi bg"},
    {EShowcaseSection::PALETTE_256, U"256 color"}, {EShowcaseSection::TRUECOLOR, U"truecolor"},
    {EShowcaseSection::STYLES, U"styles"},       {EShowcaseSection::CONTRAST, U"contrast"},
    {EShowcaseSection::UNDERLINE, U"underline"},
    {EShowcaseSection::WIDE, U"wide"},           {EShowcaseSection::EMOJI, U"emoji"},
    {EShowcaseSection::BOX, U"box"},             {EShowcaseSection::BLOCKS, U"blocks"},
    {EShowcaseSection::SELECTION, U"selection"}, {EShowcaseSection::LIVE, U"live"},
};
}   // namespace

class ShowcaseSceneTest : public QObject
{
    Q_OBJECT

private slots:
    void everySectionIsInTheFirstScreen()
    {
        Fixture fixture(kCols, kRows);
        const int iTop = fixture.source.GetViewportTop();
        QCOMPARE(iTop, kScrollback);
        int iPrevious = -1;
        for (int iSection = 0; iSection < int(EShowcaseSection::COUNT); ++iSection)
        {
            const EShowcaseSection eSection = EShowcaseSection(iSection);
            const int iLine = fixture.scene.GetSectionLine(eSection);
            const int iCount = fixture.scene.GetSectionLineCount(eSection);
            QVERIFY2(iLine >= iTop && iLine + iCount <= iTop + kRows, qPrintable(QStringLiteral("section %1 at %2 (+%3)").arg(iSection).arg(iLine).arg(iCount)));
            QVERIFY2(iCount >= 1, qPrintable(QStringLiteral("section %1").arg(iSection)));
            QVERIFY(iLine > iPrevious);
            iPrevious = iLine;
        }
        QCOMPARE(fixture.scene.GetSectionLine(EShowcaseSection::PROMPT), kScrollback + kRows - 1);
        QCOMPARE(fixture.scene.GetSectionLine(EShowcaseSection::LIVE), kScrollback + kRows - 2);
        QCOMPARE(fixture.scene.GetSectionLineCount(EShowcaseSection::PALETTE_256), 3);
        QCOMPARE(fixture.scene.GetSectionLineCount(EShowcaseSection::BOX), 3);
        QCOMPARE(fixture.scene.GetSectionLineCount(EShowcaseSection::TITLE), 3);
    }

    void sectionsHaveLabels()
    {
        Fixture fixture(kCols, kRows);
        for (const LabelCase& label : kLabels)
        {
            const std::u32string strText = GetRowText(fixture.GetRow(label.eSection));
            const std::u32string strLabel(label.pLabel);
            QVERIFY2(strText.compare(0, strLabel.size(), strLabel) == 0, qPrintable(ToQString(strText)));
        }
        QVERIFY(ToQString(GetRowText(fixture.GetRow(EShowcaseSection::TITLE, 1))).contains(QStringLiteral("TerminalViewer")));
        QVERIFY(ToQString(GetRowText(fixture.GetRow(EShowcaseSection::PROMPT))).startsWith(QStringLiteral("user@terminalviewer:~$ echo 안녕")));
    }

    void capturedLinesMatchTheScene()
    {
        Fixture fixture(kCols, kRows);
        QCOMPARE(int(fixture.scene.GetLines().size()), kScrollback + kRows);
        QCOMPARE(int(fixture.frame.vecRows.size()), kScrollback + kRows);
        const QString strDiff = CompareWithScene(fixture.scene, fixture.frame, fixture.source.GetViewportTop());
        QVERIFY2(strDiff.isEmpty(), qPrintable(strDiff));
        for (const TerminalRow& row : fixture.frame.vecRows)
        {
            const QString strPairs = CheckWidePairs(row, kCols);
            QVERIFY2(strPairs.isEmpty(), qPrintable(strPairs));
        }
    }

    void cjkTextIsWideWithSpacerTail()
    {
        Fixture fixture(kCols, kRows);
        std::set<uint32_t> setWide;
        const int iWideLines = fixture.scene.GetSectionLineCount(EShowcaseSection::WIDE);
        for (int iOffset = 0; iOffset < iWideLines; ++iOffset)
        {
            const TerminalRow& row = fixture.GetRow(EShowcaseSection::WIDE, iOffset);
            for (size_t uCol = 0; uCol + 1 < row.vecCells.size(); ++uCol)
            {
                if (row.vecCells[uCol].eWidth == ECellWidth::WIDE && row.vecCells[uCol + 1].eWidth == ECellWidth::SPACER_TAIL)
                    setWide.insert(row.vecCells[uCol].uCodepoint);
            }
        }
        QVERIFY(setWide.count(0xD55C));   // 한
        QVERIFY(setWide.count(0xC548));   // 안
        QVERIFY(setWide.count(0x65E5));   // 日
        QVERIFY(setWide.count(0x3072));   // ひ
        QVERIFY(setWide.count(0x30AB));   // カ
        QVERIFY(setWide.count(0x4E2D));   // 中
        // 프롬프트의 "안녕" 도 2칸이다
        const TerminalRow& prompt = fixture.GetRow(EShowcaseSection::PROMPT);
        const int iAn = fixture.scene.GetPromptWidth() - 4;
        QCOMPARE(prompt.vecCells[size_t(iAn)].uCodepoint, uint32_t(0xC548));
        QCOMPARE(prompt.vecCells[size_t(iAn)].eWidth, ECellWidth::WIDE);
        QCOMPARE(prompt.vecCells[size_t(iAn + 1)].eWidth, ECellWidth::SPACER_TAIL);
    }

    // 회색 · 흰색뿐인 컬러 이모지(흰 하트 · 흰 깃발)는 다른 이모지와 다른 글자색(빨강)으로 넣는다:
    // 렌더러가 단색으로 잘못 판정하면 그 글자색 실루엣으로 보여 눈으로 알 수 있다.
    // 흰 깃발 U+1F3F3 은 혼자면 텍스트 표시(1칸)라 U+FE0F 를 붙인 2칸 글자(vecGraphemes)로 넣는다
    void grayEmojiHaveTintedForeground()
    {
        Fixture fixture(kCols, kRows);
        const TerminalRow& row = fixture.GetRow(EShowcaseSection::EMOJI);
        const std::u32string strWhiteFlag = U"\U0001F3F3\uFE0F";
        std::map<std::u32string, TerminalRgb> mapForegrounds;   // 칸의 글자(그래핌이면 그 전체) → 글자색
        for (size_t uCol = 0; uCol + 1 < row.vecCells.size(); ++uCol)
        {
            const TerminalCell& cell = row.vecCells[uCol];
            if (cell.eWidth != ECellWidth::WIDE)
                continue;
            QCOMPARE(row.vecCells[uCol + 1].eWidth, ECellWidth::SPACER_TAIL);
            const std::u32string strText = cell.iGrapheme >= 0 ? row.vecGraphemes[size_t(cell.iGrapheme)]
                                                               : std::u32string(1, char32_t(cell.uCodepoint));
            QCOMPARE(cell.uCodepoint, uint32_t(strText.front()));
            mapForegrounds[strText] = cell.fg;
        }
        QVERIFY(mapForegrounds.count(U"\U0001F600"));   // 😀 (보통 글자색)
        QVERIFY(mapForegrounds.count(U"\U0001F90D"));   // 🤍
        QVERIFY(mapForegrounds.count(strWhiteFlag));    // 🏳️
        QVERIFY(!mapForegrounds.count(U"\U0001F3F3"));   // 혼자 쓴 🏳 를 2칸에 넣지 않는다
        QVERIFY(mapForegrounds[U"\U0001F90D"] != mapForegrounds[U"\U0001F600"]);
        QVERIFY(mapForegrounds[strWhiteFlag] == mapForegrounds[U"\U0001F90D"]);
    }

    void zwjEmojiUsesGraphemes()
    {
        Fixture fixture(kCols, kRows);
        const TerminalRow& row = fixture.GetRow(EShowcaseSection::EMOJI);
        const std::u32string strFamily = U"\U0001F468\u200D\U0001F469\u200D\U0001F467";   // \u200D = ZWJ
        bool bFamily = false;
        bool bSingle = false;
        for (size_t uCol = 0; uCol + 1 < row.vecCells.size(); ++uCol)
        {
            const TerminalCell& cell = row.vecCells[uCol];
            if (cell.iGrapheme >= 0 && row.vecGraphemes[size_t(cell.iGrapheme)] == strFamily)
            {
                bFamily = true;
                QCOMPARE(cell.uCodepoint, uint32_t(0x1F468));
                QCOMPARE(cell.eWidth, ECellWidth::WIDE);
                QCOMPARE(row.vecCells[uCol + 1].eWidth, ECellWidth::SPACER_TAIL);
            }
            if (cell.uCodepoint == 0x1F600 && cell.iGrapheme < 0)
            {
                bSingle = true;
                QCOMPARE(cell.eWidth, ECellWidth::WIDE);
            }
        }
        QVERIFY(bFamily);
        QVERIFY(bSingle);
        QVERIFY(row.vecGraphemes.size() >= 3);   // 가족 + 다른 ZWJ · 피부색 조합
    }

    void stylesAndUnderlines()
    {
        Fixture fixture(kCols, kRows);
        bool bBold = false, bItalic = false, bBoldItalic = false, bFaint = false, bInverse = false, bInvisible = false, bStrike = false;
        for (const TerminalCell& cell : fixture.GetRow(EShowcaseSection::STYLES).vecCells)
        {
            if (cell.uCodepoint == 0 || cell.uCodepoint == ' ')
                continue;
            bBold = bBold || (cell.bBold && !cell.bItalic);
            bItalic = bItalic || (cell.bItalic && !cell.bBold);
            bBoldItalic = bBoldItalic || (cell.bBold && cell.bItalic);
            bFaint = bFaint || cell.bFaint;
            bInverse = bInverse || cell.bInverse;
            bInvisible = bInvisible || cell.bInvisible;
            bStrike = bStrike || cell.bStrikethrough;
        }
        QVERIFY(bBold && bItalic && bBoldItalic && bFaint && bInverse && bInvisible && bStrike);

        std::map<EUnderline, TerminalRgb> mapUnderlines;
        for (const TerminalCell& cell : fixture.GetRow(EShowcaseSection::UNDERLINE).vecCells)
        {
            if (cell.eUnderline == EUnderline::NONE)
                continue;
            QVERIFY(cell.underlineColor != cell.fg);   // 색 밑줄
            mapUnderlines[cell.eUnderline] = cell.underlineColor;
        }
        QCOMPARE(int(mapUnderlines.size()), 5);
        std::set<std::tuple<int, int, int>> setColors;
        for (const auto& [eKind, rgb] : mapUnderlines)
            setColors.insert(MakeColorKey(rgb));
        QCOMPARE(int(setColors.size()), 5);
    }

    // 최소 대비(M 키, --min-contrast)를 보이는 줄: 모든 글자가 대비 3 에도 못 미친다(켜면 4.5 로 고쳐진다).
    // 어두운 배경(밝게 고침) · 밝은 배경(어둡게 고침) · 기본 배경 · 흐리게를 모두 담는다
    void contrastSectionShowsLowContrastText()
    {
        Fixture fixture(kCols, kRows);
        QCOMPARE(fixture.scene.GetSectionLineCount(EShowcaseSection::CONTRAST), 1);
        const TerminalRow& row = fixture.GetRow(EShowcaseSection::CONTRAST);
        std::set<std::tuple<int, int, int, int, int, int>> setPairs;
        bool bLightBackground = false;
        bool bDarkBackground = false;
        bool bDefaultBackground = false;
        bool bFaint = false;
        int iGlyphs = 0;
        for (int iCol = kLabelColumns; iCol < int(row.vecCells.size()); ++iCol)
        {
            const TerminalCell& cell = row.vecCells[size_t(iCol)];
            if (cell.uCodepoint == 0 || cell.uCodepoint == ' ')
                continue;
            ++iGlyphs;
            const TerminalRgb bg = cell.bBgDefault ? ShowcaseScene::kBackground : cell.bg;
            const double dRatio = GetContrastRatio(cell.fg, bg);
            QVERIFY2(dRatio < 3.0, qPrintable(QStringLiteral("col %1 contrast %2").arg(iCol).arg(dRatio)));
            QVERIFY(!IsContrastExempt(char32_t(cell.uCodepoint)));
            setPairs.insert(std::tuple_cat(MakeColorKey(cell.fg), MakeColorKey(bg)));
            bLightBackground = bLightBackground || GetRelativeLuminance(bg) > 0.5;
            bDarkBackground = bDarkBackground || (!cell.bBgDefault && GetRelativeLuminance(bg) < 0.1);
            bDefaultBackground = bDefaultBackground || cell.bBgDefault;
            bFaint = bFaint || cell.bFaint;
        }
        QVERIFY(iGlyphs >= 20);
        QVERIFY2(setPairs.size() >= 5, qPrintable(QString::number(setPairs.size())));
        QVERIFY(bLightBackground);
        QVERIFY(bDarkBackground);
        QVERIFY(bDefaultBackground);
        QVERIFY(bFaint);
    }

    void colorSections()
    {
        Fixture fixture(kCols, kRows);
        std::set<std::tuple<int, int, int>> setFg;
        for (const TerminalCell& cell : fixture.GetRow(EShowcaseSection::ANSI_FG).vecCells)
        {
            if (cell.uCodepoint != 0 && cell.uCodepoint != ' ')
                setFg.insert(MakeColorKey(cell.fg));
        }
        QVERIFY2(setFg.size() >= 17, qPrintable(QString::number(setFg.size())));   // 16색 + 라벨색

        std::set<std::tuple<int, int, int>> setBg;
        for (const TerminalCell& cell : fixture.GetRow(EShowcaseSection::ANSI_BG).vecCells)
        {
            if (!cell.bBgDefault)
                setBg.insert(MakeColorKey(cell.bg));
        }
        QCOMPARE(int(setBg.size()), 16);

        // 256색: 위 반칸(글자색) + 아래 반칸(배경색) 으로 3줄에 모두 넣는다. xterm 팔레트는 16색 중 7개가 6x6x6 과 겹친다
        std::set<std::tuple<int, int, int>> setPalette;
        for (int iOffset = 0; iOffset < 3; ++iOffset)
        {
            for (const TerminalCell& cell : fixture.GetRow(EShowcaseSection::PALETTE_256, iOffset).vecCells)
            {
                if (cell.uCodepoint == 0x2580)
                {
                    QVERIFY(!cell.bBgDefault);
                    setPalette.insert(MakeColorKey(cell.fg));
                    setPalette.insert(MakeColorKey(cell.bg));
                }
            }
        }
        QVERIFY2(setPalette.size() >= 249, qPrintable(QString::number(setPalette.size())));

        std::set<std::tuple<int, int, int>> setTrue;
        for (const TerminalCell& cell : fixture.GetRow(EShowcaseSection::TRUECOLOR).vecCells)
        {
            if (cell.uCodepoint == 0x2580)
            {
                setTrue.insert(MakeColorKey(cell.fg));
                setTrue.insert(MakeColorKey(cell.bg));
            }
        }
        QVERIFY2(setTrue.size() >= 150, qPrintable(QString::number(setTrue.size())));
    }

    void boxAndBlockGlyphsAreDrawnByBoxGlyphPainter()
    {
        Fixture fixture(kCols, kRows);
        std::set<uint32_t> setSeen;
        const auto funcCollect = [&](const TerminalRow& row, int fromCol)
        {
            for (int iCol = fromCol; iCol < int(row.vecCells.size()); ++iCol)
            {
                const uint32_t uCodepoint = row.vecCells[size_t(iCol)].uCodepoint;
                if (uCodepoint == 0 || uCodepoint == ' ')
                    continue;
                QVERIFY2(BoxGlyphPainter::Handles(char32_t(uCodepoint)), qPrintable(QStringLiteral("U+%1").arg(uCodepoint, 4, 16)));
                setSeen.insert(uCodepoint);
            }
        };
        for (int iOffset = 0; iOffset < 3; ++iOffset)
            funcCollect(fixture.GetRow(EShowcaseSection::BOX, iOffset), kLabelColumns);
        funcCollect(fixture.GetRow(EShowcaseSection::BLOCKS), kLabelColumns);
        for (const uint32_t uWanted : {0x253Cu, 0x256Cu, 0x254Bu, 0x256Du, 0x2573u, 0x2588u, 0x2580u, 0x2584u, 0x2591u, 0x2592u, 0x2593u, 0x259Au, 0x2581u, 0x258Fu})
            QVERIFY2(setSeen.count(uWanted), qPrintable(QStringLiteral("U+%1").arg(uWanted, 4, 16)));
        // 제목 틀도 상자 문자다
        const TerminalRow& top = fixture.GetRow(EShowcaseSection::TITLE);
        QCOMPARE(top.vecCells[0].uCodepoint, uint32_t(0x256D));
        QVERIFY(BoxGlyphPainter::Handles(char32_t(top.vecCells[1].uCodepoint)));
    }

    void selectionCoversOneLine()
    {
        Fixture fixture(kCols, kRows);
        int iSelectedRows = 0;
        for (const TerminalRow& row : fixture.frame.vecRows)
            iSelectedRows += row.iSelectBegin >= 0 ? 1 : 0;
        QCOMPARE(iSelectedRows, 1);
        const TerminalRow& row = fixture.GetRow(EShowcaseSection::SELECTION);
        const auto [iBegin, iEnd] = fixture.scene.GetSelection();
        QCOMPARE(row.iSelectBegin, iBegin);
        QCOMPARE(row.iSelectEnd, iEnd);
        QVERIFY(iBegin >= kLabelColumns && iEnd > iBegin && iEnd <= kCols);
        std::u32string strSelected;
        for (int iCol = iBegin; iCol < iEnd; ++iCol)
            strSelected += char32_t(row.vecCells[size_t(iCol)].uCodepoint);
        QCOMPARE(ToQString(strSelected), QStringLiteral("quick brown fox"));
    }

    void promptHasBlinkingCursorAndPreedit()
    {
        Fixture fixture(kCols, kRows);
        const TerminalCursor& cursor = fixture.frame.cursor;
        QVERIFY(cursor.bVisible);
        QVERIFY(cursor.bBlinking);
        QCOMPARE(cursor.eShape, ECursorShape::BLOCK);
        QCOMPARE(cursor.iRow, kRows - 1);
        QCOMPARE(fixture.scene.GetPromptWidth(), 32);   // "user@terminalviewer:~$ echo 안녕"
        QCOMPARE(cursor.iCol, 32);
        QVERIFY(fixture.frame.optPreedit.has_value());   // 처음은 "하" 를 조합 중이다(kInitialImeStep)
        const TerminalPreedit& preedit = *fixture.frame.optPreedit;
        QCOMPARE(int(preedit.vecCells.size()), 2);
        QCOMPARE(preedit.vecCells[0].uCodepoint, uint32_t(0xD558));   // 하
        QCOMPARE(preedit.vecCells[0].eWidth, ECellWidth::WIDE);
        QCOMPARE(preedit.vecCells[1].eWidth, ECellWidth::SPACER_TAIL);
        QCOMPARE(preedit.iCaretCell, 2);
    }

    void imeStepChangesOnlyCursorAndPreedit()
    {
        Fixture fixture(kCols, kRows);
        TerminalFrame frame;
        fixture.source.CaptureFrame(MakeChangesRequest(), frame);
        QCOMPARE(frame.eDirty, EFrameDirty::NONE);

        QVERIFY(fixture.scene.SetImeStep(fixture.source, 0));
        fixture.source.CaptureFrame(MakeChangesRequest(), frame);
        QCOMPARE(frame.eDirty, EFrameDirty::PARTIAL);
        QCOMPARE(CountDirtyRows(frame), 0);
        QVERIFY(!frame.optPreedit);
        QVERIFY(frame.cursor.bVisible);
        QVERIFY(!fixture.scene.SetImeStep(fixture.source, ShowcaseScene::kImeSteps));   // 같은 단계(0)

        QVERIFY(fixture.scene.SetImeStep(fixture.source, 6));
        fixture.source.CaptureFrame(MakeChangesRequest(), frame);
        QVERIFY(frame.optPreedit.has_value());
        QCOMPARE(frame.optPreedit->vecCells[0].uCodepoint, uint32_t(0x314E));   // ㅎ
        QCOMPARE(frame.optPreedit->vecCells[0].eWidth, ECellWidth::WIDE);
        QCOMPARE(CountDirtyRows(frame), 0);
    }

    void cursorShapeCycles()
    {
        Fixture fixture(kCols, kRows);
        const ECursorShape arrOrder[] = {ECursorShape::BAR, ECursorShape::UNDERLINE, ECursorShape::BLOCK_HOLLOW, ECursorShape::BLOCK};
        TerminalFrame frame;
        for (const ECursorShape eExpected : arrOrder)
        {
            QCOMPARE(fixture.scene.CycleCursorShape(fixture.source), eExpected);
            fixture.source.CaptureFrame(MakeChangesRequest(), frame);
            QCOMPARE(frame.cursor.eShape, eExpected);
            QCOMPARE(CountDirtyRows(frame), 0);
        }
    }

    void scrollbackHasLogLines()
    {
        Fixture fixture(kCols, kRows);
        // 로그가 공급원의 맨 위 줄부터 채운다
        for (int iLine = 0; iLine < fixture.scene.GetSectionLine(EShowcaseSection::TITLE); ++iLine)
        {
            const TerminalRow* pRow = FindRow(fixture.frame, fixture.source.GetViewportTop(), iLine);
            QVERIFY(pRow);
            QVERIFY2(!ToQString(GetRowText(*pRow)).trimmed().isEmpty(), qPrintable(QStringLiteral("line %1 is blank").arg(iLine)));
        }
        constexpr int kScroll = 50;
        QCOMPARE(fixture.source.ScrollViewport(-kScroll), -kScroll);
        QCOMPARE(fixture.source.GetViewportTop(), kScrollback - kScroll);
        TerminalFrame frame;
        fixture.source.CaptureFrame(MakeChangesRequest(), frame);
        QCOMPARE(CountDirtyRows(frame), kRows);   // 옮긴 화면은 모두 다시 그린다
        QVERIFY(ToQString(GetRowText(frame.vecRows[0])).contains(QStringLiteral("[")));
        QCOMPARE(fixture.source.ScrollViewport(-100000), -(kScrollback - kScroll));   // 맨 위에서 멈춘다
        QCOMPARE(fixture.source.ScrollViewport(100000), kScrollback);
    }

    void cursorHidesWhenPromptScrolledAway()
    {
        Fixture fixture(kCols, kRows);
        QVERIFY(!fixture.scene.SyncCursor(fixture.source));   // 맨 아래: 바뀐 것 없음
        fixture.source.ScrollViewport(-3);
        QVERIFY(fixture.scene.SyncCursor(fixture.source));
        TerminalFrame frame;
        fixture.source.CaptureFrame(MakeChangesRequest(), frame);
        QVERIFY(!frame.cursor.bVisible);
        QVERIFY(!frame.optPreedit);
        fixture.source.ScrollViewport(2);   // 아직 프롬프트 줄이 화면 아래 밖
        QVERIFY(!fixture.scene.SyncCursor(fixture.source));
        fixture.source.ScrollViewport(1);
        QVERIFY(fixture.scene.SyncCursor(fixture.source));
        fixture.source.CaptureFrame(MakeChangesRequest(), frame);
        QVERIFY(frame.cursor.bVisible);
        QCOMPARE(frame.cursor.iRow, kRows - 1);
        QVERIFY(frame.optPreedit.has_value());
    }

    void relayoutFitsEveryWidth_data()
    {
        QTest::addColumn<int>("cols");
        QTest::addColumn<int>("rows");
        for (const auto& [iCols, iRows] : {std::pair<int, int>{100, 30}, {140, 45}, {80, 24}, {64, 20}, {48, 16}, {40, 12}, {32, 10},
                                           {24, 8}, {16, 6}, {11, 5}, {6, 3}, {3, 2}, {2, 2}, {1, 1}})
            QTest::addRow("%dx%d", iCols, iRows) << iCols << iRows;
    }

    void relayoutFitsEveryWidth()
    {
        QFETCH(int, cols);
        QFETCH(int, rows);
        FakeFrameSource source(kCols, kRows, kScrollback);
        ShowcaseScene scene;
        scene.Fill(source, kCols, kRows);
        source.Resize(cols, rows);
        scene.Fill(source, cols, rows);
        QCOMPARE(scene.GetCols(), cols);
        QCOMPARE(scene.GetRows(), rows);
        QCOMPARE(int(scene.GetLines().size()), kScrollback + rows);
        for (const ShowcaseLine& line : scene.GetLines())
            QVERIFY(int(line.vecCells.size()) <= cols);

        TerminalFrame frame;
        source.CaptureFrame(MakeFullRequest(), frame);
        QCOMPARE(frame.iCols, cols);
        QCOMPARE(int(frame.vecRows.size()), kScrollback + rows);
        const QString strDiff = CompareWithScene(scene, frame, source.GetViewportTop());
        QVERIFY2(strDiff.isEmpty(), qPrintable(strDiff));
        for (const TerminalRow& row : frame.vecRows)
        {
            QCOMPARE(int(row.vecCells.size()), cols);
            const QString strPairs = CheckWidePairs(row, cols);
            QVERIFY2(strPairs.isEmpty(), qPrintable(strPairs));
        }
        for (int iSection = 0; iSection < int(EShowcaseSection::COUNT); ++iSection)
            QVERIFY2(scene.GetSectionLine(EShowcaseSection(iSection)) >= 0, qPrintable(QString::number(iSection)));
        QCOMPARE(scene.GetSectionLine(EShowcaseSection::PROMPT), kScrollback + rows - 1);
        QVERIFY(frame.cursor.iCol >= 0 && frame.cursor.iCol < cols);
        QCOMPARE(frame.cursor.iRow, rows - 1);
        const auto [iBegin, iEnd] = scene.GetSelection();
        QVERIFY(iBegin >= 0 && iBegin <= iEnd && iEnd <= cols);
    }

    void liveUpdateDirtiesOnlyLiveLine()
    {
        Fixture fixture(kCols, kRows);
        TerminalFrame frame;
        fixture.source.CaptureFrame(MakeChangesRequest(), frame);
        QCOMPARE(frame.eDirty, EFrameDirty::NONE);
        const int iTop = fixture.source.GetViewportTop();
        const int iLive = fixture.scene.GetSectionLine(EShowcaseSection::LIVE);
        const std::u32string strBefore = GetRowText(*FindRow(fixture.frame, iTop, iLive));

        fixture.scene.UpdateLive(fixture.source, 1, U"12:34:56.789");
        fixture.source.CaptureFrame(MakeChangesRequest(), frame);
        QCOMPARE(CountDirtyRows(frame), 1);
        const TerminalRow* pLive = FindRow(frame, iTop, iLive);
        QVERIFY(pLive && pLive->bDirty);
        const QString strAfter = ToQString(GetRowText(*pLive));
        QVERIFY(GetRowText(*pLive) != strBefore);
        QVERIFY2(strAfter.contains(QStringLiteral("12:34:56.789")), qPrintable(strAfter));
        QVERIFY2(strAfter.contains(QStringLiteral("%")), qPrintable(strAfter));

        fixture.scene.UpdateLive(fixture.source, 2, U"12:34:56.889");
        fixture.source.CaptureFrame(MakeChangesRequest(), frame);
        QCOMPARE(CountDirtyRows(frame), 1);
        QVERIFY(ToQString(GetRowText(*FindRow(frame, iTop, iLive))) != strAfter);

        fixture.source.CaptureFrame(MakeFullRequest(), frame);
        const QString strDiff = CompareWithScene(fixture.scene, frame, iTop);   // 장면의 기억도 함께 바뀐다
        QVERIFY2(strDiff.isEmpty(), qPrintable(strDiff));
    }

    void liveLineKeepsAfterRelayout()
    {
        Fixture fixture(kCols, kRows);
        fixture.scene.UpdateLive(fixture.source, 7, U"01:02:03.004");
        fixture.source.Resize(60, 20);
        fixture.scene.Fill(fixture.source, 60, 20);
        const ShowcaseLine& live = fixture.scene.GetLines()[size_t(fixture.scene.GetSectionLine(EShowcaseSection::LIVE))];
        std::u32string strText;
        for (const TerminalCell& cell : live.vecCells)
            strText += cell.uCodepoint ? char32_t(cell.uCodepoint) : U' ';
        QVERIFY2(ToQString(strText).contains(QStringLiteral("01:02:03.004")), qPrintable(ToQString(strText)));
    }

    void fillIsDeterministic()
    {
        Fixture first(kCols, kRows);
        Fixture second(kCols, kRows);
        const std::vector<ShowcaseLine>& vecFirst = first.scene.GetLines();
        const std::vector<ShowcaseLine>& vecSecond = second.scene.GetLines();
        QCOMPARE(vecFirst.size(), vecSecond.size());
        for (size_t uLine = 0; uLine < vecFirst.size(); ++uLine)
        {
            QCOMPARE(vecFirst[uLine].vecCells.size(), vecSecond[uLine].vecCells.size());
            QVERIFY(vecFirst[uLine].vecGraphemes == vecSecond[uLine].vecGraphemes);
            for (size_t uCol = 0; uCol < vecFirst[uLine].vecCells.size(); ++uCol)
                QVERIFY(IsSameCell(vecFirst[uLine].vecCells[uCol], vecSecond[uLine].vecCells[uCol]));
        }
    }
};

QObject* CreateShowcaseSceneTest() { return new ShowcaseSceneTest(); }

#include "ShowcaseSceneTest.moc"
