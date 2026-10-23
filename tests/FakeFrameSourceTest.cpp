#include "FakeFrameSource.h"

#include <QTest>
#include <map>

class FakeFrameSourceTest : public QObject
{
    Q_OBJECT

private:
    static TerminalCaptureRequest Req(int above = 0, int below = 0, bool full = false)
    {
        TerminalCaptureRequest request;
        request.iOverscanAbove = above;
        request.iOverscanBelow = below;
        request.bFull = full;
        return request;
    }

    static int DirtyCount(const TerminalFrame& frame)
    {
        int iCount = 0;
        for (const TerminalRow& row : frame.vecRows)
        {
            iCount += row.bDirty ? 1 : 0;
        }
        return iCount;
    }

private slots:
    void firstCaptureIsFull()
    {
        FakeFrameSource source(10, 5, 20);
        TerminalFrame frame;
        source.CaptureFrame(Req(), frame);
        QCOMPARE(frame.eDirty, EFrameDirty::FULL);
        QCOMPARE(frame.iCols, 10);
        QCOMPARE(frame.iRows, 5);
        QCOMPARE(int(frame.vecRows.size()), 5);
        for (const TerminalRow& row : frame.vecRows)
        {
            QCOMPARE(int(row.vecCells.size()), 10);
        }
        QCOMPARE(source.GetCaptureCount(), 1);
    }

    void unchangedIsNone()
    {
        FakeFrameSource source(10, 5, 20);
        TerminalFrame frame;
        source.CaptureFrame(Req(), frame);
        source.CaptureFrame(Req(), frame);
        QCOMPARE(frame.eDirty, EFrameDirty::NONE);
        QCOMPARE(DirtyCount(frame), 0);
        for (const TerminalRow& row : frame.vecRows)
        {
            QVERIFY(row.vecCells.empty());
        }
    }

    void changedLineIsPartial()
    {
        FakeFrameSource source(10, 5, 20);
        TerminalFrame frame;
        source.CaptureFrame(Req(), frame);
        const uint64_t llOldId = frame.vecRows[2].llId;
        const uint64_t llOtherId = frame.vecRows[3].llId;
        source.SetText(source.GetViewportTop() + 2, 0, U"abc", TerminalCell{});
        source.CaptureFrame(Req(), frame);
        QCOMPARE(frame.eDirty, EFrameDirty::PARTIAL);
        QCOMPARE(DirtyCount(frame), 1);
        QVERIFY(frame.vecRows[2].bDirty);
        QVERIFY(frame.vecRows[2].llId != llOldId);
        QCOMPARE(frame.vecRows[3].llId, llOtherId);
        QCOMPARE(int(frame.vecRows[2].vecCells.size()), 10);
        QCOMPARE(frame.vecRows[2].vecCells[1].uCodepoint, uint32_t('b'));
    }

    void scrollMakesAllDirtyKeepsIds()
    {
        FakeFrameSource source(10, 5, 20);
        const int iTop = source.GetViewportTop();
        TerminalFrame frame;
        source.CaptureFrame(Req(), frame);
        const uint64_t llId = frame.vecRows[0].llId; // 줄 iTop
        QCOMPARE(source.ScrollViewport(-3), -3);
        QCOMPARE(source.GetViewportTop(), iTop - 3);
        source.CaptureFrame(Req(), frame);
        QCOMPARE(frame.eDirty, EFrameDirty::PARTIAL);
        QCOMPARE(DirtyCount(frame), 5);
        QCOMPARE(frame.vecRows[3].llId, llId);
    }

    void scrollClampsAtTop()
    {
        FakeFrameSource source(10, 5, 20);
        QCOMPARE(source.ScrollViewport(-100), -20);
        QCOMPARE(source.GetViewportTop(), 0);
        QCOMPARE(source.ScrollViewport(-1), 0);
        QCOMPARE(source.ScrollViewport(100), 20);
    }

    void fullFillsCleanRows()
    {
        FakeFrameSource source(10, 5, 20);
        TerminalFrame frame;
        source.CaptureFrame(Req(), frame);
        source.CaptureFrame(Req(0, 0, true), frame);
        QCOMPARE(frame.eDirty, EFrameDirty::FULL);
        for (const TerminalRow& row : frame.vecRows)
        {
            QCOMPARE(int(row.vecCells.size()), 10);
        }
    }

