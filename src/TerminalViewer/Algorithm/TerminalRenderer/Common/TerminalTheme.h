#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalFrame.h"

#include <cmath>
#include <optional>

struct TerminalTheme
{
    TerminalRgb selectionBg;
    std::optional<TerminalRgb> optSelectionFg;   // 없으면 원래 글자색
    TerminalRgb cursorColor;                     // 프레임 cursor.optColor 가 없을 때
    int iPaddingLogicalPx = 4;
    // 최소 대비: WCAG 2.x 대비비(1..21). 글자색과 배경색의 대비가 이보다 낮으면 글자색의 밝기를 옮겨 맞춘다.
    // 1 이하(기본 1) · NaN 은 끔, 21 보다 크면 21. 규칙: TerminalContrast.h, README §5
    double dMinContrastRatio = 1.0;

    bool operator==(const TerminalTheme& other) const
    {
        const bool bSameContrast = dMinContrastRatio == other.dMinContrastRatio ||
                                   (std::isnan(dMinContrastRatio) && std::isnan(other.dMinContrastRatio));
        return selectionBg == other.selectionBg && optSelectionFg == other.optSelectionFg &&
               cursorColor == other.cursorColor && iPaddingLogicalPx == other.iPaddingLogicalPx && bSameContrast;
    }
    bool operator!=(const TerminalTheme& other) const { return !(*this == other); }
};
