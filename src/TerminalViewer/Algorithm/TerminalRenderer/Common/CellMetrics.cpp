#include "TerminalViewer/Algorithm/TerminalRenderer/Common/CellMetrics.h"

#include <QFontMetricsF>

#include <algorithm>
#include <cmath>

CellMetrics CellMetrics::Compute(const QFont& font, qreal devicePixelRatio)
{
    const QFontMetricsF fm(font);
    const qreal dRatio = devicePixelRatio;
    CellMetrics metrics;
    metrics.dDevicePixelRatio = dRatio;
    metrics.iCellWidth = int(std::ceil(fm.horizontalAdvance(QLatin1Char('M')) * dRatio));
    metrics.iCellHeight = int(std::ceil((fm.ascent() + fm.descent()) * dRatio));
    metrics.iBaseline = int(std::ceil(fm.ascent() * dRatio));
    metrics.iUnderlineThickness = std::max(1, int(std::lround(fm.lineWidth() * dRatio)));
    metrics.iUnderlineY = std::min(metrics.iBaseline + std::max(1, int(std::lround(fm.underlinePos() * dRatio))),
                                   metrics.iCellHeight - metrics.iUnderlineThickness);
    metrics.iStrikeThickness = metrics.iUnderlineThickness;
    const int iStrikeY = metrics.iBaseline - int(std::lround(fm.strikeOutPos() * dRatio)) - metrics.iStrikeThickness / 2;
    metrics.iStrikeY = std::max(0, std::min(iStrikeY, metrics.iCellHeight - metrics.iStrikeThickness));
    metrics.iLineThickness = std::max(1, int(std::lround(dRatio)));
    return metrics;
}

QSize CellMetrics::ComputeGrid(const QSize& devicePixelSize, int paddingDevicePx) const
{
    if (iCellWidth <= 0 || iCellHeight <= 0)
    {
        return QSize(1, 1);
    }
    const int iUsableWidth = devicePixelSize.width() - 2 * paddingDevicePx;
    const int iUsableHeight = devicePixelSize.height() - 2 * paddingDevicePx;
    return QSize(std::max(1, iUsableWidth / iCellWidth), std::max(1, iUsableHeight / iCellHeight));
}
