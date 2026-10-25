#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphRasterizer.h"

#include <QFontInfo>
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

// 빈 이름과 이미 있는 이름(대소문자 무시)은 붙이지 않는다
void AppendUnique(QStringList& list, const QString& name)
{
    if (!name.isEmpty() && !list.contains(name, Qt::CaseInsensitive))
        list << name;
}

// Qt 5.15 가 QFont::family() 를 읽는 것처럼 쉼표로 나누고 앞뒤 공백과 감싼 따옴표를 벗긴다
QStringList SplitFamilyNames(const QString& family)
{
    QStringList listNames;
    for (const QString& strPart : family.split(QLatin1Char(',')))
    {
        QString strName = strPart.trimmed();
        const bool bDoubleQuoted = strName.startsWith(QLatin1Char('"')) && strName.endsWith(QLatin1Char('"'));
        const bool bSingleQuoted = strName.startsWith(QLatin1Char('\'')) && strName.endsWith(QLatin1Char('\''));
        if (strName.size() >= 2 && (bDoubleQuoted || bSingleQuoted))
            strName = strName.mid(1, strName.size() - 2);
        AppendUnique(listNames, strName);
    }
    return listNames;
}

// baseFont 에 대체 목록(GlyphRasterizer::BuildFontFamilies)을 붙인 글꼴. 목록은 붙이기 전의 baseFont 로 만든다
QFont AttachFontFamilies(const QFont& baseFont)
{
    QFont font = baseFont;
    font.setFamilies(GlyphRasterizer::BuildFontFamilies(baseFont));
    return font;
}

// baseFont 는 AttachFontFamilies 로 대체 목록을 이미 붙인 글꼴
QFont BuildFont(const QFont& baseFont, bool bold, bool italic)
{
    QFont font = baseFont;
    font.setStyleStrategy(QFont::NoSubpixelAntialias);
    font.setBold(bold);
    font.setItalic(italic);
    return font;
}

// 검은 펜을 따른 잉크인가. 투명 바탕에 검은 펜으로 그린 단색 글리프는 premultiplied rgb 가 모두 0 이다(알파만 남는다).
// rgb 가 0 이 아니면 글리프가 펜 색 대신 제 색으로 그린 픽셀이다(회색 · 흰색 포함).
bool IsBlackInk(QRgb pixel)
{
    return qRed(pixel) == 0 && qGreen(pixel) == 0 && qBlue(pixel) == 0;
}

// 알파가 있는 픽셀의 경계(장치 픽셀). 없으면 null. outColor 가 있으면 검은 펜을 따르지 않은 잉크(컬러) 여부를 쓴다.
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
            bColor = bColor || !IsBlackInk(pRow[iX]);
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
    : m_baseFont(AttachFontFamilies(baseFont))
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

    // 투명 여백 자르기 + 컬러 판정(IsColorGlyph 와 같은 규칙. 검은 펜으로 그렸으므로 다시 그리지 않고 한 번에 판정한다)
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

QStringList GlyphRasterizer::BuildFontFamilies(const QFont& baseFont)
{
    // Qt 와 같은 순서(families() 다음 family())로 시작해 칸 크기를 잰 글꼴(CellMetrics::Compute)과 같은 글꼴을 고르게 한다
    QStringList listFamilies;
    for (const QString& strName : baseFont.families())
        AppendUnique(listFamilies, strName);
    for (const QString& strName : SplitFamilyNames(baseFont.family()))
        AppendUnique(listFamilies, strName);
    // 위 이름이 하나도 없으면 Qt 가 대신 고른 글꼴. 이것이 없으면 한글 대체 글꼴이 라틴 글자를 그린다
    AppendUnique(listFamilies, QFontInfo(baseFont).family());
    AppendUnique(listFamilies, QString::fromLatin1(HANGUL_FAMILY));
    AppendUnique(listFamilies, QString::fromLatin1(EMOJI_FAMILY));
    return listFamilies;
}

bool GlyphRasterizer::IsColorGlyph(const QImage& drawnWithBlackPen)
{
    if (drawnWithBlackPen.isNull())
        return false;
    bool bColor = false;
    FindInk(drawnWithBlackPen.convertToFormat(QImage::Format_ARGB32_Premultiplied), &bColor);
    return bColor;
}
