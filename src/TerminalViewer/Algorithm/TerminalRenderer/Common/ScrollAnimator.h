#pragma once

#include <QtGlobal>

// 픽셀 스크롤 오프셋 애니메이션(순수 로직). 단위는 장치 픽셀.
// 누적 위치 C 가 목표 T 를 향해 지수 감쇠로 움직인다: C += (T − C)(1 − e^(−dt/τ)), |T − C| < 0.5 면 C = T.
// 줄 = floor(C / 셀 높이), 오프셋 = round(C − 줄 × 셀 높이). 오프셋이 셀 높이로 반올림되면 줄 +1, 오프셋 0.
// 오프셋은 격자를 위로 민 양이다.
class ScrollAnimator
{
public:
    static constexpr double kTimeConstantMs = 60.0;
    void AddPixels(double pixels);           // + = 아래(최근) 쪽
    // 진행한다. 반환: 이번에 넘은 줄 수(±). nowNs 는 단조 시각. 처음 부르면 시각만 기록하고 0
    int Advance(qint64 nowNs, int cellHeight);
    int GetOffsetPx() const;                 // [0, cellHeight), 정수
    bool IsAnimating() const;                // |목표 − 현재| ≥ 0.5px
    void StopAtEdge();                       // 끝에 닿음: 목표 = 현재 줄 경계, 오프셋 0, 애니메이션 끝
    void Reset();

private:
    // C 와 T 는 마지막으로 돌려준 줄 경계를 0 으로 하는 상대 값이다(줄을 돌려줄 때마다 줄 × 셀 높이를 뺀다).
    double m_dCurrent = 0.0;
    double m_dTarget = 0.0;
    qint64 m_llLastNs = 0;
    bool m_bHasTime = false;
    int m_iOffsetPx = 0;
};
