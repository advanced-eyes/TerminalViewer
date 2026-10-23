#pragma once

#include <QtGlobal>

#include <map>
#include <optional>
#include <vector>

// 어느 화면을 언제 그릴지 정한다(깨움, 스크롤 애니메이션, 커서 깜빡임, 표시 실패 재시도). 순수 로직.
// 시각은 단조 시각(ns). 렌더 스레드 전용(잠금 없음).
// 숨은 화면은 due 도 마감도 만들지 않는다(그리지 않는 것이 정확성 요구다. 스펙 §5).
// 애니메이션 중인 화면은 그 화면의 프레임 간격(SetFrameInterval, 기본 60Hz)마다 한 번만 due 다.
// 표시를 못 해 다시 보는 화면은 그동안 마감이 지금 + kRetryDelayNs 로 묶인다(바쁜 대기 없음).
class FrameScheduler
{
public:
    static constexpr qint64 kBlinkIntervalNs = 530'000'000;
    static constexpr qint64 kRetryDelayNs = 1'000'000;   // 표시를 못 한 화면을 다시 볼 때까지
    static constexpr qint64 kDefaultFrameIntervalNs = 16'666'667;   // 주사율을 받지 않은 화면(60Hz)

    void AddView(int viewId);                    // 보이는 상태, 첫 프레임을 그려야 함
    void RemoveView(int viewId);
    void SetFrameInterval(int viewId, qint64 frameIntervalNs);   // 그 화면의 애니메이션 프레임 간격. 0 이하는 무시
    void OnWake(int viewId);
    void OnVisible(int viewId, bool visible);    // 다시 보이면 그려야 함
    void OnFocus(int viewId, bool focused);
    void OnConfigChanged(int viewId);            // 크기·배율·글꼴·테마
    void OnActivity(int viewId, qint64 nowNs);   // 출력·입력: 커서를 보이게 하고 깜빡임 주기를 다시 시작
    void OnRendered(int viewId, qint64 nowNs, bool animating, bool cursorBlinks);
    void OnNotReady(int viewId);                 // 표시를 못 함: 다음 차례에 다시
    std::vector<int> CollectDueViews(qint64 nowNs) const;   // 보이는 화면 중 그릴 것
    std::optional<qint64> NextDeadline(qint64 nowNs) const;
    bool IsBlinkOn(int viewId, qint64 nowNs) const;

private:
    struct ViewState
    {
        bool bVisible = true;
        bool bFocused = false;
        bool bDirty = true;           // 깨움·설정 변경·다시 보임·첫 프레임
        bool bRetry = false;          // 표시를 못 해 다시 봐야 함
        bool bAnimating = false;      // 마지막으로 그릴 때 스크롤 애니메이션 중이었음
        qint64 llFrameIntervalNs = kDefaultFrameIntervalNs;
        qint64 llNextAnimNs = 0;      // 애니메이션 다음 프레임 시각(마지막으로 그린 시각 + 프레임 간격)
        bool bCursorBlinks = false;   // 마지막으로 그린 프레임의 커서가 깜빡임 커서
        bool bHasBlinkEpoch = false;
        qint64 llBlinkEpochNs = 0;    // 깜빡임 주기 시작(이때 커서가 보임)
        bool bLastBlinkOn = true;     // 마지막으로 그린 깜빡임 상태
    };

    static bool BlinkActive(const ViewState& view);
    static bool BlinkOnAt(const ViewState& view, qint64 nowNs);

    std::map<int, ViewState> m_mapViews;
};
