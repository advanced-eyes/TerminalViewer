#include "TerminalViewer/Algorithm/TerminalRenderer/Common/BoxGlyphPainter.h"

#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{
constexpr char32_t FIRST_BOX = 0x2500;
constexpr char32_t LAST_BLOCK = 0x259F;
constexpr char32_t FIRST_BLOCK = 0x2580;
constexpr int EIGHTHS = 8;

// 팔 무게: 0 없음, 1 가는, 2 굵은, 3 이중
struct Arms
{
    std::uint8_t iUp;
    std::uint8_t iDown;
    std::uint8_t iLeft;
    std::uint8_t iRight;
};

constexpr Arms NONE_ARMS{0, 0, 0, 0};

// U+2500 부터 순서대로. 점선·둥근·대각선 칸은 NONE_ARMS 으로 두고 따로 그린다.
constexpr std::array<Arms, 128> ARMS_TABLE = {{
    {0, 0, 1, 1}, {0, 0, 2, 2}, {1, 1, 0, 0}, {2, 2, 0, 0},                 // 2500-2503
    NONE_ARMS, NONE_ARMS, NONE_ARMS, NONE_ARMS, NONE_ARMS, NONE_ARMS, NONE_ARMS, NONE_ARMS,                   // 2504-250B 점선
    {0, 1, 0, 1}, {0, 1, 0, 2}, {0, 2, 0, 1}, {0, 2, 0, 2},                 // 250C-250F
    {0, 1, 1, 0}, {0, 1, 2, 0}, {0, 2, 1, 0}, {0, 2, 2, 0},                 // 2510-2513
    {1, 0, 0, 1}, {1, 0, 0, 2}, {2, 0, 0, 1}, {2, 0, 0, 2},                 // 2514-2517
    {1, 0, 1, 0}, {1, 0, 2, 0}, {2, 0, 1, 0}, {2, 0, 2, 0},                 // 2518-251B
    {1, 1, 0, 1}, {1, 1, 0, 2}, {2, 1, 0, 1}, {1, 2, 0, 1},                 // 251C-251F
    {2, 2, 0, 1}, {2, 1, 0, 2}, {1, 2, 0, 2}, {2, 2, 0, 2},                 // 2520-2523
    {1, 1, 1, 0}, {1, 1, 2, 0}, {2, 1, 1, 0}, {1, 2, 1, 0},                 // 2524-2527
    {2, 2, 1, 0}, {2, 1, 2, 0}, {1, 2, 2, 0}, {2, 2, 2, 0},                 // 2528-252B
    {0, 1, 1, 1}, {0, 1, 2, 1}, {0, 1, 1, 2}, {0, 1, 2, 2},                 // 252C-252F
    {0, 2, 1, 1}, {0, 2, 2, 1}, {0, 2, 1, 2}, {0, 2, 2, 2},                 // 2530-2533
    {1, 0, 1, 1}, {1, 0, 2, 1}, {1, 0, 1, 2}, {1, 0, 2, 2},                 // 2534-2537
    {2, 0, 1, 1}, {2, 0, 2, 1}, {2, 0, 1, 2}, {2, 0, 2, 2},                 // 2538-253B
    {1, 1, 1, 1}, {1, 1, 2, 1}, {1, 1, 1, 2}, {1, 1, 2, 2},                 // 253C-253F
    {2, 1, 1, 1}, {1, 2, 1, 1}, {2, 2, 1, 1}, {2, 1, 2, 1},                 // 2540-2543
    {2, 1, 1, 2}, {1, 2, 2, 1}, {1, 2, 1, 2}, {2, 1, 2, 2},                 // 2544-2547
    {1, 2, 2, 2}, {2, 2, 2, 1}, {2, 2, 1, 2}, {2, 2, 2, 2},                 // 2548-254B
    NONE_ARMS, NONE_ARMS, NONE_ARMS, NONE_ARMS,                                               // 254C-254F 점선
    {0, 0, 3, 3}, {3, 3, 0, 0}, {0, 1, 0, 3}, {0, 3, 0, 1},                 // 2550-2553
    {0, 3, 0, 3}, {0, 1, 3, 0}, {0, 3, 1, 0}, {0, 3, 3, 0},                 // 2554-2557
    {1, 0, 0, 3}, {3, 0, 0, 1}, {3, 0, 0, 3}, {1, 0, 3, 0},                 // 2558-255B
    {3, 0, 1, 0}, {3, 0, 3, 0}, {1, 1, 0, 3}, {3, 3, 0, 1},                 // 255C-255F
    {3, 3, 0, 3}, {1, 1, 3, 0}, {3, 3, 1, 0}, {3, 3, 3, 0},                 // 2560-2563
    {0, 1, 3, 3}, {0, 3, 1, 1}, {0, 3, 3, 3}, {1, 0, 3, 3},                 // 2564-2567
    {3, 0, 1, 1}, {3, 0, 3, 3}, {1, 1, 3, 3}, {3, 3, 1, 1},                 // 2568-256B
    {3, 3, 3, 3},                                                             // 256C
    NONE_ARMS, NONE_ARMS, NONE_ARMS, NONE_ARMS,                                               // 256D-2570 둥근
    NONE_ARMS, NONE_ARMS, NONE_ARMS,                                                      // 2571-2573 대각선
    {0, 0, 1, 0}, {1, 0, 0, 0}, {0, 0, 0, 1}, {0, 1, 0, 0},                 // 2574-2577
    {0, 0, 2, 0}, {2, 0, 0, 0}, {0, 0, 0, 2}, {0, 2, 0, 0},                 // 2578-257B
    {0, 0, 1, 2}, {1, 2, 0, 0}, {0, 0, 2, 1}, {2, 1, 0, 0},                 // 257C-257F
}};

