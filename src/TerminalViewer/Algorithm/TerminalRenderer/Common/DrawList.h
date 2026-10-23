#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalFrame.h"

#include <cstddef>
#include <cstdint>
#include <vector>

enum class EQuadKind : uint8_t { RECT = 0, GLYPH_MONO = 1, GLYPH_COLOR = 2 };

struct DrawQuad                    // GPU 인스턴스와 같은 배치 (stride 20)
{
    int16_t iX = 0, iY = 0;        // 장치 픽셀. iX 는 격자 왼쪽 기준, iY 는 행 위 기준
    uint16_t uW = 0, uH = 0;
    uint16_t uAtlasX = 0, uAtlasY = 0;
    uint32_t uColor = 0;           // premultiplied RGBA8: R | G<<8 | B<<16 | A<<24
    EQuadKind eKind = EQuadKind::RECT;
    uint8_t uReserved = 0;
    int16_t iRow = 0;              // 뷰포트 행(위 여유 음수, 아래 여유 rows 이상)
};
static_assert(sizeof(DrawQuad) == 20);
static_assert(offsetof(DrawQuad, iX) == 0);
static_assert(offsetof(DrawQuad, uW) == 4);
static_assert(offsetof(DrawQuad, uAtlasX) == 8);
static_assert(offsetof(DrawQuad, uColor) == 12);
static_assert(offsetof(DrawQuad, eKind) == 16);
static_assert(offsetof(DrawQuad, iRow) == 18);

uint32_t PackColor(const TerminalRgb& rgb, uint8_t alpha = 255);   // premultiply 한 뒤 묶는다

struct DrawList
{
    std::vector<DrawQuad> vecQuads;  // [배경 | 글리프 | 장식·커서]
    int iBackgroundCount = 0;
    int iGlyphCount = 0;
    int iDecorationCount = 0;
    TerminalRgb clearColor;          // 프레임 기본 배경
    int iCols = 0, iRows = 0;
    int iPaddingPx = 0;              // 장치 픽셀
    int iCellWidth = 0, iCellHeight = 0;
    int iScrollOffsetPx = 0;         // [0, 셀 높이)
    uint32_t uAtlasGeneration = 0;
    bool bFullRedraw = true;
    std::vector<int> vecDirtyRows;   // CPU 부분 합성용(뷰포트 행)
};
