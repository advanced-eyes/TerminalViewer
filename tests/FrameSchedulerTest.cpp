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
        scheduler.OnRendered(1, kT0 - kFrameNs, false, false);   // 한 간격 전에 그렸다(한가함)
        scheduler.OnRendered(2, kT0 - kFrameNs, false, false);
        scheduler.OnWake(1, kT0);
        scheduler.OnWake(1, kT0);
        const std::vector<int> vecDue = scheduler.CollectDueViews(kT0);
        QCOMPARE(int(vecDue.size()), 1);
        QVERIFY(Contains(vecDue, 1));
        scheduler.OnRendered(1, kT0, false, false);
        QVERIFY(scheduler.CollectDueViews(kT0 + kMsNs).empty());
        scheduler.OnWake(99, kT0 + kMsNs);   // 모르는 화면은 무시
        QVERIFY(scheduler.CollectDueViews(kT0 + kMsNs).empty());
    }

    void hiddenViewIsNeverDue()
    {
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnVisible(1, false);
        QVERIFY(scheduler.CollectDueViews(kT0).empty());
        scheduler.OnWake(1, kT0);
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

        // 깨움도 프레임 시각까지 기다린다(주기당 한 번). 설정이 바뀌면 기다리지 않는다
        scheduler.OnWake(1, kT0 + kMsNs);
        QVERIFY(scheduler.CollectDueViews(kT0 + kMsNs).empty());
        QCOMPARE(scheduler.NextDeadline(kT0 + kMsNs), std::optional<qint64>(kT0 + kFrameNs));
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + kFrameNs), 1));
        scheduler.OnRendered(1, kT0 + kFrameNs, true, false);
        QVERIFY(scheduler.CollectDueViews(kT0 + kFrameNs + kMsNs).empty());
        scheduler.OnConfigChanged(1);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + kFrameNs + kMsNs), 1));
        scheduler.OnRendered(1, kT0 + kFrameNs + kMsNs, true, false);
        QCOMPARE(scheduler.NextDeadline(kT0 + kFrameNs + kMsNs), std::optional<qint64>(kT0 + kFrameNs + kMsNs + kFrameNs));
    }

    void firstWakeAfterIdleIsDueImmediately()
    {
        // 마지막으로 그린 뒤 한 간격 이상 지났으면 깨움을 바로 그린다(키 입력 지연을 늘리지 않는다)
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnRendered(1, kT0, false, false);
        scheduler.OnWake(1, kT0 + kFrameNs);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + kFrameNs), 1));
        QCOMPARE(scheduler.NextDeadline(kT0 + kFrameNs), std::optional<qint64>(kT0 + kFrameNs));
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + 50 * kFrameNs), 1));
        QCOMPARE(scheduler.NextDeadline(kT0 + 50 * kFrameNs), std::optional<qint64>(kT0 + 50 * kFrameNs));
    }

    void wakesWithinIntervalWaitForFrameTime()
    {
        // 이어서 오는 깨움은 다음 마감(마지막 마감 + 그 화면의 간격)에 그린다. 깜빡임 프레임도 그린 프레임이다
        constexpr qint64 k120HzNs = 8'333'333;
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.AddView(2);
        scheduler.SetFrameInterval(2, k120HzNs);
        scheduler.OnRendered(1, kT0, false, false);
        scheduler.OnRendered(2, kT0, false, false);
        scheduler.OnWake(1, kT0 + kMsNs);
        scheduler.OnWake(2, kT0 + kMsNs);
        QVERIFY(scheduler.CollectDueViews(kT0 + 2 * kMsNs).empty());
        QCOMPARE(scheduler.NextDeadline(kT0 + 2 * kMsNs), std::optional<qint64>(kT0 + k120HzNs));
        QVERIFY(scheduler.CollectDueViews(kT0 + k120HzNs - 1).empty());
        std::vector<int> vecDue = scheduler.CollectDueViews(kT0 + k120HzNs);
        QCOMPARE(int(vecDue.size()), 1);
        QVERIFY(Contains(vecDue, 2));
        scheduler.OnRendered(2, kT0 + k120HzNs, false, false);
        QCOMPARE(scheduler.NextDeadline(kT0 + k120HzNs), std::optional<qint64>(kT0 + kFrameNs));
        QVERIFY(scheduler.CollectDueViews(kT0 + kFrameNs - 1).empty());
        vecDue = scheduler.CollectDueViews(kT0 + kFrameNs);
        QCOMPARE(int(vecDue.size()), 1);
        QVERIFY(Contains(vecDue, 1));

        // 마감보다 조금(3ms) 늦게 그려도 위상은 그대로: 다음 마감은 이전 마감 + 간격
        scheduler.OnRendered(1, kT0 + kFrameNs + 3 * kMsNs, false, false);
        scheduler.OnWake(1, kT0 + kFrameNs + 4 * kMsNs);
        QVERIFY(scheduler.CollectDueViews(kT0 + 2 * kFrameNs - 1).empty());
        QCOMPARE(scheduler.NextDeadline(kT0 + kFrameNs + 4 * kMsNs), std::optional<qint64>(kT0 + 2 * kFrameNs));

        // 깜빡임으로 그린 프레임 바로 뒤의 깨움도 한 간격을 기다린다
        FrameScheduler blink;
        blink.AddView(1);
        blink.OnFocus(1, true);
        blink.OnRendered(1, kT0, false, true);
        QVERIFY(Contains(blink.CollectDueViews(kT0 + kBlinkNs), 1));
        blink.OnRendered(1, kT0 + kBlinkNs, false, true);
        blink.OnWake(1, kT0 + kBlinkNs + kMsNs);
        QVERIFY(blink.CollectDueViews(kT0 + kBlinkNs + kMsNs).empty());
        QCOMPARE(blink.NextDeadline(kT0 + kBlinkNs + kMsNs), std::optional<qint64>(kT0 + kBlinkNs + kFrameNs));
    }

    void wakeCapKeepsPhaseWhenDrawnSlightlyLate()
    {
        // 이어서 쏟는 깨움은 매번 조금(0.5ms) 늦게 그려도 마감이 t0 + k*간격에 머문다(위상 고정): 늦음이 쌓여 주사율보다 덜 그리지 않는다
        constexpr qint64 kLateNs = 500'000;
        constexpr int kFrames = 20;
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnRendered(1, kT0, false, false);
        qint64 llDrawn = kT0;
        for (int iFrame = 1; iFrame <= kFrames; ++iFrame)
        {
            scheduler.OnWake(1, llDrawn + kMsNs);   // 그린 직후 또 출력
            const qint64 llDue = kT0 + iFrame * kFrameNs;
            QCOMPARE(scheduler.NextDeadline(llDrawn + kMsNs), std::optional<qint64>(llDue));
            QVERIFY(scheduler.CollectDueViews(llDue - 1).empty());
            QVERIFY(Contains(scheduler.CollectDueViews(llDue + kLateNs), 1));
            llDrawn = llDue + kLateNs;
            scheduler.OnRendered(1, llDrawn, false, false);
        }

        // GPU 가 표시를 못 해 재시도 끝에 그려도 위상은 그대로
        const qint64 llDue = kT0 + (kFrames + 1) * kFrameNs;
        scheduler.OnWake(1, llDrawn + kMsNs);
        scheduler.OnNotReady(1, llDue);
        QVERIFY(Contains(scheduler.CollectDueViews(llDue + kMsNs), 1));
        scheduler.OnRendered(1, llDue + kMsNs, false, false);
        scheduler.OnWake(1, llDue + 2 * kMsNs);
        QCOMPARE(scheduler.NextDeadline(llDue + 2 * kMsNs), std::optional<qint64>(llDue + kFrameNs));
    }

    void wakeCapRestartsAfterIdleOrLongDelay()
    {
        // 마감보다 한 간격 넘게 늦게 그렸으면(GUI 가 조각을 늦게 받음 등) 그 시각부터 다시 센다: 놓친 마감을 따라잡으려 몰아서 그리지 않는다
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnRendered(1, kT0, false, false);
        scheduler.OnWake(1, kT0 + kMsNs);                           // 마감 kT0 + 간격
        const qint64 llLate = kT0 + 2 * kFrameNs + 3 * kMsNs;       // 그 마감보다 한 간격 넘게 늦다
        QVERIFY(Contains(scheduler.CollectDueViews(llLate), 1));
        scheduler.OnRendered(1, llLate, false, false);
        scheduler.OnWake(1, llLate + kMsNs);
        QCOMPARE(scheduler.NextDeadline(llLate + kMsNs), std::optional<qint64>(llLate + kFrameNs));
        QVERIFY(scheduler.CollectDueViews(llLate + kFrameNs - 1).empty());
        scheduler.OnRendered(1, llLate + kFrameNs, false, false);   // 제때 그렸다

        // 한가했다: 마감이 지난 뒤에 온 깨움은 바로 그리고 그 시각부터 다시 센다
        const qint64 llIdleWake = llLate + 2 * kFrameNs + 5 * kMsNs;
        scheduler.OnWake(1, llIdleWake);
        QVERIFY(Contains(scheduler.CollectDueViews(llIdleWake), 1));
        QCOMPARE(scheduler.NextDeadline(llIdleWake), std::optional<qint64>(llIdleWake));
        scheduler.OnRendered(1, llIdleWake, false, false);
        scheduler.OnWake(1, llIdleWake + kMsNs);
        QCOMPARE(scheduler.NextDeadline(llIdleWake + kMsNs), std::optional<qint64>(llIdleWake + kFrameNs));

        // 마감 바로 뒤(0.2ms)에 온 깨움도 그 마감에는 그릴 일이 없었으므로 그 시각부터 센다
        scheduler.OnRendered(1, llIdleWake + kFrameNs, false, false);   // 제때 그렸다: 다음 마감 llIdleWake + 2*간격
        const qint64 llJustAfter = llIdleWake + 2 * kFrameNs + 200'000;
        scheduler.OnWake(1, llJustAfter);
        QVERIFY(Contains(scheduler.CollectDueViews(llJustAfter), 1));
        scheduler.OnRendered(1, llJustAfter, false, false);
        scheduler.OnWake(1, llJustAfter + kMsNs);
        QCOMPARE(scheduler.NextDeadline(llJustAfter + kMsNs), std::optional<qint64>(llJustAfter + kFrameNs));
    }

    void configVisibleAndFocusChangesAreNotCapped()
    {
        // 크기·글꼴·테마, 다시 보임, 포커스는 화면이 틀린 채로 남으므로 간격을 기다리지 않는다(GUI 가 내는 드문 일)
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnRendered(1, kT0, false, false);
        scheduler.OnConfigChanged(1);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + kMsNs), 1));
        QCOMPARE(scheduler.NextDeadline(kT0 + kMsNs), std::optional<qint64>(kT0 + kMsNs));
        scheduler.OnRendered(1, kT0 + kMsNs, false, false);

        scheduler.OnVisible(1, false);
        scheduler.OnVisible(1, true);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + 2 * kMsNs), 1));
        scheduler.OnRendered(1, kT0 + 2 * kMsNs, false, false);

        scheduler.OnFocus(1, true);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + 3 * kMsNs), 1));
        scheduler.OnRendered(1, kT0 + 3 * kMsNs, false, false);

        // 그린 뒤의 깨움은 다시 간격을 지킨다
        scheduler.OnWake(1, kT0 + 4 * kMsNs);
        QVERIFY(scheduler.CollectDueViews(kT0 + 4 * kMsNs).empty());
        QCOMPARE(scheduler.NextDeadline(kT0 + 4 * kMsNs), std::optional<qint64>(kT0 + 3 * kMsNs + kFrameNs));
    }

    void pendingPatchHoldsViewUntilReceived()
    {
        // GUI 가 아직 받지 않은 조각이 있으면 그 화면은 그릴 일이 있어도 due 도 마감도 없다(그릴 일은 남는다)
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.AddView(2);
        scheduler.OnFocus(1, true);
        scheduler.OnRendered(1, kT0, true, true);   // 애니메이션 + 깜빡임
        scheduler.OnRendered(2, kT0, false, false);
        scheduler.OnPatchSent(1);
        scheduler.OnWake(1, kT0 + kMsNs);
        scheduler.OnConfigChanged(1);
        QVERIFY(scheduler.CollectDueViews(kT0 + kBlinkNs).empty());
        QVERIFY(!scheduler.NextDeadline(kT0 + kBlinkNs).has_value());

        // 받으면 남은 일을 그린다
        scheduler.OnPatchReceived(1);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + kBlinkNs), 1));
        QCOMPARE(scheduler.NextDeadline(kT0 + kBlinkNs), std::optional<qint64>(kT0 + kBlinkNs));

        // 깨움만 남았으면 받은 뒤에도 간격을 지킨다
        scheduler.OnRendered(1, kT0 + kBlinkNs, false, false);
        scheduler.OnPatchSent(1);
        scheduler.OnWake(1, kT0 + kBlinkNs + kMsNs);
        scheduler.OnPatchReceived(1);
        QVERIFY(scheduler.CollectDueViews(kT0 + kBlinkNs + kMsNs).empty());
        QCOMPARE(scheduler.NextDeadline(kT0 + kBlinkNs + kMsNs), std::optional<qint64>(kT0 + kBlinkNs + kFrameNs));
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + kBlinkNs + kFrameNs), 1));

        // 다른 화면은 막지 않는다. 모르는 화면·지운 화면은 무시
        scheduler.OnWake(2, kT0 + kFrameNs);
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + kFrameNs), 2));
        scheduler.OnPatchReceived(99);
        scheduler.OnPatchSent(99);
        scheduler.OnPatchSent(2);
        scheduler.RemoveView(2);
        scheduler.AddView(2);   // 다시 붙인 화면은 받지 않은 조각이 없다
        QVERIFY(Contains(scheduler.CollectDueViews(kT0 + kFrameNs), 2));
    }

    void notReadyRetryBacksOffWithinFrameFraction()
    {
        // 표시를 못 하면 1ms 뒤에 다시 보고, 잇달아 못 하면 간격을 두 배씩 늘리되 프레임 간격의 1/4 을 넘지 않는다
        constexpr qint64 k120HzNs = 8'333'333;
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.SetFrameInterval(1, k120HzNs);
        scheduler.OnRendered(1, kT0, false, false);
        scheduler.OnWake(1, kT0 + kMsNs);
        qint64 llNow = kT0 + k120HzNs;
        const qint64 llCap = k120HzNs / 4;
        const std::vector<qint64> vecExpected = {kMsNs, 2 * kMsNs, llCap, llCap, llCap};
        for (const qint64 llDelay : vecExpected)
        {
            scheduler.OnNotReady(1, llNow);
            QCOMPARE(scheduler.NextDeadline(llNow), std::optional<qint64>(llNow + llDelay));
            QVERIFY(scheduler.CollectDueViews(llNow + llDelay - 1).empty());   // 그 전에는 다시 보지 않는다
            QVERIFY(Contains(scheduler.CollectDueViews(llNow + llDelay), 1));
            llNow += llDelay;
        }
        // 그리면 처음부터
        scheduler.OnRendered(1, llNow, false, false);
        QVERIFY(!scheduler.NextDeadline(llNow).has_value());
        scheduler.OnWake(1, llNow);
        llNow += k120HzNs;
        scheduler.OnNotReady(1, llNow);
        QCOMPARE(scheduler.NextDeadline(llNow), std::optional<qint64>(llNow + kMsNs));

        // 60Hz 는 1, 2, 4ms 다음 1/4(4.17ms) 에서 멈춘다
        FrameScheduler slow;
        slow.AddView(1);
        qint64 llSlow = kT0;
        const std::vector<qint64> vecSlow = {kMsNs, 2 * kMsNs, 4 * kMsNs, kFrameNs / 4, kFrameNs / 4};
        for (const qint64 llDelay : vecSlow)
        {
            slow.OnNotReady(1, llSlow);
            QCOMPARE(slow.NextDeadline(llSlow), std::optional<qint64>(llSlow + llDelay));
            llSlow += llDelay;
        }
    }

    void animationKeepsPhaseWhenDrawnSlightlyLate()
    {
        // 애니메이션 프레임을 매번 약 1ms 늦게 그려도 다음 애니메이션 마감은 t0 + k*간격에 머문다(위상 고정): 늦음이 쌓여
        // 주사율보다 덜 그리지 않는다
        constexpr qint64 kLateNs = 1'000'000;
        constexpr int kFrames = 20;
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnRendered(1, kT0, true, false);   // 애니메이션 시작: 그 시각부터 센다
        qint64 llDrawn = kT0;
        for (int iFrame = 1; iFrame <= kFrames; ++iFrame)
        {
            const qint64 llDue = kT0 + iFrame * kFrameNs;
            QCOMPARE(scheduler.NextDeadline(llDrawn), std::optional<qint64>(llDue));
            QVERIFY(scheduler.CollectDueViews(llDue - 1).empty());
            QVERIFY(Contains(scheduler.CollectDueViews(llDue + kLateNs), 1));
            llDrawn = llDue + kLateNs;
            scheduler.OnRendered(1, llDrawn, true, false);
        }
        // GPU 가 표시를 못 해 재시도 끝에 그려도 위상은 그대로
        const qint64 llDue = kT0 + (kFrames + 1) * kFrameNs;
        scheduler.OnNotReady(1, llDue);
        scheduler.OnRendered(1, llDue + 2 * kMsNs, true, false);
        QCOMPARE(scheduler.NextDeadline(llDue + 2 * kMsNs), std::optional<qint64>(llDue + kFrameNs));
    }

    void animationRestartsAfterLongDelayOrNewStart()
    {
        // 애니메이션 마감보다 한 간격 넘게 늦게 그렸거나, 애니메이션을 새로 시작했거나, 마감 전에 다른 까닭으로 그렸으면
        // 그 시각부터 다시 센다(놓친 마감을 따라잡으려 몰아서 그리지 않는다)
        FrameScheduler scheduler;
        scheduler.AddView(1);
        scheduler.OnRendered(1, kT0, true, false);
        const qint64 llLate = kT0 + 2 * kFrameNs + 3 * kMsNs;   // 마감(kT0 + 간격)보다 한 간격 넘게 늦다
        scheduler.OnRendered(1, llLate, true, false);
        QCOMPARE(scheduler.NextDeadline(llLate), std::optional<qint64>(llLate + kFrameNs));

        scheduler.OnRendered(1, llLate + kFrameNs, false, false);   // 애니메이션이 끝났다
        const qint64 llRestart = llLate + 3 * kFrameNs + 5 * kMsNs;
        scheduler.OnRendered(1, llRestart, true, false);             // 다시 시작
        QCOMPARE(scheduler.NextDeadline(llRestart), std::optional<qint64>(llRestart + kFrameNs));

        scheduler.OnConfigChanged(1);                                // 마감 전에 설정 바뀜으로 그렸다
        scheduler.OnRendered(1, llRestart + 4 * kMsNs, true, false);
        QCOMPARE(scheduler.NextDeadline(llRestart + 4 * kMsNs), std::optional<qint64>(llRestart + 4 * kMsNs + kFrameNs));
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
        scheduler.OnRendered(1, kT0 - kFrameNs, false, false);
        scheduler.OnWake(1, kT0);
        scheduler.OnNotReady(1, kT0);
        const std::optional<qint64> optDeadline = scheduler.NextDeadline(kT0);
        QVERIFY(optDeadline.has_value());
        QVERIFY(*optDeadline > kT0);   // 바로 다시 돌지 않는다(바쁜 대기 없음)
        QVERIFY(*optDeadline <= kT0 + kMsNs);
        QVERIFY(Contains(scheduler.CollectDueViews(*optDeadline), 1));
        scheduler.OnNotReady(1, *optDeadline);
        const std::optional<qint64> optSecond = scheduler.NextDeadline(*optDeadline);
        QVERIFY(optSecond.has_value());
        QVERIFY(Contains(scheduler.CollectDueViews(*optSecond), 1));
        scheduler.OnRendered(1, *optSecond, false, false);
        QVERIFY(!scheduler.NextDeadline(*optSecond).has_value());
        QVERIFY(scheduler.CollectDueViews(*optSecond + kMsNs).empty());
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
        scheduler.OnWake(1, llNow);
        for (int iPass = 0; iPass < 5; ++iPass)
        {
            scheduler.OnNotReady(1, llNow);
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