    void overscanBelow()
    {
        FakeFrameSource source(10, 5, 20);
        TerminalFrame frame;
        source.CaptureFrame(Req(0, 1), frame);
        QCOMPARE(int(frame.vecRows.size()), 5);
        for (const TerminalRow& row : frame.vecRows)
        {
            QVERIFY(row.iViewportY < 5);
        }
        source.ScrollViewport(-2);
        source.CaptureFrame(Req(0, 1), frame);
        QCOMPARE(int(frame.vecRows.size()), 6);
        QCOMPARE(frame.vecRows.back().iViewportY, 5);
    }

    void overscanAbove()
    {
        FakeFrameSource source(10, 5, 20);
        TerminalFrame frame;
        source.CaptureFrame(Req(1, 0), frame);
        QCOMPARE(int(frame.vecRows.size()), 6);
        QCOMPARE(frame.vecRows.front().iViewportY, -1);
    }

    void wideCharacterGetsSpacer()
    {
        FakeFrameSource source(10, 5, 20);
        source.SetText(source.GetViewportTop(), 0, U"한", TerminalCell{});
        TerminalFrame frame;
        source.CaptureFrame(Req(), frame);
        QCOMPARE(frame.vecRows[0].vecCells[0].eWidth, ECellWidth::WIDE);
        QCOMPARE(frame.vecRows[0].vecCells[1].eWidth, ECellWidth::SPACER_TAIL);
        QCOMPARE(frame.vecRows[0].vecCells[0].uCodepoint, uint32_t(0xD55C));
    }

    void cellDefaults()
    {
        const TerminalCell cell;
        QCOMPARE(cell.uCodepoint, uint32_t(0));
        QVERIFY(cell.fg == (TerminalRgb{255, 255, 255}));
        QVERIFY(cell.bg == (TerminalRgb{0, 0, 0}));
        QVERIFY(cell.bBgDefault);
        QCOMPARE(cell.eWidth, ECellWidth::NARROW);
        QCOMPARE(cell.iGrapheme, -1);
        QCOMPARE(TerminalRow{}.iSelectBegin, -1);
    }

    void mixedIsDeterministic()
    {
        FakeFrameSource sourceA(40, 12, 5);
        FakeFrameSource sourceB(40, 12, 5);
        sourceA.FillPattern(EFakePattern::MIXED, 42);
        sourceB.FillPattern(EFakePattern::MIXED, 42);
        TerminalFrame frameA;
        TerminalFrame frameB;
        sourceA.CaptureFrame(Req(), frameA);
        sourceB.CaptureFrame(Req(), frameB);
        QCOMPARE(frameA.vecRows.size(), frameB.vecRows.size());
        bool bAnyContent = false;
        bool bAnySelect = false;
        bool bAnyGrapheme = false;
        for (size_t uRow = 0; uRow < frameA.vecRows.size(); ++uRow)
        {
            QCOMPARE(frameA.vecRows[uRow].vecCells.size(), frameB.vecRows[uRow].vecCells.size());
            QCOMPARE(frameA.vecRows[uRow].iSelectBegin, frameB.vecRows[uRow].iSelectBegin);
            bAnySelect = bAnySelect || frameA.vecRows[uRow].iSelectBegin >= 0;
            bAnyGrapheme = bAnyGrapheme || !frameA.vecRows[uRow].vecGraphemes.empty();
            QVERIFY(frameA.vecRows[uRow].vecGraphemes == frameB.vecRows[uRow].vecGraphemes);
            for (size_t uCol = 0; uCol < frameA.vecRows[uRow].vecCells.size(); ++uCol)
            {
                const TerminalCell& cellA = frameA.vecRows[uRow].vecCells[uCol];
                const TerminalCell& cellB = frameB.vecRows[uRow].vecCells[uCol];
                bAnyContent = bAnyContent || cellA.uCodepoint != 0;
                QCOMPARE(cellA.uCodepoint, cellB.uCodepoint);
                QVERIFY(cellA.fg == cellB.fg && cellA.bg == cellB.bg && cellA.underlineColor == cellB.underlineColor);
                QCOMPARE(cellA.bBold, cellB.bBold);
                QCOMPARE(cellA.bItalic, cellB.bItalic);
                QCOMPARE(cellA.bFaint, cellB.bFaint);
                QCOMPARE(cellA.bInverse, cellB.bInverse);
                QCOMPARE(cellA.bStrikethrough, cellB.bStrikethrough);
                QCOMPARE(cellA.eUnderline, cellB.eUnderline);
                QCOMPARE(cellA.eWidth, cellB.eWidth);
                QCOMPARE(cellA.iGrapheme, cellB.iGrapheme);
                QCOMPARE(cellA.bBgDefault, cellB.bBgDefault);
            }
        }
        QVERIFY(bAnyContent);
        QVERIFY(bAnySelect);
        QVERIFY(bAnyGrapheme);
    }

