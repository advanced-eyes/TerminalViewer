#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalContrast.h"

#include <QString>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
constexpr TerminalRgb kBlack{0, 0, 0};
constexpr TerminalRgb kWhite{255, 255, 255};
constexpr double kEpsilon = 1e-9;
// 8비트 한 단계(채널마다 1)로 바뀌는 대비의 위 한도보다 넉넉한 값: 고친 색은 목표를 넘되 이만큼 넘게 지나치지 않는다
constexpr double kQuantizationSlack = 0.25;
constexpr double kHueToleranceDegrees = 3.0;
constexpr int kMinChromaForHue = 40;   // 채널 최대 - 최소가 이보다 작으면 반올림으로 색조가 흔들려 비교하지 않는다
constexpr double kRatios[] = {1.5, 3.0, 4.5, 7.0, 21.0};

std::vector<TerminalRgb> MakeSampleColors()
{
    std::vector<TerminalRgb> vecColors;
    for (int iGray = 0; iGray <= 255; iGray += 17)
        vecColors.push_back(TerminalRgb{uint8_t(iGray), uint8_t(iGray), uint8_t(iGray)});
    const TerminalRgb arrOthers[] = {{255, 0, 0},    {0, 255, 0},     {0, 0, 255},     {0, 0, 238},    {205, 0, 0},
                                     {0, 205, 0},    {205, 205, 0},   {205, 0, 205},   {0, 205, 205},  {92, 92, 255},
                                     {30, 30, 160},  {120, 20, 20},   {22, 24, 30},    {60, 64, 72},   {230, 220, 60},
                                     {250, 250, 250}, {110, 110, 110}, {90, 90, 90},   {255, 204, 0},  {50, 90, 160},
                                     {200, 120, 40}, {10, 20, 30},    {118, 118, 118}, {1, 2, 3},      {254, 253, 252}};
    for (const TerminalRgb& rgb : arrOthers)
        vecColors.push_back(rgb);
    return vecColors;
}

// HSV 색조(도). 무채색이면 -1
double GetHue(const TerminalRgb& rgb)
{
    const int iMax = std::max({int(rgb.r), int(rgb.g), int(rgb.b)});
    const int iMin = std::min({int(rgb.r), int(rgb.g), int(rgb.b)});
    if (iMax == iMin)
        return -1.0;
    const double dChroma = iMax - iMin;
    double dHue = 0.0;
    if (iMax == rgb.r)
        dHue = std::fmod((rgb.g - rgb.b) / dChroma, 6.0);
    else if (iMax == rgb.g)
        dHue = (rgb.b - rgb.r) / dChroma + 2.0;
    else
        dHue = (rgb.r - rgb.g) / dChroma + 4.0;
    dHue *= 60.0;
    return dHue < 0.0 ? dHue + 360.0 : dHue;
}

int GetChroma(const TerminalRgb& rgb)
{
    return std::max({int(rgb.r), int(rgb.g), int(rgb.b)}) - std::min({int(rgb.r), int(rgb.g), int(rgb.b)});
}

double GetHueDistance(double a, double b)
{
    const double dDiff = std::fabs(a - b);
    return std::min(dDiff, 360.0 - dDiff);
}

QString Describe(const TerminalRgb& fg, const TerminalRgb& bg, double ratio, const TerminalRgb& result)
{
    return QStringLiteral("fg (%1,%2,%3) bg (%4,%5,%6) ratio %7 -> (%8,%9,%10) contrast %11")
        .arg(fg.r).arg(fg.g).arg(fg.b).arg(bg.r).arg(bg.g).arg(bg.b).arg(ratio)
        .arg(result.r).arg(result.g).arg(result.b).arg(GetContrastRatio(result, bg));
}

bool IsReachable(const TerminalRgb& bg, double ratio)
{
    return std::max(GetContrastRatio(kWhite, bg), GetContrastRatio(kBlack, bg)) >= ratio;
}
}   // namespace

