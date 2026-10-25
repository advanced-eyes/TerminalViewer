#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h"

#include <QFont>
#include <QImage>
#include <QPoint>
#include <QSizeF>
#include <QStringList>

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
    // 그릴 때 쓰는 글꼴 대체 목록. 순서: baseFont 의 families(), family()(쉼표로 나눈 이름), Qt 가 baseFont 대신 고른 글꼴,
    // 플랫폼 한글 · 이모지 글꼴. 겹치는 이름(대소문자 무시)은 한 번만
    static QStringList BuildFontFamilies(const QFont& baseFont);
    // 컬러 글리프인가. 순수 함수. drawnWithBlackPen: 투명 바탕에 검은 펜으로 그린 글리프.
    // 펜 색을 따른 단색 잉크(알파가 있는 픽셀)는 premultiplied rgb 가 모두 0 이다. rgb 가 0 이 아닌 잉크가 하나라도 있으면
    // 컬러다(회색 · 흰색뿐인 컬러 이모지 ⚪ 🤍 🏳 도). 잉크가 모두 순수한 검정인 컬러 글리프는 단색으로 본다(Segoe UI Emoji 에는 없다)
    static bool IsColorGlyph(const QImage& drawnWithBlackPen);

private:
    QFont m_baseFont;
    CellMetrics m_metrics;
};