QImage MakeImage(int width, int height)
{
    QImage image(std::max(1, width), std::max(1, height), QImage::Format_Alpha8);
    image.fill(0);
    return image;
}

// 반열린 구간 [x0,x1) x [y0,y1) 을 value 로 채운다. 범위 밖은 자른다.
void FillRect(QImage& image, int x0, int y0, int x1, int y1, std::uint8_t value = 255)
{
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(image.width(), x1);
    y1 = std::min(image.height(), y1);
    for (int iY = y0; iY < y1; ++iY)
    {
        uchar* pRow = image.scanLine(iY);
        for (int iX = x0; iX < x1; ++iX)
        {
            pRow[iX] = value;
        }
    }
}

// 투명 ARGB 캔버스에 그린 뒤 알파만 Alpha8 로 옮긴다.
QImage MakeCanvas(int width, int height)
{
    QImage canvas(std::max(1, width), std::max(1, height), QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    return canvas;
}

QImage ExtractAlpha(const QImage& canvas)
{
    QImage out = MakeImage(canvas.width(), canvas.height());
    for (int iY = 0; iY < canvas.height(); ++iY)
    {
        const QRgb* pSrc = reinterpret_cast<const QRgb*>(canvas.constScanLine(iY));
        uchar* pDst = out.scanLine(iY);
        for (int iX = 0; iX < canvas.width(); ++iX)
        {
            pDst[iX] = uchar(qAlpha(pSrc[iX]));
        }
    }
    return out;
}

int GetThickness(int weight, int lineThickness)
{
    return weight * lineThickness;  // 가는 t, 굵은 2t, 이중(선 2 + 틈 1) 3t
}

// 가운데 상자: 세로 팔 중 가장 두꺼운 것이 폭, 가로 팔 중 가장 두꺼운 것이 높이.
// 팔은 상자 먼 변까지 들어가 모서리를 채운다. 이중선은 먼저 통째로 그린 뒤 가운데 틈을 파내고,
// 가는·굵은 팔은 마지막에 그려 틈을 가로지른다.
void DrawArms(QImage& image, const Arms& arms, int lineThickness)
{
    const int iW = image.width();
    const int iH = image.height();
    const int iT = lineThickness;
    const int iCx = iW / 2;
    const int iCy = iH / 2;

    const int iThickV = std::max(GetThickness(arms.iUp, iT), GetThickness(arms.iDown, iT));
    const int iThickH = std::max(GetThickness(arms.iLeft, iT), GetThickness(arms.iRight, iT));
    const int iBoxW = iThickV > 0 ? iThickV : iThickH;
    const int iBoxH = iThickH > 0 ? iThickH : iThickV;
    const int iBx0 = iCx - iBoxW / 2;
    const int iBx1 = iBx0 + iBoxW;
    const int iBy0 = iCy - iBoxH / 2;
    const int iBy1 = iBy0 + iBoxH;

    const bool bDoubleH = arms.iLeft == 3 || arms.iRight == 3;
    const bool bDoubleV = arms.iUp == 3 || arms.iDown == 3;
    const bool bBothV = arms.iUp != 0 && arms.iDown != 0;
    const bool bBothH = arms.iLeft != 0 && arms.iRight != 0;

    // 이중선이 아닌 팔은, 수직 방향 팔이 양쪽에 있고 그중 이중선이 있으며 반대쪽 팔이 없으면
    // 상자 가까운 변에서 멈춘다(이중선 틈을 가로지르지 않게).
    auto funcDrawLeft = [&](int weight) {
        const int iThick = GetThickness(weight, iT);
        const int iY0 = iCy - iThick / 2;
        const bool bNear = weight != 3 && bBothV && bDoubleV && arms.iRight == 0;
        FillRect(image, 0, iY0, bNear ? iBx0 : iBx1, iY0 + iThick);
    };
    auto funcDrawRight = [&](int weight) {
        const int iThick = GetThickness(weight, iT);
        const int iY0 = iCy - iThick / 2;
        const bool bNear = weight != 3 && bBothV && bDoubleV && arms.iLeft == 0;
        FillRect(image, bNear ? iBx1 : iBx0, iY0, iW, iY0 + iThick);
    };
    auto funcDrawUp = [&](int weight) {
        const int iThick = GetThickness(weight, iT);
        const int iX0 = iCx - iThick / 2;
        const bool bNear = weight != 3 && bBothH && bDoubleH && arms.iDown == 0;
        FillRect(image, iX0, 0, iX0 + iThick, bNear ? iBy0 : iBy1);
    };
    auto funcDrawDown = [&](int weight) {
        const int iThick = GetThickness(weight, iT);
        const int iX0 = iCx - iThick / 2;
        const bool bNear = weight != 3 && bBothH && bDoubleH && arms.iUp == 0;
        FillRect(image, iX0, bNear ? iBy1 : iBy0, iX0 + iThick, iH);
    };

    // 1) 이중선 팔
    if (arms.iLeft == 3) funcDrawLeft(3);
    if (arms.iRight == 3) funcDrawRight(3);
    if (arms.iUp == 3) funcDrawUp(3);
    if (arms.iDown == 3) funcDrawDown(3);

    // 2) 이중선 가운데 틈 파내기
    if (bDoubleH)
    {
        const int iGy0 = iCy - (3 * iT) / 2 + iT;
        int iXs = 0;
        int iXe = iW;
        if (!(arms.iLeft == 3 && arms.iRight == 3))
        {
            if (arms.iRight == 3) iXs = bDoubleV ? iBx0 + iT : iBx1;
            else iXe = bDoubleV ? iBx0 + 2 * iT : iBx0;
        }
        FillRect(image, iXs, iGy0, iXe, iGy0 + iT, 0);
    }
    if (bDoubleV)
    {
        const int iGx0 = iCx - (3 * iT) / 2 + iT;
        int iYs = 0;
        int iYe = iH;
        if (!(arms.iUp == 3 && arms.iDown == 3))
        {
            if (arms.iDown == 3) iYs = bDoubleH ? iBy0 + iT : iBy1;
            else iYe = bDoubleH ? iBy0 + 2 * iT : iBy0;
        }
        FillRect(image, iGx0, iYs, iGx0 + iT, iYe, 0);
    }

    // 3) 가는·굵은 팔은 틈 위에 다시 그린다
    if (arms.iLeft == 1 || arms.iLeft == 2) funcDrawLeft(arms.iLeft);
    if (arms.iRight == 1 || arms.iRight == 2) funcDrawRight(arms.iRight);
    if (arms.iUp == 1 || arms.iUp == 2) funcDrawUp(arms.iUp);
    if (arms.iDown == 1 || arms.iDown == 2) funcDrawDown(arms.iDown);
}

// total 길이를 count 조각으로 나눠 조각마다 양쪽에 gap/2 를 비운다.
// 셀 경계에서 이웃 셀과 이어져도 틈 크기가 같다.
std::vector<std::pair<int, int>> MakeDashRuns(int total, int count, double gap)
{
    std::vector<std::pair<int, int>> runs;
    const double dPeriod = double(total) / count;
    for (int iIndex = 0; iIndex < count; ++iIndex)
    {
        const int iStart = int(std::lround(iIndex * dPeriod + gap / 2.0));
        int iEnd = int(std::lround((iIndex + 1) * dPeriod - gap / 2.0));
        if (iEnd <= iStart) iEnd = iStart + 1;
        runs.emplace_back(iStart, iEnd);
    }
    return runs;
}

void DrawDashed(QImage& image, char32_t codepoint, int lineThickness)
{
    // 2504-250B: 3·4 조각, 254C-254F: 2 조각. 순서는 (가로 가는, 가로 굵은, 세로 가는, 세로 굵은).
    int iCount = 2;
    bool bHorizontal = true;
    bool bHeavy = false;
    if (codepoint >= 0x2504 && codepoint <= 0x250B)
    {
        const int iIndex = int(codepoint - 0x2504);  // 0..7
        iCount = iIndex < 4 ? 3 : 4;
        bHorizontal = (iIndex % 4) < 2;
        bHeavy = (iIndex % 2) == 1;
    }
    else
    {
        const int iIndex = int(codepoint - 0x254C);  // 0..3
        bHorizontal = iIndex < 2;
        bHeavy = (iIndex % 2) == 1;
    }
    const int iThick = (bHeavy ? 2 : 1) * lineThickness;
    const int iLength = bHorizontal ? image.width() : image.height();
    const int iSpan = bHorizontal ? image.height() : image.width();
    const double dPeriod = double(iLength) / iCount;
    const double dGap = std::max(1.0, std::round(dPeriod / 4.0));
    const int iOffset = iSpan / 2 - iThick / 2;
    for (const auto& run : MakeDashRuns(iLength, iCount, dGap))
    {
        if (bHorizontal) FillRect(image, run.first, iOffset, run.second, iOffset + iThick);
        else FillRect(image, iOffset, run.first, iOffset + iThick, run.second);
    }
}

QImage DrawArc(char32_t codepoint, int width, int height, int lineThickness)
{
    // 선 중심선이 지나는 좌표(정수 경계에 맞춘 선의 한가운데)
    const double dCx = (width / 2 - lineThickness / 2) + lineThickness / 2.0;
    const double dCy = (height / 2 - lineThickness / 2) + lineThickness / 2.0;
    const bool bDown = codepoint == 0x256D || codepoint == 0x256E;
    const bool bRight = codepoint == 0x256D || codepoint == 0x2570;
    const double dSx = bRight ? 1.0 : -1.0;
    const double dSy = bDown ? 1.0 : -1.0;
    // 반지름은 가운데에서 가까운 변까지 거리 중 작은 쪽의 절반. 나머지는 직선으로 이어 변에 직각으로 닿는다.
    constexpr double RADIUS_RATIO = 0.5;
    const double dRadius = RADIUS_RATIO * std::min(bRight ? width - dCx : dCx, bDown ? height - dCy : dCy);
    const QPointF arcCenter(dCx + dSx * dRadius, dCy + dSy * dRadius);
    constexpr double TAIL = 2.0;  // 셀 밖으로 조금 더 그려 끝이 변에서 잘리게 한다

    QPainterPath path;
    path.moveTo(dCx, bDown ? height + TAIL : -TAIL);
    path.lineTo(dCx, arcCenter.y());
    constexpr int ARC_STEPS = 48;
    constexpr double HALF_PI = 1.57079632679489661923;
    for (int iIndex = 1; iIndex <= ARC_STEPS; ++iIndex)
    {
        const double dAngle = HALF_PI * iIndex / ARC_STEPS;
        path.lineTo(arcCenter.x() - dSx * dRadius * std::cos(dAngle), arcCenter.y() - dSy * dRadius * std::sin(dAngle));
    }
    path.lineTo(bRight ? width + TAIL : -TAIL, dCy);

    QImage canvas = MakeCanvas(width, height);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(Qt::white, lineThickness, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
    painter.drawPath(path);
    painter.end();
    return ExtractAlpha(canvas);
}

QImage DrawDiagonal(char32_t codepoint, int width, int height, int lineThickness)
{
    QImage canvas = MakeCanvas(width, height);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(Qt::white, lineThickness, Qt::SolidLine, Qt::FlatCap));
    const double dLen = std::hypot(double(width), double(height));
    const double dEx = lineThickness * width / dLen;   // 모서리까지 칠해지도록 양끝을 늘인다
    const double dEy = lineThickness * height / dLen;
    if (codepoint == 0x2571 || codepoint == 0x2573)
    {
        painter.drawLine(QPointF(width + dEx, -dEy), QPointF(-dEx, height + dEy));
    }
    if (codepoint == 0x2572 || codepoint == 0x2573)
    {
        painter.drawLine(QPointF(-dEx, -dEy), QPointF(width + dEx, height + dEy));
    }
    painter.end();
    return ExtractAlpha(canvas);
}

int CalcEighths(int size, int count)
{
    return int(std::lround(double(size) * count / EIGHTHS));
}

// 2x2 무늬: 1 이면 한 칸, 2 이면 바둑판, 3 이면 한 칸만 비운다.
// 폭·높이가 홀수일 때 남는 마지막 열·행은 무늬를 성기게/촘촘하게 보정해 덮는 비율을 맞춘다.
void DrawShade(QImage& image, int coverageQuarters)
{
    const int iW = image.width();
    const int iH = image.height();
    for (int iY = 0; iY < iH; ++iY)
    {
        uchar* pRow = image.scanLine(iY);
        for (int iX = 0; iX < iW; ++iX)
        {
            const int iPhase = (iX & 1) + 2 * (iY & 1);
            bool bOn = false;
            switch (coverageQuarters)
            {
            case 1: bOn = iPhase == 0; break;
            case 2: bOn = iPhase == 0 || iPhase == 3; break;
            default: bOn = iPhase != 3; break;
            }
            const bool bLeftoverCol = (iW & 1) && iX == iW - 1;
            const bool bLeftoverRow = (iH & 1) && iY == iH - 1;
            if (coverageQuarters == 1)
            {
                if (bLeftoverCol) bOn = bOn && ((iY / 2) % 2 == 0);
                else if (bLeftoverRow) bOn = bOn && ((iX / 2) % 2 == 0);
            }
            else if (coverageQuarters == 3)
            {
                if (bLeftoverCol) bOn = (iY % 4) != 3;
                else if (bLeftoverRow) bOn = (iX % 4) != 3;
            }
            pRow[iX] = bOn ? 255 : 0;
        }
    }
}

QImage DrawBlock(char32_t codepoint, int width, int height)
{
    QImage image = MakeImage(width, height);
    const int iHalfW = CalcEighths(width, 4);
    const int iHalfH = CalcEighths(height, 4);
    enum EQuadrant { UPPER_LEFT = 1, UPPER_RIGHT = 2, LOWER_LEFT = 4, LOWER_RIGHT = 8 };

    if (codepoint == 0x2580)  // UPPER HALF BLOCK
    {
        FillRect(image, 0, 0, width, iHalfH);
    }
    else if (codepoint >= 0x2581 && codepoint <= 0x2588)  // LOWER ONE EIGHTH .. FULL BLOCK
    {
        FillRect(image, 0, height - CalcEighths(height, int(codepoint - 0x2580)), width, height);
    }
    else if (codepoint >= 0x2589 && codepoint <= 0x258F)  // LEFT SEVEN EIGHTHS .. LEFT ONE EIGHTH
    {
        FillRect(image, 0, 0, CalcEighths(width, int(0x2590 - codepoint)), height);
    }
    else if (codepoint == 0x2590)  // RIGHT HALF BLOCK
    {
        FillRect(image, iHalfW, 0, width, height);
    }
    else if (codepoint >= 0x2591 && codepoint <= 0x2593)  // LIGHT / MEDIUM / DARK SHADE
    {
        DrawShade(image, int(codepoint - 0x2590));
    }
    else if (codepoint == 0x2594)  // UPPER ONE EIGHTH BLOCK
    {
        FillRect(image, 0, 0, width, CalcEighths(height, 1));
    }
    else if (codepoint == 0x2595)  // RIGHT ONE EIGHTH BLOCK
    {
        FillRect(image, width - CalcEighths(width, 1), 0, width, height);
    }
    else
    {
        int iQuadrants = 0;
        switch (codepoint)
        {
        case 0x2596: iQuadrants = LOWER_LEFT; break;
        case 0x2597: iQuadrants = LOWER_RIGHT; break;
        case 0x2598: iQuadrants = UPPER_LEFT; break;
        case 0x2599: iQuadrants = UPPER_LEFT | LOWER_LEFT | LOWER_RIGHT; break;
        case 0x259A: iQuadrants = UPPER_LEFT | LOWER_RIGHT; break;
        case 0x259B: iQuadrants = UPPER_LEFT | UPPER_RIGHT | LOWER_LEFT; break;
        case 0x259C: iQuadrants = UPPER_LEFT | UPPER_RIGHT | LOWER_RIGHT; break;
        case 0x259D: iQuadrants = UPPER_RIGHT; break;
        case 0x259E: iQuadrants = UPPER_RIGHT | LOWER_LEFT; break;
        case 0x259F: iQuadrants = UPPER_RIGHT | LOWER_LEFT | LOWER_RIGHT; break;
        default: break;
        }
        if (iQuadrants & UPPER_LEFT) FillRect(image, 0, 0, iHalfW, iHalfH);
        if (iQuadrants & UPPER_RIGHT) FillRect(image, iHalfW, 0, width, iHalfH);
        if (iQuadrants & LOWER_LEFT) FillRect(image, 0, iHalfH, iHalfW, height);
        if (iQuadrants & LOWER_RIGHT) FillRect(image, iHalfW, iHalfH, width, height);
    }
    return image;
}
}  // namespace