class TerminalContrastTest : public QObject
{
    Q_OBJECT

private slots:
    void luminanceKnownValues()
    {
        QCOMPARE(GetRelativeLuminance(kBlack), 0.0);
        QVERIFY(std::fabs(GetRelativeLuminance(kWhite) - 1.0) < kEpsilon);
        QVERIFY(std::fabs(GetRelativeLuminance(TerminalRgb{255, 0, 0}) - 0.2126) < kEpsilon);
        QVERIFY(std::fabs(GetRelativeLuminance(TerminalRgb{0, 255, 0}) - 0.7152) < kEpsilon);
        QVERIFY(std::fabs(GetRelativeLuminance(TerminalRgb{0, 0, 255}) - 0.0722) < kEpsilon);
        // sRGB -> 선형: 128 은 거듭제곱 구간, 10 은 직선 구간(10/255/12.92)
        QVERIFY(std::fabs(GetRelativeLuminance(TerminalRgb{128, 128, 128}) - 0.2158605) < 1e-6);
        QVERIFY(std::fabs(GetRelativeLuminance(TerminalRgb{10, 10, 10}) - 10.0 / 255.0 / 12.92) < 1e-9);
        // 채널마다 단조 증가
        for (int iValue = 1; iValue < 256; ++iValue)
        {
            const uint8_t uPrev = uint8_t(iValue - 1);
            const uint8_t uCur = uint8_t(iValue);
            QVERIFY(GetRelativeLuminance(TerminalRgb{uCur, 0, 0}) > GetRelativeLuminance(TerminalRgb{uPrev, 0, 0}));
            QVERIFY(GetRelativeLuminance(TerminalRgb{0, uCur, 0}) > GetRelativeLuminance(TerminalRgb{0, uPrev, 0}));
            QVERIFY(GetRelativeLuminance(TerminalRgb{0, 0, uCur}) > GetRelativeLuminance(TerminalRgb{0, 0, uPrev}));
        }
    }

    void contrastRatioKnownValues()
    {
        QVERIFY(std::fabs(GetContrastRatio(kWhite, kBlack) - 21.0) < kEpsilon);
        QVERIFY(std::fabs(GetContrastRatio(kBlack, kWhite) - 21.0) < kEpsilon);
        for (const TerminalRgb& rgb : MakeSampleColors())
            QCOMPARE(GetContrastRatio(rgb, rgb), 1.0);
        // WCAG 에서 흔히 드는 값: 흰 배경 위 #767676 은 4.54(AA 통과), #777777 은 4.48(실패)
        QVERIFY(std::fabs(GetContrastRatio(TerminalRgb{118, 118, 118}, kWhite) - 4.54) < 0.01);
        QVERIFY(GetContrastRatio(TerminalRgb{118, 118, 118}, kWhite) >= 4.5);
        QVERIFY(GetContrastRatio(TerminalRgb{119, 119, 119}, kWhite) < 4.5);
        QVERIFY(std::fabs(GetContrastRatio(TerminalRgb{255, 0, 0}, kBlack) - 5.252) < 1e-9);
        for (const TerminalRgb& a : MakeSampleColors())
        {
            for (const TerminalRgb& b : MakeSampleColors())
            {
                const double dRatio = GetContrastRatio(a, b);
                QCOMPARE(dRatio, GetContrastRatio(b, a));
                QVERIFY(dRatio >= 1.0 && dRatio <= 21.0 + kEpsilon);
            }
        }
    }

    void offLeavesEveryColor()
    {
        const double arrOff[] = {1.0, 0.5, 0.0, -3.0, std::numeric_limits<double>::quiet_NaN()};
        for (const double dRatio : arrOff)
        {
            for (const TerminalRgb& fg : MakeSampleColors())
            {
                for (const TerminalRgb& bg : MakeSampleColors())
                    QVERIFY(EnsureContrast(fg, bg, dRatio) == fg);
            }
        }
    }

    void enoughContrastIsKept()
    {
        for (const double dRatio : kRatios)
        {
            for (const TerminalRgb& fg : MakeSampleColors())
            {
                for (const TerminalRgb& bg : MakeSampleColors())
                {
                    if (GetContrastRatio(fg, bg) >= dRatio)
                        QVERIFY2(EnsureContrast(fg, bg, dRatio) == fg, qPrintable(Describe(fg, bg, dRatio, EnsureContrast(fg, bg, dRatio))));
                }
            }
        }
    }

