#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h"

#include <QFont>
#include <QImage>
#include <QPoint>
#include <QSizeF>

#include <string>

struct RasterGlyph
{
    QImage image;       // 흑백: Format_Alpha8, 컬러: Format_RGBA8888_Premultiplied, 비었으면 null
    bool bColor = false;
    QPoint offset;      // 첫 칸 셀 왼쪽 위 -> 비트맵 왼쪽 위 (장치 픽셀, 0 이상)
};

class GlyphRasterizer
{
public:
    GlyphRasterizer(const QFont& baseFont, const CellMetrics& metrics);

public:
    // text: 그래핌 하나. cellSpan: 1 또는 2
    RasterGlyph Rasterize(const std::u32string& text, bool bold, bool italic, int cellSpan) const;
    // 글리프 경계를 칸 상자 안에 넣는 배율(1 이하). 순수 함수
    static qreal FitScale(const QSizeF& glyphSize, const QSizeF& box);

private:
    QFont m_baseFont;
    CellMetrics m_metrics;
};
