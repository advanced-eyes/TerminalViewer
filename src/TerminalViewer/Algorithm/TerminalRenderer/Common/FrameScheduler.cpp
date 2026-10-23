#include "TerminalViewer/Algorithm/TerminalRenderer/Common/FrameScheduler.h"

#include <algorithm>

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

void FrameScheduler::OnWake(int viewId)
{
    auto it = m_mapViews.find(viewId);
    if (it != m_mapViews.end())
    {
        it->second.bDirty = true;
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
    it->second.bHasBlinkEpoch = false;     // 다음에 그릴 때 보이는 커서로 주기를 다시 시작한다
}

void FrameScheduler::OnConfigChanged(int viewId)
{
    OnWake(viewId);
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
    view.bDirty = false;
    view.bRetry = false;
    view.bAnimating = animating;
    if (animating)
    {
        view.llNextAnimNs = nowNs + view.llFrameIntervalNs;
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

void FrameScheduler::OnNotReady(int viewId)
{
    auto it = m_mapViews.find(viewId);
    if (it != m_mapViews.end())
    {
        // 그려야 할 일은 남기되 바로 다시 돌지 않고 kRetryDelayNs 뒤에 다시 본다.
        it->second.bDirty = false;
        it->second.bRetry = true;
    }
}

std::vector<int> FrameScheduler::CollectDueViews(qint64 nowNs) const
{
    std::vector<int> vecDue;
    for (const auto& [iViewId, view] : m_mapViews)
    {
        if (!view.bVisible)
        {
            continue;
        }
        const bool bBlinkDue = BlinkActive(view) && BlinkOnAt(view, nowNs) != view.bLastBlinkOn;
        const bool bAnimationDue = view.bAnimating && nowNs >= view.llNextAnimNs;
        if (view.bDirty || view.bRetry || bAnimationDue || bBlinkDue)
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
        if (!view.bVisible)
        {
            continue;
        }
        if (view.bRetry)
        {
            // 표시를 못 하는 동안 깨움·깜빡임·애니메이션 마감이 지났어도 재시도 간격보다 일찍 깨지 않는다.
            funcConsider(nowNs + kRetryDelayNs);
            continue;
        }
        if (view.bDirty)
        {
            funcConsider(nowNs);
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