    // 닿을 수 있으면 목표 이상이고 많이 지나치지 않는다(가장 작게 고친다). 어느 쪽으로도 닿지 못하면 대비가 더 큰 끝(흰색 · 검정)
    void lowContrastReachesTheTarget()
    {
        int iAdjusted = 0;
        int iUnreachable = 0;
        for (const double dRatio : kRatios)
        {
            for (const TerminalRgb& fg : MakeSampleColors())
            {
                for (const TerminalRgb& bg : MakeSampleColors())
                {
                    if (GetContrastRatio(fg, bg) >= dRatio)
                        continue;
                    const TerminalRgb result = EnsureContrast(fg, bg, dRatio);
                    const QString strCase = Describe(fg, bg, dRatio, result);
                    if (IsReachable(bg, dRatio))
                    {
                        ++iAdjusted;
                        QVERIFY2(GetContrastRatio(result, bg) >= dRatio, qPrintable(strCase));
                        QVERIFY2(GetContrastRatio(result, bg) < dRatio + kQuantizationSlack, qPrintable(strCase));
                    }
                    else
                    {
                        ++iUnreachable;
                        const TerminalRgb best = GetContrastRatio(kWhite, bg) >= GetContrastRatio(kBlack, bg) ? kWhite : kBlack;
                        QVERIFY2(result == best, qPrintable(strCase));
                    }
                }
            }
        }
        QVERIFY(iAdjusted > 100);
        QVERIFY(iUnreachable > 10);   // 7 · 21 은 가운데 밝기 배경에서 닿지 못한다
    }

    // 먼저 배경 반대쪽(글자가 배경보다 밝으면 더 밝게, 어두우면 더 어둡게)으로 옮기고, 닿지 못하면 다른 쪽
    void movesAwayFromTheBackgroundFirst()
    {
        // 검정 위 어두운 파랑: 밝게. 파랑에 흰색을 섞으므로 빨강 = 초록, 파랑이 가장 크다
        const TerminalRgb blue = EnsureContrast(TerminalRgb{0, 0, 238}, kBlack, 4.5);
        QVERIFY(GetRelativeLuminance(blue) > GetRelativeLuminance(TerminalRgb{0, 0, 238}));
        QVERIFY(blue.r == blue.g && blue.b > blue.r && blue.b >= 238);
        QVERIFY(blue != kWhite);

        // 흰 배경 위 주황: 어둡게. 채널마다 줄어든다
        const TerminalRgb orange{200, 120, 40};
        const TerminalRgb darker = EnsureContrast(orange, kWhite, 4.5);
        QVERIFY(GetRelativeLuminance(darker) < GetRelativeLuminance(orange));
        QVERIFY(darker.r <= orange.r && darker.g <= orange.g && darker.b <= orange.b);
        QVERIFY(darker != kBlack);

        // 배경(40)보다 어두운 글자(10): 어둡게는 닿지 못해(검정도 1.42) 밝게 뒤집는다
        const TerminalRgb bg40{40, 40, 40};
        QVERIFY(GetContrastRatio(kBlack, bg40) < 4.5);
        const TerminalRgb flipped = EnsureContrast(TerminalRgb{10, 10, 10}, bg40, 4.5);
        QVERIFY(GetRelativeLuminance(flipped) > GetRelativeLuminance(bg40));
        QVERIFY(GetContrastRatio(flipped, bg40) >= 4.5);

        // 배경과 같은 색: 어두운 배경이면 밝게, 밝은 배경이면 어둡게
        const TerminalRgb dark{22, 24, 30};
        QVERIFY(GetRelativeLuminance(EnsureContrast(dark, dark, 4.5)) > GetRelativeLuminance(dark));
        const TerminalRgb light{240, 240, 240};
        QVERIFY(GetRelativeLuminance(EnsureContrast(light, light, 4.5)) < GetRelativeLuminance(light));
    }

    void hueIsKept()
    {
        int iChecked = 0;
        for (const double dRatio : {3.0, 4.5, 7.0})
        {
            for (const TerminalRgb& fg : MakeSampleColors())
            {
                for (const TerminalRgb& bg : MakeSampleColors())
                {
                    const TerminalRgb result = EnsureContrast(fg, bg, dRatio);
                    if (result == fg || GetChroma(fg) < kMinChromaForHue || GetChroma(result) < kMinChromaForHue)
                        continue;
                    ++iChecked;
                    QVERIFY2(GetHueDistance(GetHue(fg), GetHue(result)) <= kHueToleranceDegrees,
                             qPrintable(Describe(fg, bg, dRatio, result)));
                }
            }
        }
        QVERIFY(iChecked > 20);
    }

