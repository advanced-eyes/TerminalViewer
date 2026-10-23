#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphRasterizer.h"

#include <QGlyphRun>
#include <QPainter>
#include <QRectF>
#include <QTextLayout>
#include <QTextOption>

#include <algorithm>
#include <cmath>

namespace
{
#if defined(Q_OS_MAC)
const char* const HANGUL_FAMILY = "Apple SD Gothic Neo";
const char* const EMOJI_FAMILY = "Apple Color Emoji";
#else
const char* const HANGUL_FAMILY = "Malgun Gothic";
const char* const EMOJI_FAMILY = "Segoe UI Emoji";
#endif

QFont BuildFont(const QFont& baseFont, bool bold, bool italic)
{
    QFont font = baseFont;
    font.setFamilies({QStringLiteral("Cascadia Mono"), QString::fromLatin1(HANGUL_FAMILY), QString::fromLatin1(EMOJI_FAMILY)});
    font.setStyleStrategy(QFont::NoSubpixelAntialias);
    font.setBold(bold);
    font.setItalic(italic);
    return font;
}

bool IsGray(QRgb pixel)
{
    return qRed(pixel) == qGreen(pixel) && qGreen(pixel) == qBlue(pixel);
}

// 알파가 있는 픽셀의 경계(장치 픽셀). 없으면 null. outColor 가 있으면 회색 아닌 픽셀 여부를 쓴다.
QRect FindInk(const QImage& image, bool* outColor)
{
    int iMinX = image.width();
    int iMinY = image.height();
    int iMaxX = -1;
    int iMaxY = -1;
    bool bColor = false;
    for (int iY = 0; iY < image.height(); ++iY)
    {
        const QRgb* pRow = reinterpret_cast<const QRgb*>(image.constScanLine(iY));
        for (int iX = 0; iX < image.width(); ++iX)
        {
            if (qAlpha(pRow[iX]) == 0)
                continue;
            iMinX = std::min(iMinX, iX);
            iMaxX = std::max(iMaxX, iX);
            iMinY = std::min(iMinY, iY);
            iMaxY = std::max(iMaxY, iY);
            bColor = bColor || !IsGray(pRow[iX]);
        }
    }
    if (outColor)
        *outColor = bColor;
    if (iMaxX < 0)
        return QRect();
    return QRect(iMinX, iMinY, iMaxX - iMinX + 1, iMaxY - iMinY + 1);
}
}   // namespace

GlyphRasterizer::GlyphRasterizer(const QFont& baseFont, const CellMetrics& metrics)
    : m_baseFont(baseFont)
    , m_metrics(metrics)
{
}

qreal GlyphRasterizer::FitScale(const QSizeF& glyphSize, const QSizeF& box)
{
    qreal dScale = 1.0;
    if (glyphSize.width() > box.width() && glyphSize.width() > 0)
        dScale = std::min(dScale, box.width() / glyphSize.width());
    if (glyphSize.height() > box.height() && glyphSize.height() > 0)
        dScale = std::min(dScale, box.height() / glyphSize.height());
    return dScale;
}

RasterGlyph GlyphRasterizer::Rasterize(const std::u32string& text, bool bold, bool italic, int cellSpan) const
{
    RasterGlyph result;
    const QString str = QString::fromUcs4(text.data(), int(text.size()));
    if (str.trimmed().isEmpty() || m_metrics.iCellWidth <= 0 || m_metrics.iCellHeight <= 0)
        return result;

    const qreal dRatio = m_metrics.dDevicePixelRatio;
    const QSize boxDevice(m_metrics.iCellWidth * cellSpan, m_metrics.iCellHeight);
    const QSizeF boxLogical(boxDevice.width() / dRatio, boxDevice.height() / dRatio);

    const QFont font = BuildFont(m_baseFont, bold, italic);
    QTextLayout layout(str, font);
    QTextOption option;
    option.setWrapMode(QTextOption::NoWrap);
    layout.setTextOption(option);
    layout.beginLayout();
    QTextLine line = layout.createLine();
    layout.endLayout();
    if (!line.isValid())
        return result;
    line.setPosition(QPointF(0, 0));

    // 잉크 경계: 여유를 둔 큰 캔버스에 크기 변환 없이 그려 실제 픽셀로 잰다(상자 왼쪽 위 기준, 논리 좌표).
    const qreal dBaselineShift = m_metrics.iBaseline / dRatio - line.ascent();
    const qreal dMargin = std::ceil(std::max(boxLogical.width(), boxLogical.height()) * 2);
    const QSize probeDevice(int(std::ceil((boxLogical.width() + 2 * dMargin) * dRatio)),
                            int(std::ceil((boxLogical.height() + 2 * dMargin) * dRatio)));
    QImage probe(probeDevice, QImage::Format_ARGB32_Premultiplied);
    probe.fill(Qt::transparent);
    probe.setDevicePixelRatio(dRatio);
    {
        QPainter painter(&probe);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setPen(Qt::black);
        layout.draw(&painter, QPointF(dMargin, dMargin + dBaselineShift));
    }
    const QRect probeInk = FindInk(probe, nullptr);
    if (probeInk.isNull())
        return result;
    const QRectF ink(probeInk.x() / dRatio - dMargin, probeInk.y() / dRatio - dMargin,
                     probeInk.width() / dRatio, probeInk.height() / dRatio);

    const qreal dScale = FitScale(ink.size(), boxLogical);
    qreal dShiftX = 0;
    qreal dShiftY = 0;
    if (dScale < 1.0)
    {
        // 줄인 뒤 칸 상자 가운데에 놓는다.
        dShiftX = boxLogical.width() / 2 - ink.center().x();
        dShiftY = boxLogical.height() / 2 - ink.center().y();
    }
    else
    {
        // 크기는 맞으나 상자 밖으로 나가면 밀어 넣는다. 안에 있으면 기준선을 유지한다.
        if (ink.left() < 0)
            dShiftX = -ink.left();
        else if (ink.right() > boxLogical.width())
            dShiftX = boxLogical.width() - ink.right();
        if (ink.top() < 0)
            dShiftY = -ink.top();
        else if (ink.bottom() > boxLogical.height())
            dShiftY = boxLogical.height() - ink.bottom();
    }

    QImage canvas(boxDevice, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    canvas.setDevicePixelRatio(dRatio);
    {
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setPen(Qt::black);
        const QPointF pivot = ink.center();
        painter.translate(pivot + QPointF(dShiftX, dShiftY));
        painter.scale(dScale, dScale);
        painter.translate(-pivot);
        layout.draw(&painter, QPointF(0, dBaselineShift));
    }
    canvas.setDevicePixelRatio(1.0);

    // 투명 여백 자르기 + 컬러 판정
    bool bColor = false;
    const QRect crop = FindInk(canvas, &bColor);
    if (crop.isNull())
        return result;

    const QImage cropped = canvas.copy(crop);
    result.bColor = bColor;
    result.offset = crop.topLeft();
    if (bColor)
    {
        result.image = cropped.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    }
    else
    {
        QImage alpha(cropped.size(), QImage::Format_Alpha8);
        for (int iY = 0; iY < cropped.height(); ++iY)
        {
            const QRgb* pSrc = reinterpret_cast<const QRgb*>(cropped.constScanLine(iY));
            uchar* pDst = alpha.scanLine(iY);
            for (int iX = 0; iX < cropped.width(); ++iX)
                pDst[iX] = uchar(qAlpha(pSrc[iX]));
        }
        result.image = alpha;
    }
    return result;
}
