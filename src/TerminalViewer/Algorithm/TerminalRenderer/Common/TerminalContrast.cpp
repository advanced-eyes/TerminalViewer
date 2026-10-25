#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalContrast.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/BoxGlyphPainter.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace
{
// WCAG 2.x 상대 휘도: sRGB 채널을 선형으로 펴고 가중합
constexpr double kRedWeight = 0.2126;
constexpr double kGreenWeight = 0.7152;
constexpr double kBlueWeight = 0.0722;
constexpr double kLinearThreshold = 0.04045;   // 이 아래는 직선 구간
constexpr double kLinearDivisor = 12.92;
constexpr double kGammaOffset = 0.055;
constexpr double kGammaScale = 1.055;
constexpr double kGamma = 2.4;
constexpr double kFlare = 0.05;                 // 대비비의 (L + 0.05)
constexpr double kOffRatio = 1.0;
constexpr double kMaxRatio = 21.0;              // 흰색 / 검정
constexpr int kChannelMax = 255;
constexpr int kChannelCount = 256;
// 흰색 · 검정과 섞는 단계 수. 섞는 비율 step / kMixSteps. 255 면 한 단계에 채널이 많아야 1 바뀐다(8비트에서 가장 고운 단계)
constexpr int kMixSteps = 255;
// 그림 문자 범위(Ghostty 의 noMinContrast 와 같다). 상자 · 블록 U+2500-U+259F 는 BoxGlyphPainter::Handles 로 본다
constexpr char32_t kPowerlineFirst = 0xE0B0;    // Powerline 구분선 U+E0B0-U+E0BF + Powerline Extra 모양 U+E0C0-U+E0D7
constexpr char32_t kPowerlineLast = 0xE0D7;
constexpr char32_t kLegacyComputingFirst = 0x1FB00;             // Symbols for Legacy Computing(섹스턴트 등)
constexpr char32_t kLegacyComputingLast = 0x1FBFF;
constexpr char32_t kLegacyComputingSupplementFirst = 0x1CC00;   // Symbols for Legacy Computing Supplement
constexpr char32_t kLegacyComputingSupplementLast = 0x1CEBF;
// 캐시 표: 2^13 칸(칸 16 B, 128 KiB). 짝의 키(48비트)에 곱셈 해시를 해 위 13비트로 칸을 고른다
constexpr int kCacheSlotBits = 13;
constexpr size_t kCacheSlots = size_t(1) << kCacheSlotBits;
constexpr int kCacheSlotShift = 64 - kCacheSlotBits;   // 64비트 해시의 위 kCacheSlotBits 비트만 남긴다
constexpr uint64_t kHashMultiplier = 0x9E3779B97F4A7C15ull;
constexpr uint64_t kEmptyKey = ~uint64_t(0);    // 짝의 키는 48비트라 나올 수 없는 값
constexpr TerminalRgb kWhite{255, 255, 255};
constexpr TerminalRgb kBlack{0, 0, 0};

// 채널 값(0..255) -> 선형 값. 처음 쓸 때 한 번 만든다(정적 지역 변수 초기화는 스레드 안전)
const std::array<double, kChannelCount>& GetLinearTable()
{
    static const std::array<double, kChannelCount> s_arrTable = []()
    {
        std::array<double, kChannelCount> arrTable{};
        for (int iValue = 0; iValue < kChannelCount; ++iValue)
        {
            const double dValue = double(iValue) / kChannelMax;
            arrTable[size_t(iValue)] =
                dValue <= kLinearThreshold ? dValue / kLinearDivisor : std::pow((dValue + kGammaOffset) / kGammaScale, kGamma);
        }
        return arrTable;
    }();
    return s_arrTable;
}

// 두 상대 휘도의 대비비. GetContrastRatio 와 TryMix 의 판정이 같은 식을 쓴다
double ComputeContrastRatio(double a, double b)
{
    return (std::max(a, b) + kFlare) / (std::min(a, b) + kFlare);
}

// fg 를 흰색(lighten) 또는 검정 쪽으로 step / kMixSteps 만큼 섞는다. 흰색과 섞으면 (255 - c) 가, 검정과 섞으면 c 가 같은 비율로
// 줄어 채널 사이의 비(색조)가 남는다. step 에 대해 채널마다 단조이고 한 단계에 많아야 1 바뀐다
TerminalRgb Mix(const TerminalRgb& fg, bool lighten, int step)
{
    const int iKeep = kMixSteps - step;
    const auto funcChannel = [lighten, iKeep](uint8_t channel)
    {
        if (lighten)
            return uint8_t(kChannelMax - ((kChannelMax - channel) * iKeep + kMixSteps / 2) / kMixSteps);
        return uint8_t((channel * iKeep + kMixSteps / 2) / kMixSteps);
    };
    return TerminalRgb{funcChannel(fg.r), funcChannel(fg.g), funcChannel(fg.b)};
}

// 한쪽으로 섞어 목표에 닿는 가장 작은 섞음을 찾는다. 닿지 못하면 거짓.
// 닿았는지(그쪽에 있고 대비가 목표 이상)는 step 에 대해 단조(거짓..거짓 참..참)라 이분 탐색한다
bool TryMix(const TerminalRgb& fg, double bgLuminance, double ratio, bool lighten, TerminalRgb* pResult)
{
    const auto funcReaches = [&](int step)
    {
        const double dLuminance = GetRelativeLuminance(Mix(fg, lighten, step));
        if (lighten ? dLuminance < bgLuminance : dLuminance > bgLuminance)
            return false;
        return ComputeContrastRatio(dLuminance, bgLuminance) >= ratio;
    };
    if (!funcReaches(kMixSteps))
        return false;
    int iLow = 0;   // fg 자신: 대비가 모자라 닿지 않았다
    int iHigh = kMixSteps;
    while (iHigh - iLow > 1)
    {
        const int iMid = (iLow + iHigh) / 2;
        if (funcReaches(iMid))
            iHigh = iMid;
        else
            iLow = iMid;
    }
    *pResult = Mix(fg, lighten, iHigh);
    return true;
}

uint64_t MakePairKey(const TerminalRgb& fg, const TerminalRgb& bg)
{
    return (uint64_t(fg.r) << 40) | (uint64_t(fg.g) << 32) | (uint64_t(fg.b) << 24) | (uint64_t(bg.r) << 16) |
           (uint64_t(bg.g) << 8) | uint64_t(bg.b);
}
}   // namespace

