#include "TerminalViewer/Algorithm/TerminalRenderer/Common/FrameScheduler.h"

#include <QTest>

#include <algorithm>

namespace
{
constexpr qint64 kMsNs = 1'000'000;
constexpr qint64 kT0 = 10'000 * kMsNs;
constexpr qint64 kFrameNs = FrameScheduler::kDefaultFrameIntervalNs;
constexpr qint64 kBlinkNs = FrameScheduler::kBlinkIntervalNs;

bool Contains(const std::vector<int>& vecViews, int viewId)
{
    return std::find(vecViews.begin(), vecViews.end(), viewId) != vecViews.end();
}
}

class FrameSchedulerTest : public QObject
{
    Q_OBJECT

private slots:
    void newViewIsDueUntilRendered()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0), 1));
        QCOMPARE(scheduler.NextDeadline(kT0), std::optional<qint64>(kT0));
        scheduler.OnRendered(1, kT0, false, false);
        QVERIFY(scheduler.CollectDueViews(kT0).empty());
        QVERIFY(!scheduler.NextDeadline(kT0).has_value());
    }

    void wakeMakesDueUntilRendered()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.AddView(2);
        scheduler.OnRendered(1, kT0, false, false);
        scheduler.OnRendered(2, kT0, false, false);
        scheduler.OnWake(1);
        scheduler.OnWake(1);
        const std::vector<int> vecDue = scheduler.CollectDueViews(kT0);
        QCOMPARE(int(vecDue.size()), 1);
        QVERIFY(Contains(vecDue, 1));
        scheduler.OnRendered(1, kT0, false, false);
        QVERIFY(scheduler.CollectDueViews(kT0 + kMsNs).empty());
        scheduler.OnWake(99);   // 모르는 화면은 무시
        QVERIFY(scheduler.CollectDueViews(kT0 + kMsNs).empty());
    }

    void hiddenViewIsNeverDue()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnVisible(1, false);
        QVERIFY(scheduler.CollectDueViews(kT0).empty());
        scheduler.OnWake(1);
        scheduler.OnConfigChanged(1);
        QVERIFY(scheduler.CollectDueViews(kT0).empty());
        QVERIFY(!scheduler.NextDeadline(kT0).has_value());

        scheduler.OnVisible(1, true);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0), 1));
        scheduler.OnRendered(1, kT0, false, false);
        QVERIFY(scheduler.CollectDueViews(kT0).empty());

        // 다시 보이면 깨우지 않아도 그려야 한다
        scheduler.OnVisible(1, false);
        scheduler.OnVisible(1, true);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0), 1));
    }

    void hiddenAnimatingOrBlinkingViewHasNoDeadline()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnFocus(1, true);
        scheduler.OnRendered(1, kT0, true, true);
        QVERIFY(scheduler.NextDeadline(kT0).has_value());
        scheduler.OnVisible(1, false);
        QVERIFY(!scheduler.NextDeadline(kT0).has_value());
        QVERIFY(scheduler.CollectDueViews(kT0 + kBlinkNs).empty());
    }

    void focusedBlinkingCursorTogglesEveryInterval()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnFocus(1, true);
        scheduler.OnRendered(1, kT0, false, true);
        QVERIFY(scheduler.CollectDueViews(kT0).empty());
        QCOMPARE(scheduler.NextDeadline(kT0), std::optional<qint64>(kT0 + kBlinkNs));
        QVERIFY(scheduler.CollectDueViews(kT0 + kBlinkNs - kMsNs).empty());
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + kBlinkNs), 1));
        QVERIFY(scheduler.IsBlinkOn(1, kT0));
        QVERIFY(!scheduler.IsBlinkOn(1, kT0 + kBlinkNs));
        QVERIFY(scheduler.IsBlinkOn(1, kT0 + 2 * kBlinkNs));
        QVERIFY(!scheduler.IsBlinkOn(1, kT0 + 3 * kBlinkNs));

        // 늦게 그려도 다음 마감은 주기에 맞춘다
        scheduler.OnRendered(1, kT0 + kBlinkNs + 3 * kMsNs, false, true);
        QVERIFY(scheduler.CollectDueViews(kT0 + kBlinkNs + 4 * kMsNs).empty());
        QCOMPARE(scheduler.NextDeadline(kT0 + kBlinkNs + 4 * kMsNs), std::optional<qint64>(kT0 + 2 * kBlinkNs));
    }

    void noBlinkDeadlineWithoutFocus()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnRendered(1, kT0, false, true);
        QVERIFY(!scheduler.NextDeadline(kT0).has_value());
        QVERIFY(scheduler.IsBlinkOn(1, kT0 + kBlinkNs));
        QVERIFY(scheduler.CollectDueViews(kT0 + kBlinkNs).empty());

        // 포커스가 있어도 깜빡이지 않는 커서면 마감이 없다
        scheduler.OnFocus(1, true);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0), 1));   // 포커스 변화는 커서 모양을 바꾼다
        scheduler.OnRendered(1, kT0, false, false);
        QVERIFY(!scheduler.NextDeadline(kT0).has_value());
        QVERIFY(scheduler.IsBlinkOn(1, kT0 + kBlinkNs));

        // 포커스를 잃으면 보이는 커서로 끝난다
        scheduler.OnRendered(1, kT0, false, true);
        QVERIFY(!scheduler.IsBlinkOn(1, kT0 + kBlinkNs));
        scheduler.OnFocus(1, false);
        QVERIFY(scheduler.IsBlinkOn(1, kT0 + kBlinkNs));
        scheduler.OnRendered(1, kT0 + kBlinkNs, false, true);
        QVERIFY(!scheduler.NextDeadline(kT0 + kBlinkNs).has_value());
    }

    void animationWantsNextFrame()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnRendered(1, kT0, true, false);
        // 다음 애니메이션 프레임은 그린 시각 + 프레임 간격(기본 60Hz)
        QCOMPARE(scheduler.NextDeadline(kT0), std::optional<qint64>(kT0 + kFrameNs));
        QCOMPARE(scheduler.NextDeadline(kT0 + 2 * kMsNs), std::optional<qint64>(kT0 + kFrameNs));
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + kFrameNs), 1));
        scheduler.OnRendered(1, kT0 + kFrameNs, false, false);
        QVERIFY(!scheduler.NextDeadline(kT0 + kFrameNs).has_value());
        QVERIFY(scheduler.CollectDueViews(kT0 + 2 * kFrameNs).empty());
    }

    void animatingViewIsDueOnlyAtItsFrameTime()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnRendered(1, kT0, true, false);
        // 다른 화면 때문에 루프가 돌아도 프레임 간격 전에는 그리지 않는다
        QVERIFY(scheduler.CollectDueViews(kT0 + kMsNs).empty());
        QVERIFY(scheduler.CollectDueViews(kT0 + kFrameNs - kMsNs).empty());
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + kFrameNs), 1));
        QCOMPARE(scheduler.NextDeadline(kT0 + kFrameNs + 5 * kMsNs), std::optional<qint64>(kT0 + kFrameNs + 5 * kMsNs));   // 늦었으면 지금

        // 깨우거나 설정이 바뀌면 프레임 간격을 기다리지 않는다
        scheduler.OnWake(1);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + kMsNs), 1));
        scheduler.OnRendered(1, kT0 + kMsNs, true, false);
        QVERIFY(scheduler.CollectDueViews(kT0 + 2 * kMsNs).empty());
        scheduler.OnConfigChanged(1);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + 2 * kMsNs), 1));
        scheduler.OnRendered(1, kT0 + 2 * kMsNs, true, false);
        QCOMPARE(scheduler.NextDeadline(kT0 + 2 * kMsNs), std::optional<qint64>(kT0 + 2 * kMsNs + kFrameNs));
    }

    void frameIntervalPacesAnimation()
    {
        constexpr qint64 k120HzNs = 8'333'333;
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.AddView(2);
        scheduler.SetFrameInterval(1, k120HzNs);
        scheduler.SetFrameInterval(1, 0);    // 무시
        scheduler.SetFrameInterval(1, -5);   // 무시
        scheduler.OnRendered(1, kT0, true, false);
        scheduler.OnRendered(2, kT0, false, false);
        qint64 llRendered = kT0;
        for (int i = 0; i < 5; ++i)
        {
            const std::optional<qint64> optDeadline = scheduler.NextDeadline(llRendered);
            QCOMPARE(optDeadline, std::optional<qint64>(llRendered + k120HzNs));   // 프레임 사이 ~8.33ms
            QVERIFY(scheduler.CollectDueViews(*optDeadline - kMsNs).empty());
            QVERIFY(Contains(scheduler.CollectDueViews(*optDeadline), 1));
            llRendered = *optDeadline;
            scheduler.OnRendered(1, llRendered, true, false);
        }
        // 다른 화면의 간격은 그대로(기본 60Hz)
        scheduler.OnRendered(1, kT0, false, false);
        scheduler.OnRendered(2, kT0, true, false);
        QCOMPARE(scheduler.NextDeadline(kT0), std::optional<qint64>(kT0 + kFrameNs));
        scheduler.SetFrameInterval(99, k120HzNs);   // 모르는 화면은 무시
    }

    void notReadyRetriesWithinOneMillisecond()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnRendered(1, kT0, false, false);
        scheduler.OnWake(1);
        scheduler.OnNotReady(1);
        const std::optional<qint64> optDeadline = scheduler.NextDeadline(kT0);
        QVERIFY(optDeadline.has_value());
        QVERIFY(*optDeadline > kT0);   // 바로 다시 돌지 않는다(바쁜 대기 없음)
        QVERIFY(*optDeadline <= kT0 + kMsNs);
        QVERIFY(Contains(scheduler.CollectDueViews(*optDeadline), 1));
        scheduler.OnNotReady(1);
        QVERIFY(Contains(scheduler.CollectDueViews(*optDeadline + kMsNs), 1));
        scheduler.OnRendered(1, kT0 + 2 * kMsNs, false, false);
        QVERIFY(!scheduler.NextDeadline(kT0 + 2 * kMsNs).has_value());
        QVERIFY(scheduler.CollectDueViews(kT0 + 3 * kMsNs).empty());
    }

    void notReadyBlinkingViewDoesNotSpin()
    {
        // 포커스 + 깜빡임 화면이 전환 시각을 지났는데 표시를 못 하면, 그 동안 마감은 매번 지금 + 재시도 간격이다.
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnFocus(1, true);
        scheduler.OnRendered(1, kT0, true, true);   // 깜빡임 주기 시작 + 애니메이션 중
        qint64 llNow = kT0 + kBlinkNs + 70 * kMsNs;  // 전환 시각이 지났다
        QVERIFY(Contains(scheduler.CollectDueViews(llNow), 1));
        scheduler.OnWake(1);
        for (int iPass = 0; iPass < 5; ++iPass)
        {
            scheduler.OnNotReady(1);
            const std::optional<qint64> optDeadline = scheduler.NextDeadline(llNow);
            QVERIFY(optDeadline.has_value());
            QVERIFY2(*optDeadline - llNow >= FrameScheduler::kRetryDelayNs, qPrintable(QString::number(*optDeadline - llNow)));
            QVERIFY(Contains(scheduler.CollectDueViews(*optDeadline), 1));   // 그때 다시 본다
            llNow = *optDeadline;
        }
        scheduler.OnRendered(1, llNow, false, true);
        QVERIFY(scheduler.CollectDueViews(llNow).empty());
    }

    void activityShowsCursorAndRestartsCycle()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnFocus(1, true);
        scheduler.OnRendered(1, kT0, false, true);
        scheduler.OnRendered(1, kT0 + kBlinkNs, false, true);   // 꺼진 커서를 그렸다
        const qint64 llActivity = kT0 + kBlinkNs + 70 * kMsNs;
        QVERIFY(!scheduler.IsBlinkOn(1, llActivity));
        QVERIFY(scheduler.CollectDueViews(llActivity).empty());

        scheduler.OnActivity(1, llActivity);
        QVERIFY(scheduler.IsBlinkOn(1, llActivity));
        QVERIFY(Contains(scheduler.CollectDueViews(llActivity), 1));   // 보이는 커서로 다시 그려야 함
        scheduler.OnRendered(1, llActivity, false, true);
        QVERIFY(scheduler.CollectDueViews(llActivity).empty());
        QCOMPARE(scheduler.NextDeadline(llActivity), std::optional<qint64>(llActivity + kBlinkNs));
        QVERIFY(scheduler.IsBlinkOn(1, llActivity + kBlinkNs - kMsNs));
        QVERIFY(!scheduler.IsBlinkOn(1, llActivity + kBlinkNs));
    }

    void configChangeMakesDue()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnRendered(1, kT0, false, false);
        scheduler.OnConfigChanged(1);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0), 1));
        QCOMPARE(scheduler.NextDeadline(kT0), std::optional<qint64>(kT0));
    }

    void focusChangeOnlyWhenDifferent()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnRendered(1, kT0, false, false);
        scheduler.OnFocus(1, false);
        QVERIFY(scheduler.CollectDueViews(kT0).empty());
        scheduler.OnFocus(1, true);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0), 1));
    }

    void removedViewIsForgotten()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnFocus(1, true);
        scheduler.OnRendered(1, kT0, true, true);
        scheduler.RemoveView(1);
        QVERIFY(scheduler.CollectDueViews(kT0 + kBlinkNs).empty());
        QVERIFY(!scheduler.NextDeadline(kT0).has_value());
        QVERIFY(scheduler.IsBlinkOn(1, kT0 + kBlinkNs));
    }

    void deadlineIsEarliestOfViews()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.AddView(2);
        scheduler.OnFocus(1, true);
        scheduler.OnRendered(1, kT0, false, true);    // 깜빡임: kT0 + 530ms
        scheduler.OnRendered(2, kT0, true, false);    // 애니메이션: now + 16ms
        QCOMPARE(scheduler.NextDeadline(kT0), std::optional<qint64>(kT0 + kFrameNs));
        scheduler.OnRendered(2, kT0, false, false);
        QCOMPARE(scheduler.NextDeadline(kT0), std::optional<qint64>(kT0 + kBlinkNs));
    }
};

QObject* CreateFrameSchedulerTest() { return new FrameSchedulerTest(); }

#include "FrameSchedulerTest.moc"
