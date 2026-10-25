#pragma once

#include <QtGlobal>

#include <map>
#include <optional>
#include <vector>

// 어느 화면을 언제 그릴지 정한다(깨움, 스크롤 애니메이션, 커서 깜빡임, 표시 실패 재시도). 순수 로직.
// 시각은 단조 시각(ns). 렌더 스레드 전용(잠금 없음).
// 숨은 화면은 due 도 마감도 만들지 않는다(그리지 않는 것이 정확성 요구다. 스펙 §5).
// 프레임 간격(SetFrameInterval, 기본 60Hz)당 한 번:
// - 깨움(OnWake)으로 그리는 프레임은 그 화면의 프레임 간격당 한 번 이하다. 마감 = 위상 기준 시각 + 간격이고, 그 전에 온 깨움은
//   마감에 due 다(NextDeadline 이 그 시각을 낸다). 마감이 지난 뒤에 온 깨움(한가함)은 바로 due 다.
// - 위상 기준 시각: 마감 전부터 기다린 깨움을 마감 뒤 한 간격 안에 그렸으면(제때) 그 마감으로 둔다. 다음 마감이 이전 마감 + 간격이
//   되어 깨어나는 늦음이 쌓이지 않는다(쏟는 동안 주사율만큼 그린다). 그 밖에는(한가했다 온 깨움, 한 간격 넘게 늦음, 마감 전에 다른
//   까닭으로 그림, 첫 프레임) 그린 시각으로 다시 센다(놓친 마감을 따라잡으려 몰아서 그리지 않는다). 그린 프레임은 깨움·애니메이션·
//   깜빡임·설정 무엇으로 그렸든 센다.
// - 설정 변경(크기·배율·글꼴·테마), 다시 보임, 포커스 변화, 첫 프레임은 간격을 기다리지 않는다(화면이 틀린 채로 남는 드문 일).
// - 애니메이션 중인 화면은 그 간격마다(깨움과 같은 위상 고정: 제때 그렸으면 다음 = 이전 애니메이션 마감 + 간격, 새로 시작 ·
//   한 간격 넘게 늦음 · 마감 전에 그림은 그린 시각 + 간격), 깜빡임은 전환 시각에 due 다.
// GUI 가 아직 받지 않은 조각이 있는 화면(OnPatchSent ~ OnPatchReceived)은 그릴 일을 남긴 채 due 도 마감도 없다(CPU 역압).
// 표시를 못 한 화면은 ComputeRetryDelayNs 뒤에 다시 본다(1ms 에서 시작해 잇달아 못 하면 두 배씩, 프레임 간격의 1/4 까지. 바쁜 대기 없음).
class FrameScheduler
{
public:
    static constexpr qint64 kBlinkIntervalNs = 530'000'000;
    static constexpr qint64 kRetryDelayNs = 1'000'000;   // 표시를 못 한 화면을 처음 다시 볼 때까지
    static constexpr qint64 kRetryIntervalDivisor = 4;   // 재시도 간격의 한도 = 프레임 간격 / 이 값(1ms 보다 작아지지는 않는다)
    static constexpr qint64 kDefaultFrameIntervalNs = 16'666'667;   // 주사율을 받지 않은 화면(60Hz)

    // 잇달아 notReadyStreak 번째로 표시를 못 했을 때 다시 볼 때까지의 간격
    static qint64 ComputeRetryDelayNs(int notReadyStreak, qint64 frameIntervalNs);

    void AddView(int viewId);                    // 보이는 상태, 첫 프레임을 그려야 함
    void RemoveView(int viewId);
    void SetFrameInterval(int viewId, qint64 frameIntervalNs);   // 그 화면의 프레임 간격. 0 이하는 무시
    void OnWake(int viewId, qint64 wakeNs);      // wakeNs: 그 Wake 를 부른 시각(마감 때 이미 그릴 일이 있었는지 본다)
    void OnVisible(int viewId, bool visible);    // 다시 보이면 그려야 함
    void OnFocus(int viewId, bool focused);
    void OnConfigChanged(int viewId);            // 크기·배율·글꼴·테마
    void OnActivity(int viewId, qint64 nowNs);   // 출력·입력: 커서를 보이게 하고 깜빡임 주기를 다시 시작
    void OnRendered(int viewId, qint64 nowNs, bool animating, bool cursorBlinks);
    void OnNotReady(int viewId, qint64 nowNs);   // 표시를 못 함: 재시도 간격 뒤에 다시
    void OnPatchSent(int viewId);                // GUI 에 조각을 보냈다: 받을 때까지 그 화면을 그리지 않는다
    void OnPatchReceived(int viewId);            // GUI 가 조각을 받았다
    std::vector<int> CollectDueViews(qint64 nowNs) const;   // 보이는 화면 중 그릴 것
    std::optional<qint64> NextDeadline(qint64 nowNs) const;
    bool IsBlinkOn(int viewId, qint64 nowNs) const;

private:
    struct ViewState
    {
        bool bVisible = true;
        bool bFocused = false;
        bool bDirty = true;           // 깨움·설정 변경·다시 보임·첫 프레임
        bool bImmediate = true;       // bDirty 를 간격을 기다리지 않고 그린다(설정 변경·다시 보임·포커스·첫 프레임)
        bool bRetry = false;          // 표시를 못 해 다시 봐야 함
        int iNotReadyStreak = 0;      // 잇달아 표시를 못 한 횟수(그리면 0)
        qint64 llRetryAtNs = 0;       // 다시 볼 시각
        bool bPatchPending = false;   // GUI 가 아직 받지 않은 조각이 있다
        bool bHasFrame = false;       // 그린 프레임이 있다
        qint64 llPhaseNs = 0;         // 위상 기준 시각: 다음 깨움 마감 = 이 값 + 간격(제때 그린 프레임은 그 마감, 아니면 그린 시각)
        bool bHasPendingWake = false; // 마지막으로 그린 뒤 깨움이 왔다(표시를 못 해 다시 보는 동안에도 남는다)
        qint64 llFirstWakeNs = 0;     // 그 첫 깨움 시각
        bool bAnimating = false;      // 마지막으로 그릴 때 스크롤 애니메이션 중이었음
        qint64 llFrameIntervalNs = kDefaultFrameIntervalNs;
        qint64 llNextAnimNs = 0;      // 애니메이션 다음 프레임 시각(이전 애니메이션 마감 또는 그린 시각 + 프레임 간격)
        bool bCursorBlinks = false;   // 마지막으로 그린 프레임의 커서가 깜빡임 커서
        bool bHasBlinkEpoch = false;
        qint64 llBlinkEpochNs = 0;    // 깜빡임 주기 시작(이때 커서가 보임)
        bool bLastBlinkOn = true;     // 마지막으로 그린 깜빡임 상태
    };

    static bool BlinkActive(const ViewState& view);
    static bool BlinkOnAt(const ViewState& view, qint64 nowNs);
    static qint64 ComputeDirtyDueNs(const ViewState& view);   // bDirty 를 그릴 수 있는 가장 이른 시각(깨움 마감)

    std::map<int, ViewState> m_mapViews;
};