double GetRelativeLuminance(const TerminalRgb& rgb)
{
    const std::array<double, kChannelCount>& arrLinear = GetLinearTable();
    return kRedWeight * arrLinear[rgb.r] + kGreenWeight * arrLinear[rgb.g] + kBlueWeight * arrLinear[rgb.b];
}

double GetContrastRatio(const TerminalRgb& a, const TerminalRgb& b)
{
    return ComputeContrastRatio(GetRelativeLuminance(a), GetRelativeLuminance(b));
}

TerminalRgb EnsureContrast(const TerminalRgb& fg, const TerminalRgb& bg, double minRatio)
{
    if (!(minRatio > kOffRatio))   // 1 이하 · NaN: 끔
        return fg;
    const double dRatio = std::min(minRatio, kMaxRatio);
    const double dFg = GetRelativeLuminance(fg);
    const double dBg = GetRelativeLuminance(bg);
    if (ComputeContrastRatio(dFg, dBg) >= dRatio)
        return fg;
    const double dWhiteRatio = ComputeContrastRatio(GetRelativeLuminance(kWhite), dBg);
    const double dBlackRatio = ComputeContrastRatio(GetRelativeLuminance(kBlack), dBg);
    // 배경 반대쪽이 먼저(밝은 글자는 더 밝게, 어두운 글자는 더 어둡게). 휘도가 같으면 여유가 큰 쪽
    const bool bLightenFirst = dFg != dBg ? dFg > dBg : dWhiteRatio >= dBlackRatio;
    TerminalRgb result;
    if (TryMix(fg, dBg, dRatio, bLightenFirst, &result) || TryMix(fg, dBg, dRatio, !bLightenFirst, &result))
        return result;
    return dWhiteRatio >= dBlackRatio ? kWhite : kBlack;
}

bool IsContrastExempt(char32_t codepoint)
{
    return BoxGlyphPainter::Handles(codepoint) || (codepoint >= kPowerlineFirst && codepoint <= kPowerlineLast) ||
           (codepoint >= kLegacyComputingFirst && codepoint <= kLegacyComputingLast) ||
           (codepoint >= kLegacyComputingSupplementFirst && codepoint <= kLegacyComputingSupplementLast);
}

TerminalRgb ContrastCache::Apply(const TerminalRgb& fg, const TerminalRgb& bg)
{
    if (!m_bEnabled)
        return fg;
    const uint64_t llKey = MakePairKey(fg, bg);
    if (m_bHasLast && llKey == m_llLastKey)   // 한 줄의 글자는 대개 같은 짝이 이어진다
        return m_lastResult;
    Slot& slot = m_vecSlots[size_t((llKey * kHashMultiplier) >> kCacheSlotShift)];
    if (slot.llKey != llKey)
    {
        slot.llKey = llKey;
        slot.result = EnsureContrast(fg, bg, m_dRatio);
    }
    const TerminalRgb result = slot.result;
    m_bHasLast = true;
    m_llLastKey = llKey;
    m_lastResult = result;
    return result;
}

void ContrastCache::SetRatio(double minRatio)
{
    const bool bEnabled = minRatio > kOffRatio;   // NaN 이면 거짓
    const double dRatio = bEnabled ? std::min(minRatio, kMaxRatio) : kOffRatio;
    if (bEnabled == m_bEnabled && dRatio == m_dRatio)
        return;
    m_bEnabled = bEnabled;
    m_dRatio = dRatio;
    m_bHasLast = false;
    if (m_bEnabled)
        m_vecSlots.assign(kCacheSlots, Slot{kEmptyKey, TerminalRgb()});
    else
        std::vector<Slot>().swap(m_vecSlots);   // 끄면 메모리를 돌려준다
}

bool ContrastCache::IsEnabled() const
{
    return m_bEnabled;
}
