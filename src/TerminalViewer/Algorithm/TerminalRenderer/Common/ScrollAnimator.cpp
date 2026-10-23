#include "TerminalViewer/Algorithm/TerminalRenderer/Common/ScrollAnimator.h"

#include <cmath>

namespace
{
constexpr double kNsPerMs = 1'000'000.0;
constexpr double kSnapPx = 0.5;   // 목표와 이만큼 가까우면 목표에 붙인다
}

void ScrollAnimator::AddPixels(double pixels)
{
    m_dTarget += pixels;
}

int ScrollAnimator::Advance(qint64 nowNs, int cellHeight)
{
    if (!m_bHasTime)
    {
        m_llLastNs = nowNs;
        m_bHasTime = true;
        return 0;
    }
    const double dDtMs = nowNs > m_llLastNs ? double(nowNs - m_llLastNs) / kNsPerMs : 0.0;
    m_llLastNs = nowNs;
    if (cellHeight <= 0)
    {
        return 0;
    }

    m_dCurrent += (m_dTarget - m_dCurrent) * (1.0 - std::exp(-dDtMs / kTimeConstantMs));
    if (std::abs(m_dTarget - m_dCurrent) < kSnapPx)
    {
        m_dCurrent = m_dTarget;
    }

    // floor(C / h) 와 round(C − 줄 × h)(셀 높이로 반올림되면 다음 줄) 은 정수 위치 floor(C + 0.5) 를 h 로 나눈 몫·나머지와 같다.
    const long long llPosition = static_cast<long long>(std::floor(m_dCurrent + 0.5));
    long long llLines = llPosition / cellHeight;
    if (llPosition % cellHeight < 0)
    {
        --llLines;
    }
    m_iOffsetPx = int(llPosition - llLines * cellHeight);

    // 돌려준 줄만큼 기준을 옮겨 C·T 를 작게 유지한다(정수 배라 다음 계산 결과는 같다).
    const double dShift = double(llLines) * cellHeight;
    m_dCurrent -= dShift;
    m_dTarget -= dShift;
    return int(llLines);
}

int ScrollAnimator::GetOffsetPx() const
{
    return m_iOffsetPx;
}

bool ScrollAnimator::IsAnimating() const
{
    return std::abs(m_dTarget - m_dCurrent) >= kSnapPx;
}

void ScrollAnimator::StopAtEdge()
{
    // 기준을 마지막 줄 경계로 옮겨 두었으므로 현재 줄 경계는 0 이다.
    m_dCurrent = 0.0;
    m_dTarget = 0.0;
    m_iOffsetPx = 0;
}

void ScrollAnimator::Reset()
{
    m_dCurrent = 0.0;
    m_dTarget = 0.0;
    m_llLastNs = 0;
    m_bHasTime = false;
    m_iOffsetPx = 0;
}
