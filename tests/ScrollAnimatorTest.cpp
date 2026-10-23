#include "TerminalViewer/Algorithm/TerminalRenderer/Common/ScrollAnimator.h"

#include <QTest>

namespace
{
constexpr int kCellHeight = 20;
constexpr qint64 kMsNs = 1'000'000;
constexpr qint64 kStepNs = 16 * kMsNs;
constexpr qint64 kT0 = 5'000 * kMsNs;
constexpr qint64 kOneSecondNs = 1'000 * kMsNs;
}

class ScrollAnimatorTest : public QObject
{
    Q_OBJECT

private:
    struct Track
    {
        int iLines = 0;
        int iLastPosition = 0;
        bool bMonotonic = true;
        qint64 llStoppedAtNs = -1;
    };

    // 16ms 씩 durationNs 동안 진행하며 넘은 줄을 더한다. direction 쪽으로만 움직였는지 본다.
    static Track Run(ScrollAnimator& animator, qint64 startNs, qint64 durationNs, int direction)
    {
        Track track;
        for (qint64 llNow = startNs + kStepNs; llNow <= startNs + durationNs; llNow += kStepNs)
        {
            track.iLines += animator.Advance(llNow, kCellHeight);
            const int iOffset = animator.GetOffsetPx();
            if (iOffset < 0 || iOffset >= kCellHeight)
            {
                track.bMonotonic = false;
            }
            const int iPosition = track.iLines * kCellHeight + iOffset;
            if ((iPosition - track.iLastPosition) * direction < 0)
            {
                track.bMonotonic = false;
            }
            track.iLastPosition = iPosition;
            if (!animator.IsAnimating() && track.llStoppedAtNs < 0)
            {
                track.llStoppedAtNs = llNow - startNs;
            }
        }
        return track;
    }

private slots:
    void firstAdvanceOnlyRecordsTime()
    {
        ScrollAnimator animator;
        QCOMPARE(animator.Advance(kT0, kCellHeight), 0);
        QCOMPARE(animator.GetOffsetPx(), 0);
        QVERIFY(!animator.IsAnimating());

        ScrollAnimator pending;
        pending.AddPixels(100);
        QVERIFY(pending.IsAnimating());
        QCOMPARE(pending.Advance(kT0, kCellHeight), 0);
        QCOMPARE(pending.GetOffsetPx(), 0);
        QVERIFY(pending.IsAnimating());
    }

    void firstStepFollowsTimeConstant()
    {
        ScrollAnimator animator;
        QCOMPARE(animator.Advance(kT0, kCellHeight), 0);
        animator.AddPixels(100);
        QVERIFY(animator.IsAnimating());
        // 100 × (1 − e^(−16/60)) = 23.4 → 1줄, 오프셋 3
        QCOMPARE(animator.Advance(kT0 + kStepNs, kCellHeight), 1);
        QCOMPARE(animator.GetOffsetPx(), 3);
        QVERIFY(animator.IsAnimating());
    }

    void convergesDownwardWithinOneSecond()
    {
        ScrollAnimator animator;
        animator.Advance(kT0, kCellHeight);
        animator.AddPixels(100);
        const Track track = Run(animator, kT0, kOneSecondNs, +1);
        QCOMPARE(track.iLines, 5);
        QCOMPARE(animator.GetOffsetPx(), 0);
        QVERIFY(!animator.IsAnimating());
        QVERIFY(track.bMonotonic);
        QVERIFY(track.llStoppedAtNs > 0 && track.llStoppedAtNs <= kOneSecondNs);
    }

    void convergesUpwardBelowLineBoundary()
    {
        ScrollAnimator animator;
        animator.Advance(kT0, kCellHeight);
        animator.AddPixels(-30);
        const Track track = Run(animator, kT0, kOneSecondNs, -1);
        // −30 = −2 × 20 + 10
        QCOMPARE(track.iLines, -2);
        QCOMPARE(animator.GetOffsetPx(), 10);
        QVERIFY(!animator.IsAnimating());
        QVERIFY(track.bMonotonic);
    }

    void offsetRoundingToCellHeightCarriesLine()
    {
        ScrollAnimator animator;
        animator.Advance(kT0, kCellHeight);
        animator.AddPixels(19.6);
        // 10초 뒤면 목표에 닿는다: 19.6 → 오프셋이 20 으로 반올림 → 1줄, 오프셋 0
        QCOMPARE(animator.Advance(kT0 + 10 * kOneSecondNs, kCellHeight), 1);
        QCOMPARE(animator.GetOffsetPx(), 0);
        QVERIFY(!animator.IsAnimating());
        QCOMPARE(animator.Advance(kT0 + 11 * kOneSecondNs, kCellHeight), 0);
        QCOMPARE(animator.GetOffsetPx(), 0);
    }

    void smallMovesAddUp()
    {
        ScrollAnimator animator;
        animator.Advance(kT0, kCellHeight);
        int iLines = 0;
        qint64 llNow = kT0;
        for (int i = 0; i < 10; ++i)
        {
            animator.AddPixels(7);
            llNow += kStepNs;
            iLines += animator.Advance(llNow, kCellHeight);
        }
        llNow += 10 * kOneSecondNs;
        iLines += animator.Advance(llNow, kCellHeight);
        QCOMPARE(iLines * kCellHeight + animator.GetOffsetPx(), 70);
        QVERIFY(!animator.IsAnimating());
    }

    void stopAtEdgeEndsAnimation()
    {
        ScrollAnimator animator;
        animator.Advance(kT0, kCellHeight);
        animator.AddPixels(100);
        QCOMPARE(animator.Advance(kT0 + kStepNs, kCellHeight), 1);
        QCOMPARE(animator.GetOffsetPx(), 3);
        animator.StopAtEdge();
        QCOMPARE(animator.GetOffsetPx(), 0);
        QVERIFY(!animator.IsAnimating());
        QCOMPARE(animator.Advance(kT0 + 2 * kStepNs, kCellHeight), 0);
        QCOMPARE(animator.GetOffsetPx(), 0);
        QVERIFY(!animator.IsAnimating());
    }

    void resetForgetsEverything()
    {
        ScrollAnimator animator;
        animator.Advance(kT0, kCellHeight);
        animator.AddPixels(55);
        animator.Advance(kT0 + kStepNs, kCellHeight);
        animator.Reset();
        QCOMPARE(animator.GetOffsetPx(), 0);
        QVERIFY(!animator.IsAnimating());
        // 시각도 잊는다: 다시 처음 부른 것처럼 0
        animator.AddPixels(100);
        QCOMPARE(animator.Advance(kT0 + 2 * kOneSecondNs, kCellHeight), 0);
        QCOMPARE(animator.Advance(kT0 + 2 * kOneSecondNs + kStepNs, kCellHeight), 1);
        QCOMPARE(animator.GetOffsetPx(), 3);
    }
};

QObject* CreateScrollAnimatorTest() { return new ScrollAnimatorTest(); }

#include "ScrollAnimatorTest.moc"
