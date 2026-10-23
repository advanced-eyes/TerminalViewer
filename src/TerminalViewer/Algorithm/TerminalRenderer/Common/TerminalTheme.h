#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalFrame.h"

#include <optional>

struct TerminalTheme
{
    TerminalRgb selectionBg;
    std::optional<TerminalRgb> optSelectionFg;   // 없으면 원래 글자색
    TerminalRgb cursorColor;                     // 프레임 cursor.optColor 가 없을 때
    int iPaddingLogicalPx = 4;

    bool operator==(const TerminalTheme& other) const
    {
        return selectionBg == other.selectionBg && optSelectionFg == other.optSelectionFg &&
               cursorColor == other.cursorColor && iPaddingLogicalPx == other.iPaddingLogicalPx;
    }
    bool operator!=(const TerminalTheme& other) const { return !(*this == other); }
};
