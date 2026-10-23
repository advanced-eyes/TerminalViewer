#pragma once

#include <QFont>
#include <QSize>
#include <QtGlobal>

struct CellMetrics
{
    int iCellWidth = 0;            // 모두 장치 픽셀
    int iCellHeight = 0;
    int iBaseline = 0;             // 셀 위 → 기준선
    int iUnderlineY = 0;           // 셀 위 → 밑줄 윗변
    int iUnderlineThickness = 1;
    int iStrikeY = 0;
    int iStrikeThickness = 1;
    int iLineThickness = 1;        // 박스 가는 선, 커서 막대 = max(1, round(배율))
    qreal dDevicePixelRatio = 1.0;

    static CellMetrics Compute(const QFont& font, qreal devicePixelRatio);
    // 장치 픽셀 크기와 여백(장치 픽셀, 한쪽) → 열×행. 최소 1×1
    QSize ComputeGrid(const QSize& devicePixelSize, int paddingDevicePx) const;
};
