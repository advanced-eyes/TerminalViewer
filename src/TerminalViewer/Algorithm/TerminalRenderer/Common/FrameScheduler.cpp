#include "TerminalViewer/Algorithm/TerminalRenderer/Common/FrameScheduler.h"

#include <algorithm>
#include <limits>

bool FrameScheduler::BlinkActive(const ViewState& view)
{
    return view.bFocused && view.bCursorBlinks && view.bHasBlinkEpoch;
}

bool FrameScheduler::BlinkOnAt(const ViewState& view, qint64 nowNs)
{
    if (!BlinkActive(view) || nowNs < view.llBlinkEpochNs)
    {
        return true;
    }
    return ((nowNs - view.llBlinkEpochNs) / kBlinkIntervalNs) % 2 == 0;
}

qint64 FrameScheduler::ComputeDirtyDueNs(const ViewState& view)
{
    // 설정·보임·포커스·첫 프레임은 바로. 깨움은 위상 기준 시각 + 간격(한가했으면 그 시각은 이미 지났다).
    if (view.bImmediate || !view.bHasFrame)
    {
        return std::numeric_limits<qint64>::min();
    }
    return view.llPhaseNs + view.llFrameIntervalNs;
}

qint64 FrameScheduler::ComputeRetryDelayNs(int notReadyStreak, qint64 frameIntervalNs)
{
    const qint64 llCap = std::max(kRetryDelayNs, frameIntervalNs / kRetryIntervalDivisor);
    qint64 llDelay = kRetryDelayNs;
    for (int iStreak = 1; iStreak < notReadyStreak && llDelay < llCap; ++iStreak)
    {
        llDelay *= 2;
    }
    return std::min(llDelay, llCap);
}

void FrameScheduler::AddView(int viewId)
{
    m_mapViews[viewId] = ViewState();
}

void FrameScheduler::RemoveView(int viewId)
{
    m_mapViews.erase(viewId);
}

void FrameScheduler::SetFrameInterval(int viewId, qint64 frameIntervalNs)
{
    auto it = m_mapViews.find(viewId);
    if (it == m_mapViews.end() || frameIntervalNs <= 0)
    {
        return;
    }
    it->second.llFrameIntervalNs = frameIntervalNs;
}

void FrameScheduler::OnWake(int viewId, qint64 wakeNs)
{
    auto it = m_mapViews.find(viewId);
    if (it == m_mapViews.end())
    {
        return;
    }
    ViewState& view = it->second;
    view.bDirty = true;
    if (!view.bHasPendingWake)
    {
        view.bHasPendingWake = true;
        view.llFirstWakeNs = wakeNs;
    }
}

void FrameScheduler::OnVisible(int viewId, bool visible)
{
    auto it = m_mapViews.find(viewId);
    if (it == m_mapViews.end())
    {
        return;
    }
    if (visible && !it->second.bVisible)
    {
        it->second.bDirty = true;
        it->second.bImmediate = true;
    }
    it->second.bVisible = visible;
}

void FrameScheduler::OnFocus(int viewId, bool focused)
{
    auto it = m_mapViews.find(viewId);
    if (it == m_mapViews.end() || it->second.bFocused == focused)
    {
        return;
    }
    it->second.bFocused = focused;
    it->second.bDirty = true;              // 포커스에 따라 커서 모양이 바뀐다
    it->second.bImmediate = true;
    it->second.bHasBlinkEpoch = false;     // 다음에 그릴 때 보이는 커서로 주기를 다시 시작한다
}

void FrameScheduler::OnConfigChanged(int viewId)
{
    auto it = m_mapViews.find(viewId);
    if (it != m_mapViews.end())
    {
        it->second.bDirty = true;
        it->second.bImmediate = true;
    }
}

void FrameScheduler::OnActivity(int viewId, qint64 nowNs)
{
    auto it = m_mapViews.find(viewId);
    if (it != m_mapViews.end())
    {
        it->second.bHasBlinkEpoch = true;
        it->second.llBlinkEpochNs = nowNs;
    }
}

