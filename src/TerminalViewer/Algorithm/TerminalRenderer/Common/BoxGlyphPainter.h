#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h"

#include <QImage>

enum class EUnderlineSprite { CURLY, DOTTED, DASHED };

// 박스 그리기(U+2500-257F)와 블록 요소(U+2580-259F)를 셀 크기 비트맵으로 직접 그린다.
// 글꼴에 의존하지 않으며, 코드는 Unicode 문자 이름만 근거로 새로 썼다.
namespace BoxGlyphPainter
{
bool Handles(char32_t codepoint);                                   // U+2500-U+259F
// Format_Alpha8, 정확히 iCellWidth x iCellHeight
QImage Paint(char32_t codepoint, const CellMetrics& metrics);
// Format_Alpha8. 폭 iCellWidth, 높이 = min(3 x iUnderlineThickness, iCellHeight - GetUnderlineSpriteTop(metrics))
QImage PaintUnderline(EUnderlineSprite sprite, const CellMetrics& metrics);
// 무늬를 그릴 셀 안 윗변 = max(0, iUnderlineY - iUnderlineThickness)
int GetUnderlineSpriteTop(const CellMetrics& metrics);
}