namespace BoxGlyphPainter
{
bool Handles(char32_t codepoint)
{
    return codepoint >= FIRST_BOX && codepoint <= LAST_BLOCK;
}

QImage Paint(char32_t codepoint, const CellMetrics& metrics)
{
    if (!Handles(codepoint)) return QImage();
    const int iW = std::max(1, metrics.iCellWidth);
    const int iH = std::max(1, metrics.iCellHeight);
    const int iT = std::max(1, metrics.iLineThickness);

    if (codepoint >= FIRST_BLOCK) return DrawBlock(codepoint, iW, iH);
    if (codepoint >= 0x256D && codepoint <= 0x2570) return DrawArc(codepoint, iW, iH, iT);
    if (codepoint >= 0x2571 && codepoint <= 0x2573) return DrawDiagonal(codepoint, iW, iH, iT);

    QImage image = MakeImage(iW, iH);
    if ((codepoint >= 0x2504 && codepoint <= 0x250B) || (codepoint >= 0x254C && codepoint <= 0x254F))
    {
        DrawDashed(image, codepoint, iT);
        return image;
    }
    DrawArms(image, ARMS_TABLE[codepoint - FIRST_BOX], iT);
    return image;
}

int GetUnderlineSpriteTop(const CellMetrics& metrics)
{
    return std::max(0, metrics.iUnderlineY - metrics.iUnderlineThickness);
}

QImage PaintUnderline(EUnderlineSprite sprite, const CellMetrics& metrics)
{
    const int iW = std::max(1, metrics.iCellWidth);
    const int iT = std::max(1, metrics.iUnderlineThickness);
    const int iTop = GetUnderlineSpriteTop(metrics);
    const int iH = std::max(1, std::min(3 * iT, metrics.iCellHeight - iTop));

    if (sprite == EUnderlineSprite::CURLY)
    {
        // 한 칸에 물결 한 주기. 양끝 높이가 같아 이웃 칸과 이어진다.
        constexpr double TWO_PI = 6.28318530717958647692;
        constexpr double CURVE_STEP = 0.25;
        const double dMid = iH / 2.0;
        const double dAmplitude = std::max(0.0, (iH - iT) / 2.0);
        QPainterPath path;
        for (double dX = -1.0; dX <= iW + 1.0 + 1e-9; dX += CURVE_STEP)
        {
            const QPointF point(dX, dMid - dAmplitude * std::sin(TWO_PI * dX / iW));
            if (dX == -1.0) path.moveTo(point);
            else path.lineTo(point);
        }
        QImage canvas = MakeCanvas(iW, iH);
        QPainter painter(&canvas);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(Qt::white, iT, Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
        painter.drawPath(path);
        painter.end();
        return ExtractAlpha(canvas);
    }

    QImage image = MakeImage(iW, iH);
    const bool bDotted = sprite == EUnderlineSprite::DOTTED;
    const int iOnLength = (bDotted ? 1 : 3) * iT;
    const int iPeriodTarget = (bDotted ? 2 : 4) * iT;
    const int iCount = std::max(1, int(std::lround(double(iW) / iPeriodTarget)));
    const double dGap = std::max(1.0, double(iW) / iCount - iOnLength);
    const int iRow = metrics.iUnderlineY - iTop;
    for (const auto& run : MakeDashRuns(iW, iCount, dGap))
    {
        FillRect(image, run.first, iRow, run.second, iRow + iT);
    }
    return image;
}
}  // namespace BoxGlyphPainter