    void floodChangesEveryLineWithSeed()
    {
        FakeFrameSource source(20, 6, 0);
        source.FillPattern(EFakePattern::FLOOD, 1);
        TerminalFrame frame;
        source.CaptureFrame(Req(), frame);
        std::vector<uint64_t> vecIds;
        std::vector<std::vector<TerminalCell>> vecOldCells;
        for (const TerminalRow& row : frame.vecRows)
        {
            vecIds.push_back(row.llId);
            vecOldCells.push_back(row.vecCells);
        }
        source.FillPattern(EFakePattern::FLOOD, 2);
        source.CaptureFrame(Req(), frame);
        QCOMPARE(frame.eDirty, EFrameDirty::PARTIAL);
        QCOMPARE(DirtyCount(frame), 6);
        for (size_t uRow = 0; uRow < vecIds.size(); ++uRow)
        {
            QVERIFY(frame.vecRows[uRow].llId != vecIds[uRow]);
            const std::vector<TerminalCell>& vecNew = frame.vecRows[uRow].vecCells;
            QCOMPARE(vecNew.size(), vecOldCells[uRow].size());
            bool bDiffer = false;
            for (size_t uCol = 0; uCol < vecNew.size(); ++uCol)
            {
                bDiffer = bDiffer || vecNew[uCol].uCodepoint != vecOldCells[uRow][uCol].uCodepoint ||
                          vecNew[uCol].fg != vecOldCells[uRow][uCol].fg || vecNew[uCol].bg != vecOldCells[uRow][uCol].bg;
            }
            QVERIFY2(bDiffer, qPrintable(QStringLiteral("row %1 content unchanged").arg(uRow)));
        }
    }

    void rowIdsAreNeverReusedAcrossResize()
    {
        FakeFrameSource source(10, 5, 4);
        std::map<uint64_t, std::vector<uint32_t>> mapSeen;   // id -> 첫 칸들의 내용
        const auto collect = [&](const TerminalFrame& frame)
        {
            for (const TerminalRow& row : frame.vecRows)
            {
                std::vector<uint32_t> vecContent;
                for (const TerminalCell& cell : row.vecCells)
                {
                    vecContent.push_back(cell.uCodepoint);
                }
                const auto it = mapSeen.find(row.llId);
                if (it != mapSeen.end())
                {
                    QVERIFY2(it->second == vecContent, "row id reused for different content");
                }
                else
                {
                    mapSeen[row.llId] = vecContent;
                }
            }
        };
        TerminalFrame frame;
        source.SetText(source.GetViewportTop() + 4, 0, U"bottom", TerminalCell{});   // 줄어들 때 사라질 줄
        source.CaptureFrame(Req(0, 0, true), frame);
        collect(frame);
        source.Resize(10, 2);
        source.CaptureFrame(Req(0, 0, true), frame);
        collect(frame);
        source.Resize(10, 5);
        source.CaptureFrame(Req(0, 0, true), frame);
        collect(frame);
        source.SetText(source.GetViewportTop(), 0, U"zzz", TerminalCell{});
        source.CaptureFrame(Req(0, 0, true), frame);
        collect(frame);
        source.SetText(source.GetViewportTop() + 1, 0, U"yyy", TerminalCell{});
        source.CaptureFrame(Req(0, 0, true), frame);
        collect(frame);
        if (QTest::currentTestFailed())
        {
            return;
        }
        // 변경 전 id 와 변경 후 id 가 겹치지 않는다는 것을 직접도 확인한다.
        QVERIFY(mapSeen.size() > 5);
    }

    void cursorOnlyChangeIsPartial()
    {
        FakeFrameSource source(10, 5, 20);
        TerminalFrame frame;
        source.CaptureFrame(Req(), frame);
        TerminalCursor cursor;
        cursor.bVisible = true;
        cursor.iCol = 3;
        cursor.iRow = 2;
        source.SetCursor(cursor);
        source.CaptureFrame(Req(), frame);
        QCOMPARE(frame.eDirty, EFrameDirty::PARTIAL);
        QCOMPARE(frame.cursor.iCol, 3);
    }
};

QObject* CreateFakeFrameSourceTest() { return new FakeFrameSourceTest(); }

#include "FakeFrameSourceTest.moc"
