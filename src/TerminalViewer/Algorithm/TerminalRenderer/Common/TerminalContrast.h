#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalFrame.h"

#include <cstdint>
#include <vector>

// 최소 대비(TerminalTheme::dMinContrastRatio): WCAG 2.x 대비비로 글자색이 배경에서 충분히 보이게 고친다.

// WCAG 2.x 상대 휘도(sRGB -> 선형, 0..1)
double GetRelativeLuminance(const TerminalRgb& rgb);
// WCAG 2.x 대비비 (밝은 휘도 + 0.05) / (어두운 휘도 + 0.05). 1(같은 휘도) .. 21(흰색 / 검정). 순서와 무관하다
double GetContrastRatio(const TerminalRgb& a, const TerminalRgb& b);
// bg 위 fg 의 대비가 minRatio 보다 낮으면 fg 의 밝기를 옮겨 맞춘다. minRatio 가 1 이하 · NaN 이면 fg 그대로, 21 보다 크면 21.
// 먼저 배경 반대쪽(fg 가 bg 보다 밝으면 흰색 쪽, 어두우면 검정 쪽)으로 섞고, 닿지 못하면 다른 쪽으로, 그래도 못 닿으면
// 대비가 더 큰 끝(흰색 또는 검정)을 고른다. 흰색 · 검정과 섞어 옮기므로 색조가 남는다. 목표에 닿는 가장 작은 섞음을 고른다
TerminalRgb EnsureContrast(const TerminalRgb& fg, const TerminalRgb& bg, double minRatio);
// 고치지 않는 글자: 상자 · 블록 문자(U+2500-U+259F, BoxGlyphPainter) · Powerline 모양(U+E0B0-U+E0D7) ·
// Symbols for Legacy Computing(U+1FB00-U+1FBFF, U+1CC00-U+1CEBF). Ghostty 의 noMinContrast 와 같은 범위다(xterm.js 도
// 상자 · 블록 · Powerline 을 뺀다). 글자가 아니라 그림이라 글자색 · 배경색 짝에 뜻이 있다(반칸 화소, 구분선)
bool IsContrastExempt(char32_t codepoint);

// (글자색, 배경색) -> EnsureContrast 결과. 결과는 EnsureContrast 와 늘 같다(캐시는 빠르게만 한다). 한 스레드에서 쓴다.
// 바로 앞 짝을 기억하고, 그 밖은 크기가 정해진 직접 사상 표(짝의 해시로 칸 하나, 겹치면 덮어씀)에 둔다. 표는 켰을 때만 잡는다
class ContrastCache
{
public:
    TerminalRgb Apply(const TerminalRgb& fg, const TerminalRgb& bg);   // 끄면 fg 그대로

public:
    void SetRatio(double minRatio);   // 바뀌면 비운다
    bool IsEnabled() const;           // 1 보다 큰 비율이면 참

private:
    struct Slot
    {
        uint64_t llKey;               // kEmptyKey 면 빈 칸
        TerminalRgb result;
    };

    double m_dRatio = 1.0;
    bool m_bEnabled = false;
    std::vector<Slot> m_vecSlots;
    bool m_bHasLast = false;
    uint64_t m_llLastKey = 0;
    TerminalRgb m_lastResult;
};