void FrameScheduler::OnRendered(int viewId, qint64 nowNs, bool animating, bool cursorBlinks)
{
    auto it = m_mapViews.find(viewId);
    if (it == m_mapViews.end())
    {
        return;
    }
    ViewState& view = it->second;
    // 위상: 마감 전부터 기다린 깨움을 마감 뒤 한 간격 안에 그렸으면 다음 마감은 이 마감 + 간격(늦음이 쌓이지 않는다).
    // 한가했다 온 깨움, 한 간격 넘게 늦음, 마감 전에 그림, 첫 프레임은 그린 시각부터 다시 센다(몰아서 그리지 않는다).
    const qint64 llDue = view.llPhaseNs + view.llFrameIntervalNs;
    const bool bOnTime = view.bHasFrame && view.bHasPendingWake && view.llFirstWakeNs < llDue && nowNs >= llDue &&
                         nowNs - llDue < view.llFrameIntervalNs;
    view.llPhaseNs = bOnTime ? llDue : nowNs;
    // 애니메이션도 같은 규칙: 애니메이션 중이던 화면을 그 마감 뒤 한 간격 안에 그렸으면 다음 = 이 마감 + 간격. 새로 시작했거나,
    // 한 간격 넘게 늦었거나, 마감 전에 다른 까닭으로 그렸으면 그린 시각 + 간격.
    const bool bAnimOnTime = view.bAnimating && nowNs >= view.llNextAnimNs && nowNs - view.llNextAnimNs < view.llFrameIntervalNs;
    view.bHasPendingWake = false;
    view.bDirty = false;
    view.bImmediate = false;
    view.bRetry = false;
    view.iNotReadyStreak = 0;
    view.bHasFrame = true;
    view.bAnimating = animating;
    if (animating)
    {
        view.llNextAnimNs = (bAnimOnTime ? view.llNextAnimNs : nowNs) + view.llFrameIntervalNs;
    }
    view.bCursorBlinks = cursorBlinks;
    if (cursorBlinks && view.bFocused)
    {
        if (!view.bHasBlinkEpoch)
        {
            view.bHasBlinkEpoch = true;
            view.llBlinkEpochNs = nowNs;
        }
    }
    else
    {
        view.bHasBlinkEpoch = false;
    }
    view.bLastBlinkOn = BlinkOnAt(view, nowNs);
}

void FrameScheduler::OnNotReady(int viewId, qint64 nowNs)
{
    auto it = m_mapViews.find(viewId);
    if (it == m_mapViews.end())
    {
        return;
    }
    // 그려야 할 일은 남기되 바로 다시 돌지 않고 재시도 간격 뒤에 다시 본다(잇달아 못 하면 간격을 늘린다).
    // 재시도하는 동안 due · 마감은 재시도 시각만 본다. 그릴 일(bDirty · 기다린 깨움)은 그대로 두어 그린 뒤 위상을 정하는 데 쓴다.
    ViewState& view = it->second;
    view.bRetry = true;
    ++view.iNotReadyStreak;
    view.llRetryAtNs = nowNs + ComputeRetryDelayNs(view.iNotReadyStreak, view.llFrameIntervalNs);
}

void FrameScheduler::OnPatchSent(int viewId)
{
    auto it = m_mapViews.find(viewId);
    if (it != m_mapViews.end())
    {
        it->second.bPatchPending = true;
    }
}

void FrameScheduler::OnPatchReceived(int viewId)
{
    auto it = m_mapViews.find(viewId);
    if (it != m_mapViews.end())
    {
        it->second.bPatchPending = false;
    }
}

std::vector<int> FrameScheduler::CollectDueViews(qint64 nowNs) const
{
    std::vector<int> vecDue;
    for (const auto& [iViewId, view] : m_mapViews)
    {
        if (!view.bVisible || view.bPatchPending)
        {
            continue;
        }
        if (view.bRetry)
        {
            if (nowNs >= view.llRetryAtNs)
            {
                vecDue.push_back(iViewId);
            }
            continue;
        }
        const bool bBlinkDue = BlinkActive(view) && BlinkOnAt(view, nowNs) != view.bLastBlinkOn;
        const bool bAnimationDue = view.bAnimating && nowNs >= view.llNextAnimNs;
        const bool bDirtyDue = view.bDirty && nowNs >= ComputeDirtyDueNs(view);
        if (bDirtyDue || bAnimationDue || bBlinkDue)
        {
            vecDue.push_back(iViewId);
        }
    }
    return vecDue;
}

std::optional<qint64> FrameScheduler::NextDeadline(qint64 nowNs) const
{
    std::optional<qint64> optDeadline;
    const auto funcConsider = [&optDeadline](qint64 deadlineNs)
    {
        optDeadline = optDeadline ? std::min(*optDeadline, deadlineNs) : deadlineNs;
    };
    for (const auto& [iViewId, view] : m_mapViews)
    {
        Q_UNUSED(iViewId);
        if (!view.bVisible || view.bPatchPending)
        {
            continue;   // 받지 않은 조각이 있으면 GUI 가 받았다는 명령이 깨운다
        }
        if (view.bRetry)
        {
            // 표시를 못 하는 동안 깨움·깜빡임·애니메이션 마감이 지났어도 재시도 시각보다 일찍 깨지 않는다.
            funcConsider(std::max(nowNs, view.llRetryAtNs));
            continue;
        }
        if (view.bDirty)
        {
            funcConsider(std::max(nowNs, ComputeDirtyDueNs(view)));
        }
        if (view.bAnimating)
        {
            funcConsider(std::max(nowNs, view.llNextAnimNs));
        }
        if (BlinkActive(view))
        {
            if (BlinkOnAt(view, nowNs) != view.bLastBlinkOn)
            {
                funcConsider(nowNs);
            }
            else
            {
                const qint64 llElapsed = std::max<qint64>(0, nowNs - view.llBlinkEpochNs);
                funcConsider(view.llBlinkEpochNs + (llElapsed / kBlinkIntervalNs + 1) * kBlinkIntervalNs);
            }
        }
    }
    return optDeadline;
}

bool FrameScheduler::IsBlinkOn(int viewId, qint64 nowNs) const
{
    const auto it = m_mapViews.find(viewId);
    return it == m_mapViews.end() || BlinkOnAt(it->second, nowNs);
}