    void sameInputSameOutput()
    {
        for (const double dRatio : kRatios)
        {
            for (const TerminalRgb& fg : MakeSampleColors())
            {
                for (const TerminalRgb& bg : MakeSampleColors())
                    QVERIFY(EnsureContrast(fg, bg, dRatio) == EnsureContrast(fg, bg, dRatio));
            }
        }
        // 21 보다 크면 21 로 본다
        QVERIFY(EnsureContrast(TerminalRgb{90, 90, 90}, TerminalRgb{100, 100, 100}, 30.0) ==
                EnsureContrast(TerminalRgb{90, 90, 90}, TerminalRgb{100, 100, 100}, 21.0));
    }

    // 상자 · 블록 문자(U+2500-U+259F, BoxGlyphPainter 가 그린다), Powerline 모양(U+E0B0-U+E0D7), Symbols for Legacy Computing
    // (U+1FB00-U+1FBFF, U+1CC00-U+1CEBF)은 글자가 아니라 그림이다: 글자색 · 배경색 짝이 뜻이 있어(반칸 ▀ 화소, Powerline 구분선)
    // 고치지 않는다(Ghostty 의 noMinContrast 와 같은 범위)
    void exemptCodepoints()
    {
        for (const char32_t codepoint : {char32_t(0x2500), char32_t(0x2502), char32_t(0x256D), char32_t(0x2580), char32_t(0x2588),
                                         char32_t(0x2591), char32_t(0x259F), char32_t(0xE0B0), char32_t(0xE0B6), char32_t(0xE0D7),
                                         char32_t(0x1FB00), char32_t(0x1FBFF), char32_t(0x1CC00), char32_t(0x1CEBF)})
            QVERIFY2(IsContrastExempt(codepoint), qPrintable(QString::number(uint32_t(codepoint), 16)));
        for (const char32_t codepoint : {char32_t('A'), char32_t(' '), char32_t(0x24FF), char32_t(0x25A0), char32_t(0x2603), char32_t(0xE0A0),
                                         char32_t(0xE0AF), char32_t(0xE0D8), char32_t(0xD55C), char32_t(0x1F600), char32_t(0x1FAFF),
                                         char32_t(0x1FC00), char32_t(0x1CBFF), char32_t(0x1CEC0), char32_t(0)})
            QVERIFY2(!IsContrastExempt(codepoint), qPrintable(QString::number(uint32_t(codepoint), 16)));
    }

    void cacheMatchesDirectComputation()
    {
        ContrastCache cache;
        QVERIFY(!cache.IsEnabled());
        const std::vector<TerminalRgb> vecColors = MakeSampleColors();
        for (const TerminalRgb& fg : vecColors)
            QVERIFY(cache.Apply(fg, TerminalRgb{40, 40, 40}) == fg);   // 끔(기본 1)

        for (const double dRatio : {4.5, 7.0, 4.5})
        {
            cache.SetRatio(dRatio);
            QVERIFY(cache.IsEnabled());
            // 같은 짝이 이어지거나(앞 짝 기억) 다시 나오는(해시) 순서
            for (int iPass = 0; iPass < 2; ++iPass)
            {
                for (size_t uFg = 0; uFg < vecColors.size(); ++uFg)
                {
                    for (size_t uBg = 0; uBg < vecColors.size(); ++uBg)
                    {
                        const TerminalRgb& fg = vecColors[iPass == 0 ? uFg : vecColors.size() - 1 - uFg];
                        const TerminalRgb& bg = vecColors[uBg];
                        QVERIFY(cache.Apply(fg, bg) == EnsureContrast(fg, bg, dRatio));
                        QVERIFY(cache.Apply(fg, bg) == EnsureContrast(fg, bg, dRatio));
                    }
                }
            }
        }
        cache.SetRatio(1.0);
        QVERIFY(!cache.IsEnabled());
        QVERIFY(cache.Apply(TerminalRgb{40, 40, 40}, TerminalRgb{40, 40, 40}) == (TerminalRgb{40, 40, 40}));
    }
};

QObject* CreateTerminalContrastTest() { return new TerminalContrastTest(); }

#include "TerminalContrastTest.moc"
