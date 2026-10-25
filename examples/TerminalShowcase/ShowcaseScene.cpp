#include "ShowcaseScene.h"

#include <TerminalViewer/Testing/FakeFrameSource.h>

#include <QString>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace
{
// 칸 · 배치
constexpr int kNarrowCells = 1;
constexpr int kWideCells = 2;
constexpr int kLabelColumns = 11;                         // 라벨(최대 10칸) + 여백 1칸
constexpr int kLabelGap = 1;                              // 라벨 뒤 여백
constexpr int kLabelShareDivisor = 3;                     // 좁으면 라벨 칸을 열의 1/3 까지 줄인다
constexpr int kSegmentGap = 1;                            // 절 안 조각 사이 빈칸
constexpr int kTitleBorderWidth = 1;                      // 제목 틀 한쪽 칸
constexpr int kTitleMargin = 1;                           // 제목 틀 안 한쪽 여백
constexpr int kHalvesPerCell = 2;                         // 반칸 ▀: 위(글자색) · 아래(배경색) 두 색
constexpr int kBoxLines = 3;                              // 상자 묶음의 줄 수
constexpr int kPreeditCells = 2;                          // 조합 글자(2칸). 캐럿은 그 뒤
constexpr int kPromptReserveCells = 1 + kPreeditCells;    // 프롬프트 뒤에 남길 칸: 커서 + 조합 글자

// IME 흉내 단계(ShowcaseScene::kImeSteps): 그 아래는 조합 없음
constexpr int kImeJamoStep = 6;                           // "ㅎ"
constexpr int kImeSyllableFirstStep = 7;                  // 여기부터 "하"

// 색(배경은 ShowcaseScene::kBackground)
constexpr TerminalRgb kForeground{214, 218, 226};
constexpr TerminalRgb kLabelColor{118, 126, 142};
constexpr TerminalRgb kBorderColor{90, 150, 230};
constexpr TerminalRgb kTitleColor{255, 210, 90};
constexpr TerminalRgb kUserColor{90, 210, 120};
constexpr TerminalRgb kPathColor{100, 160, 255};
constexpr TerminalRgb kPreeditBackground{60, 66, 96};
constexpr TerminalRgb kDarkText{16, 16, 16};              // 밝은 배경 위 번호
constexpr TerminalRgb kLightText{240, 240, 240};          // 어두운 배경 위 번호
constexpr TerminalRgb kBarDoneColor{90, 200, 120};
constexpr TerminalRgb kBarRestColor{80, 86, 100};
constexpr TerminalRgb kClockColor{150, 200, 255};
constexpr TerminalRgb kLogInfoColor{110, 200, 120};
constexpr TerminalRgb kLogErrorColor{255, 95, 95};
constexpr TerminalRgb kLogWarnColor{240, 200, 80};
constexpr TerminalRgb kLogDebugColor{150, 150, 160};
constexpr TerminalRgb kLogSourceColor{110, 190, 230};

// xterm 기본 256색 팔레트: 0..15 기본 16색, 16..231 6x6x6 정육면체, 232..255 회색 24단계
constexpr int kAnsiColorCount = 16;
constexpr int kAnsiNormalCount = 8;                       // 0..7 보통, 8..15 밝은
constexpr int kCubeFirst = 16;
constexpr int kCubeSide = 6;
constexpr int kGrayFirst = 232;
constexpr int kGrayCount = 24;
constexpr int kGrayBase = 8;
constexpr int kGrayStep = 10;
constexpr int kPaletteLines = kCubeSide / kHalvesPerCell;   // 녹색 6단계를 반칸 두 단계씩
constexpr TerminalRgb kXtermAnsiColors[kAnsiColorCount] = {{0, 0, 0},       {205, 0, 0},   {0, 205, 0},   {205, 205, 0},
                                                           {0, 0, 238},     {205, 0, 205}, {0, 205, 205}, {229, 229, 229},
                                                           {127, 127, 127}, {255, 0, 0},   {0, 255, 0},   {255, 255, 0},
                                                           {92, 92, 255},   {255, 0, 255}, {0, 255, 255}, {255, 255, 255}};
constexpr uint8_t kCubeLevels[kCubeSide] = {0, 95, 135, 175, 215, 255};
constexpr const char32_t* kAnsiNames[kAnsiColorCount] = {U"blk", U"red", U"grn", U"yel", U"blu", U"mag", U"cyn", U"wht",
                                                         U"BLK", U"RED", U"GRN", U"YEL", U"BLU", U"MAG", U"CYN", U"WHT"};
constexpr int kAnsiNumberWidth = 3;                       // 배경색 번호 칸(오른쪽 정렬)
constexpr int kAnsiBackgroundGap = 0;                     // 번호 뒤 빈칸이 이미 사이를 띄운다

// 색상환 · 밝기
constexpr double kFullTurnDegrees = 360.0;
constexpr double kHueSectorDegrees = 60.0;                // 색상환의 6 구간
constexpr double kMaxChannel = 255.0;
constexpr int kLumaRedWeight = 299;                       // BT.601 밝기 가중치(x kLumaScale)
constexpr int kLumaGreenWeight = 587;
constexpr int kLumaBlueWeight = 114;
constexpr int kLumaScale = 1000;
constexpr int kLightLumaThreshold = 140;                  // 이 밝기(0..255)를 넘으면 밝은 색

struct UnderlineSample
{
    const char32_t* pName;
    EUnderline eUnderline;
    TerminalRgb color;
};

constexpr UnderlineSample kUnderlineSamples[] = {{U"single", EUnderline::SINGLE, {90, 170, 255}},
                                                 {U"double", EUnderline::DOUBLE, {120, 220, 120}},
                                                 {U"curly", EUnderline::CURLY, {255, 90, 90}},
                                                 {U"dotted", EUnderline::DOTTED, {240, 200, 80}},
                                                 {U"dashed", EUnderline::DASHED, {200, 120, 255}}};

// 대비가 낮은 글자(대비 3 미만). 최소 대비(M 키, --min-contrast)를 켜면 읽히게 고쳐진다.
// 어두운 배경 위는 밝게, 밝은 배경 위는 어둡게 고친다. 흐린 글자는 고친 뒤에도 흐리다
struct ContrastSample
{
    const char32_t* pText;
    TerminalRgb fg;
    std::optional<TerminalRgb> optBg;   // 없으면 터미널 기본 배경
    bool bFaint;
};

// 끝의 주석은 그 짝의 대비비
constexpr ContrastSample kContrastSamples[] = {{U"blue/black", {0, 0, 170}, TerminalRgb{0, 0, 0}, false},             // 1.6
                                              {U"dim gray", {62, 66, 78}, std::nullopt, false},                       // 1.8
                                              {U"gray/gray", {128, 128, 128}, TerminalRgb{96, 96, 96}, false},        // 1.6
                                              {U"red/maroon", {200, 40, 40}, TerminalRgb{120, 20, 20}, false},        // 2.0
                                              {U"yellow/white", {210, 200, 40}, TerminalRgb{245, 245, 240}, false},   // 1.6
                                              {U"faint gray", {90, 96, 110}, std::nullopt, true}};                    // 2.8(흐리게 전)

constexpr const char32_t* kEmoji[] = {U"\U0001F600", U"\U0001F680", U"\U0001F389", U"\U0001F40D", U"\U0001F363"};

// 회색 · 흰색뿐인 컬러 이모지(흰 하트 · 흰 깃발). 빨간 글자색으로 넣는다: 컬러로 판정하면 글자색을 무시하고 제 색(흰색)으로,
// 단색으로 잘못 판정하면 빨간 실루엣으로 보인다
struct GrayEmoji
{
    const char32_t* pText;
    bool bGrapheme;   // 여러 코드포인트 글자(vecGraphemes)로 넣는다
};

constexpr GrayEmoji kGrayEmoji[] = {{U"\U0001F90D", false},         // 흰 하트: 혼자도 그림 글자(2칸)
                                    {U"\U0001F3F3\uFE0F", true}};   // 흰 깃발: 혼자면 텍스트 표시(1칸)라 \uFE0F 를 붙인 2칸 글자
constexpr TerminalRgb kGrayEmojiTint{255, 80, 80};

// 여러 코드포인트로 된 글자(vecGraphemes). \u200D = ZWJ, \uFE0F = 그림 글자로 보이기
constexpr const char32_t* kEmojiClusters[] = {U"\U0001F468\u200D\U0001F469\u200D\U0001F467",   // 가족
                                              U"\U0001F469\u200D\U0001F4BB",                    // 개발자
                                              U"\U0001F9D1\u200D\U0001F680",                    // 우주인
                                              U"\U0001F3F3\uFE0F\u200D\U0001F308",              // 무지개 깃발
                                              U"\U0001F44D\U0001F3FD"};                         // 피부색

struct BoxSample
{
    const char32_t* arrLines[kBoxLines];
    TerminalRgb color;
};

constexpr BoxSample kBoxSamples[] = {{{U"┌──┬──┐", U"├──┼──┤", U"└──┴──┘"}, {130, 200, 255}},
                                     {{U"╔══╦══╗", U"╠══╬══╣", U"╚══╩══╝"}, {255, 200, 120}},
                                     {{U"┏━━┳━━┓", U"┣━━╋━━┫", U"┗━━┻━━┛"}, {200, 160, 255}},
                                     {{U"╭──╮", U"│  │", U"╰──╯"}, {140, 230, 170}},
                                     {{U"┌┄┄┐", U"┆  ┆", U"└┈┈┘"}, {255, 140, 160}},
                                     {{U"╒═╤═╕", U"╞═╪═╡", U"╘═╧═╛"}, {150, 220, 220}},
                                     {{U"╱╲", U"╲╱", U"╳╳"}, {230, 230, 120}}};

// 실시간 줄
constexpr char32_t kSpinnerFrames[] = {U'▘', U'▝', U'▗', U'▖'};
constexpr uint32_t kSpinnerHueSteps = 24;                 // 24 틱에 색상환 한 바퀴
constexpr double kSpinnerHueStepDegrees = 15.0;
constexpr int kSpinnerCells = 2;                          // 스피너 글자 + 빈칸
constexpr int kPercentMax = 100;
constexpr uint32_t kPercentSteps = uint32_t(kPercentMax) + 1;   // 0 .. 100 %
constexpr int kPercentWidth = 3;
constexpr int kBarShareDivisor = 3;                       // 진행 막대는 라벨 · 스피너 뒤 칸의 1/3
constexpr int kBarMinCells = 4;
constexpr int kBarMaxCells = 24;
constexpr int kEighthsPerCell = 8;
constexpr char32_t kLeftEighths[kEighthsPerCell] = {0, U'▏', U'▎', U'▍', U'▌', U'▋', U'▊', U'▉'};   // 칸의 n/8 을 채운 막대
constexpr const char32_t* kNoClockText = U"--:--:--.---";
constexpr int kSparkBars = 12;
constexpr double kSparkPhaseStep = 0.5;                   // 틱 · 막대 하나만큼의 sin 위상
constexpr int kSparkTopLevel = 7;                         // ▁(0) .. █(7)
constexpr char32_t kSparkFirstGlyph = 0x2581;             // ▁
constexpr double kSparkHueBase = 200.0;
constexpr double kSparkHueStep = 10.0;

// 로그 줄(스크롤백). 줄 번호로 정해지는 가짜 값이다
constexpr uint32_t kLogHashMultiplier = 2654435761u;
constexpr uint32_t kLogHashOffset = 0x9E3779B9u;
constexpr uint32_t kLogHashMixMultiplier = 2246822519u;
constexpr int kLogHashFirstShift = 15;
constexpr int kLogHashSecondShift = 13;
constexpr uint32_t kLogRandomARange = 1000;
constexpr int kLogRandomBShift = 10;
constexpr uint32_t kLogRandomBRange = 100;
constexpr int kMsPerSecond = 1000;
constexpr int kSecondsPerMinute = 60;
constexpr int kMinutesPerHour = 60;
constexpr int kMsPerMinute = kMsPerSecond * kSecondsPerMinute;
constexpr int kMsPerHour = kMsPerMinute * kMinutesPerHour;
constexpr int kClockFieldWidth = 2;                       // HH · mm · ss
constexpr int kClockMsWidth = 3;                          // zzz
constexpr int kDecimalBase = 10;
constexpr int kLogStartHour = 9;                          // 첫 줄은 09:00:00 쯤
constexpr int kLogMsPerLine = 137;
constexpr uint32_t kLogJitterMs = 97;
constexpr int kLogIndexWidth = 4;                         // [0000]
constexpr int kLogErrorPeriod = 47;                       // index % 47 == 13 이면 ERROR
constexpr int kLogErrorPhase = 13;
constexpr int kLogWarnPeriod = 23;                        // index % 23 == 7 이면 WARN
constexpr int kLogWarnPhase = 7;
constexpr int kLogDebugPeriod = 5;                        // index % 5 == 0 이면 DEBUG
constexpr uint32_t kLogInfoKinds = 4;
constexpr const char32_t* kLogSources[] = {U"net   ", U"db    ", U"render", U"ui    ", U"io    "};

// FakeFrameSource::SetText 와 같은 범위(한글 · CJK · 전각 · 이모지)를 2칸으로 본다
bool IsWide(char32_t codepoint)
{
    return (codepoint >= 0x1100 && codepoint <= 0x115F) || (codepoint >= 0x2E80 && codepoint <= 0xA4CF) ||
           (codepoint >= 0xAC00 && codepoint <= 0xD7A3) || (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
           (codepoint >= 0xFE30 && codepoint <= 0xFE6F) || (codepoint >= 0xFF00 && codepoint <= 0xFF60) ||
           (codepoint >= 0x1F300 && codepoint <= 0x1FAFF);
}

int MeasureChar(char32_t codepoint)
{
    return IsWide(codepoint) ? kWideCells : kNarrowCells;
}

int MeasureText(const std::u32string& text)
{
    int iWidth = 0;
    for (const char32_t codepoint : text)
        iWidth += MeasureChar(codepoint);
    return iWidth;
}

TerminalCell MakeStyle(const TerminalRgb& fg)
{
    TerminalCell cell;
    cell.fg = fg;
    cell.bg = ShowcaseScene::kBackground;
    cell.underlineColor = fg;
    return cell;
}

TerminalCell MakeStyle(const TerminalRgb& fg, const TerminalRgb& bg)
{
    TerminalCell cell = MakeStyle(fg);
    cell.bg = bg;
    cell.bBgDefault = false;
    return cell;
}

TerminalRgb GetXtermColor(int index)
{
    if (index < kAnsiColorCount)
        return kXtermAnsiColors[index];
    if (index < kGrayFirst)
    {
        const int iCube = index - kCubeFirst;
        return TerminalRgb{kCubeLevels[iCube / (kCubeSide * kCubeSide)], kCubeLevels[(iCube / kCubeSide) % kCubeSide], kCubeLevels[iCube % kCubeSide]};
    }
    const uint8_t uGray = uint8_t(kGrayBase + (index - kGrayFirst) * kGrayStep);
    return TerminalRgb{uGray, uGray, uGray};
}

// 6x6x6 정육면체 안의 색 번호(각 단계 0..5)
int GetCubeColorIndex(int red, int green, int blue)
{
    return kCubeFirst + kCubeSide * kCubeSide * red + kCubeSide * green + blue;
}

TerminalRgb MakeHueColor(double degrees)
{
    const double dH = std::fmod(std::fmod(degrees, kFullTurnDegrees) + kFullTurnDegrees, kFullTurnDegrees) / kHueSectorDegrees;
    const double dX = 1.0 - std::fabs(std::fmod(dH, 2.0) - 1.0);
    double dR = 0.0;
    double dG = 0.0;
    double dB = 0.0;
    switch (int(dH))
    {
    case 0:
        dR = 1.0;
        dG = dX;
        break;
    case 1:
        dR = dX;
        dG = 1.0;
        break;
    case 2:
        dG = 1.0;
        dB = dX;
        break;
    case 3:
        dG = dX;
        dB = 1.0;
        break;
    case 4:
        dR = dX;
        dB = 1.0;
        break;
    default:
        dR = 1.0;
        dB = dX;
        break;
    }
    return TerminalRgb{uint8_t(std::lround(dR * kMaxChannel)), uint8_t(std::lround(dG * kMaxChannel)), uint8_t(std::lround(dB * kMaxChannel))};
}

bool IsLight(const TerminalRgb& rgb)
{
    return rgb.r * kLumaRedWeight + rgb.g * kLumaGreenWeight + rgb.b * kLumaBlueWeight > kLightLumaThreshold * kLumaScale;
}

// 한 줄을 왼쪽부터 채운다. 열 수(또는 SetLimit 의 한도)를 넘는 글자는 쓰지 않는다
class LineBuilder
{
public:
    explicit LineBuilder(int cols)
        : m_iCols(std::max(0, cols))
        , m_iLimit(m_iCols)
        , m_line()
    {
    }

public:
    bool PutChar(char32_t codepoint, const TerminalCell& style)
    {
        const bool bWide = IsWide(codepoint);
        if (GetRoom() < (bWide ? kWideCells : kNarrowCells))
            return false;
        TerminalCell cell = style;
        cell.uCodepoint = uint32_t(codepoint);
        cell.iGrapheme = -1;
        cell.eWidth = bWide ? ECellWidth::WIDE : ECellWidth::NARROW;
        m_line.vecCells.push_back(cell);
        if (bWide)
            PushTail(style);
        return true;
    }

    // 여러 코드포인트로 된 글자 하나(ZWJ 이모지 등): 첫 코드포인트를 칸에, 전체를 vecGraphemes 에 넣는다
    bool PutGrapheme(const std::u32string& grapheme, const TerminalCell& style)
    {
        if (grapheme.empty())
            return true;
        const bool bWide = IsWide(grapheme.front());
        if (GetRoom() < (bWide ? kWideCells : kNarrowCells))
            return false;
        TerminalCell cell = style;
        cell.uCodepoint = uint32_t(grapheme.front());
        cell.iGrapheme = int(m_line.vecGraphemes.size());
        cell.eWidth = bWide ? ECellWidth::WIDE : ECellWidth::NARROW;
        m_line.vecGraphemes.push_back(grapheme);
        m_line.vecCells.push_back(cell);
        if (bWide)
            PushTail(style);
        return true;
    }

    // 들어가는 만큼 쓴다. 모두 썼으면 참
    bool PutText(const std::u32string& text, const TerminalCell& style)
    {
        for (const char32_t codepoint : text)
        {
            if (!PutChar(codepoint, style))
                return false;
        }
        return true;
    }

    void PadTo(int col)
    {
        const int iEnd = std::min(col, m_iLimit);
        while (GetWidth() < iEnd)
            m_line.vecCells.push_back(TerminalCell());
    }

    ShowcaseLine Take()
    {
        ShowcaseLine line = std::move(m_line);
        m_line = ShowcaseLine();
        m_iLimit = m_iCols;
        return line;
    }

public:
    int GetWidth() const { return int(m_line.vecCells.size()); }
    int GetRoom() const { return m_iLimit - GetWidth(); }
    void SetLimit(int col) { m_iLimit = std::clamp(col, 0, m_iCols); }

private:
    void PushTail(const TerminalCell& style)
    {
        TerminalCell tail = style;
        tail.uCodepoint = 0;
        tail.iGrapheme = -1;
        tail.eWidth = ECellWidth::SPACER_TAIL;
        m_line.vecCells.push_back(tail);
    }

    int m_iCols;
    int m_iLimit;
    ShowcaseLine m_line;
};

// 조각: 같은 꾸밈의 글자들, 또는 여러 코드포인트 글자 하나
struct Piece
{
    std::u32string strText;
    TerminalCell style;
    bool bGrapheme = false;
};

// 줄을 넘길 때 나뉘지 않는 단위
using Segment = std::vector<Piece>;

int MeasurePiece(const Piece& piece)
{
    if (!piece.bGrapheme)
        return MeasureText(piece.strText);
    return piece.strText.empty() ? 0 : MeasureChar(piece.strText.front());
}

int MeasureSegment(const Segment& segment)
{
    int iWidth = 0;
    for (const Piece& piece : segment)
        iWidth += MeasurePiece(piece);
    return iWidth;
}

// 조각을 차례로 쓴다. 한 조각이 다 들어가지 않으면 칸 경계에서 자르고 뒤 조각은 쓰지 않는다(잘린 자리에 좁은 글자가 끼지 않게)
void PutSegment(LineBuilder& builder, const Segment& segment)
{
    for (const Piece& piece : segment)
    {
        const bool bAll = piece.bGrapheme ? builder.PutGrapheme(piece.strText, piece.style) : builder.PutText(piece.strText, piece.style);
        if (!bAll)
            return;
    }
}

Segment MakeTextSegment(const std::u32string& text, const TerminalCell& style)
{
    return Segment{Piece{text, style, false}};
}

Segment MakeGraphemeSegment(const std::u32string& grapheme, const TerminalCell& style)
{
    return Segment{Piece{grapheme, style, true}};
}

// 라벨 칸: 좁으면 라벨 칸도 줄인다(열의 1/3 까지)
int GetLabelColumns(int cols)
{
    return std::min(kLabelColumns, cols / kLabelShareDivisor);
}

void PutLabel(LineBuilder& builder, const std::u32string& label, int labelColumns)
{
    if (labelColumns > kLabelGap)
        builder.PutText(label.substr(0, size_t(labelColumns - kLabelGap)), MakeStyle(kLabelColor));
    builder.PadTo(labelColumns);
}

// 라벨 뒤에 조각들을 차례로 놓는다. 넘치면 다음 줄(라벨 칸은 비운다)로 넘기고, 한 줄에도 넘치는 조각은 자른다
std::vector<ShowcaseLine> FlowSection(int cols, const std::u32string& label, const std::vector<Segment>& segments, int gap)
{
    const int iLabelColumns = GetLabelColumns(cols);
    std::vector<ShowcaseLine> vecLines;
    LineBuilder builder(cols);
    PutLabel(builder, label, iLabelColumns);
    bool bLineEmpty = true;
    for (const Segment& segment : segments)
    {
        const int iGap = bLineEmpty ? 0 : gap;
        if (!bLineEmpty && iGap + MeasureSegment(segment) > builder.GetRoom())
        {
            vecLines.push_back(builder.Take());
            builder.PadTo(iLabelColumns);
            bLineEmpty = true;
        }
        if (!bLineEmpty)
            builder.PadTo(builder.GetWidth() + gap);
        PutSegment(builder, segment);
        bLineEmpty = false;
    }
    vecLines.push_back(builder.Take());
    return vecLines;
}

std::vector<ShowcaseLine> MakeTitle(int cols)
{
    const std::u32string strMargin(size_t(kTitleMargin), U' ');
    const std::u32string strTitle = U"TerminalViewer showcase";
    const std::u32string strSeparator = U" · ";
    const std::u32string strHelp = U"+/- font  C cursor  M contrast  wheel/PgUp/PgDn scroll  Q quit";
    const int iTextWidth = kTitleMargin + MeasureText(strTitle) + MeasureText(strSeparator) + MeasureText(strHelp) + kTitleMargin;
    const int iInner = std::max(0, std::min(iTextWidth, cols - 2 * kTitleBorderWidth));
    const TerminalCell border = MakeStyle(kBorderColor);

    std::vector<ShowcaseLine> vecLines;
    LineBuilder builder(cols);
    const auto funcRule = [&](char32_t left, char32_t right)
    {
        builder.PutChar(left, border);
        for (int iCol = 0; iCol < iInner; ++iCol)
            builder.PutChar(U'─', border);
        builder.PutChar(right, border);
        vecLines.push_back(builder.Take());
    };
    funcRule(U'╭', U'╮');
    builder.PutChar(U'│', border);
    builder.SetLimit(kTitleBorderWidth + iInner);
    TerminalCell title = MakeStyle(kTitleColor);
    title.bBold = true;
    PutSegment(builder, Segment{Piece{strMargin, MakeStyle(kForeground), false}, Piece{strTitle, title, false},
                                Piece{strSeparator, MakeStyle(kLabelColor), false}, Piece{strHelp, MakeStyle(kForeground), false}});
    builder.PadTo(kTitleBorderWidth + iInner);
    builder.SetLimit(cols);
    builder.PutChar(U'│', border);
    vecLines.push_back(builder.Take());
    funcRule(U'╰', U'╯');
    return vecLines;
}

std::vector<ShowcaseLine> MakeAnsiForeground(int cols)
{
    std::vector<Segment> vecSegments;
    for (int iColor = 0; iColor < kAnsiColorCount; ++iColor)
        vecSegments.push_back(MakeTextSegment(kAnsiNames[iColor], MakeStyle(GetXtermColor(iColor))));
    return FlowSection(cols, U"ansi fg", vecSegments, kSegmentGap);
}

std::vector<ShowcaseLine> MakeAnsiBackground(int cols)
{
    std::vector<Segment> vecSegments;
    for (int iColor = 0; iColor < kAnsiColorCount; ++iColor)
    {
        const TerminalRgb bg = GetXtermColor(iColor);
        const TerminalRgb fg = IsLight(bg) ? kDarkText : kLightText;
        const std::u32string strNumber = QStringLiteral("%1").arg(iColor, kAnsiNumberWidth).toStdU32String() + U" ";
        vecSegments.push_back(MakeTextSegment(strNumber, MakeStyle(fg, bg)));
    }
    return FlowSection(cols, U"ansi bg", vecSegments, kAnsiBackgroundGap);
}

// 6x6x6 색 정육면체를 빨강 단계마다 6x6 네모로(반칸 ▀ 로 녹색 두 단계씩, 3줄), 옆에 기본 16색과 회색 24단계
std::vector<ShowcaseLine> MakePalette(int cols)
{
    const int iLabelColumns = GetLabelColumns(cols);
    std::vector<ShowcaseLine> vecLines;
    LineBuilder builder(cols);
    const auto funcHalf = [&](int top, int bottom)
    {
        const TerminalCell cell = MakeStyle(GetXtermColor(top), GetXtermColor(bottom));
        builder.PutChar(U'▀', cell);
    };
    for (int iLine = 0; iLine < kPaletteLines; ++iLine)
    {
        PutLabel(builder, iLine == 0 ? U"256 color" : U"", iLabelColumns);
        for (int iRed = 0; iRed < kCubeSide; ++iRed)
        {
            if (iRed > 0)
                builder.PutChar(U' ', MakeStyle(kForeground));
            for (int iBlue = 0; iBlue < kCubeSide; ++iBlue)
            {
                funcHalf(GetCubeColorIndex(iRed, kHalvesPerCell * iLine, iBlue), GetCubeColorIndex(iRed, kHalvesPerCell * iLine + 1, iBlue));
            }
        }
        builder.PutText(U"  ", MakeStyle(kForeground));
        if (iLine == 0)
        {
            for (int iColor = 0; iColor < kAnsiNormalCount; ++iColor)
                funcHalf(iColor, iColor + kAnsiNormalCount);
        }
        else if (iLine == 1)
        {
            for (int iGray = 0; iGray < kGrayCount / kHalvesPerCell; ++iGray)
                funcHalf(kGrayFirst + iGray, kGrayFirst + kGrayCount / kHalvesPerCell + iGray);
        }
        else
        {
            builder.PutText(U"16 + 6x6x6 + 24 gray", MakeStyle(kLabelColor));
        }
        vecLines.push_back(builder.Take());
    }
    return vecLines;
}

// 위 반칸: 색상환, 아래 반칸: 검정 -> 흰색
std::vector<ShowcaseLine> MakeTrueColor(int cols)
{
    const int iLabelColumns = GetLabelColumns(cols);
    LineBuilder builder(cols);
    PutLabel(builder, U"truecolor", iLabelColumns);
    const int iWidth = std::max(0, cols - iLabelColumns);
    for (int iCol = 0; iCol < iWidth; ++iCol)
    {
        const double dT = iWidth > 1 ? double(iCol) / double(iWidth - 1) : 0.0;
        const uint8_t uGray = uint8_t(std::lround(dT * kMaxChannel));
        builder.PutChar(U'▀', MakeStyle(MakeHueColor(dT * kFullTurnDegrees), TerminalRgb{uGray, uGray, uGray}));
    }
    return {builder.Take()};
}

std::vector<ShowcaseLine> MakeStyles(int cols)
{
    const TerminalCell plain = MakeStyle(kForeground);
    TerminalCell bold = plain;
    bold.bBold = true;
    TerminalCell italic = plain;
    italic.bItalic = true;
    TerminalCell boldItalic = bold;
    boldItalic.bItalic = true;
    TerminalCell faint = plain;
    faint.bFaint = true;
    TerminalCell inverse = plain;
    inverse.bInverse = true;
    TerminalCell hidden = plain;
    hidden.bInvisible = true;
    TerminalCell strike = plain;
    strike.bStrikethrough = true;
    const std::vector<Segment> vecSegments = {
        MakeTextSegment(U"normal", plain),           MakeTextSegment(U"bold", bold),   MakeTextSegment(U"italic", italic),
        MakeTextSegment(U"bold+italic", boldItalic), MakeTextSegment(U"faint", faint), MakeTextSegment(U"inverse", inverse),
        Segment{Piece{U"[", plain, false}, Piece{U"hidden", hidden, false}, Piece{U"]", plain, false}},
        MakeTextSegment(U"strike", strike)};
    return FlowSection(cols, U"styles", vecSegments, kSegmentGap);
}

std::vector<ShowcaseLine> MakeContrast(int cols)
{
    std::vector<Segment> vecSegments;
    for (const ContrastSample& sample : kContrastSamples)
    {
        TerminalCell style = sample.optBg ? MakeStyle(sample.fg, *sample.optBg) : MakeStyle(sample.fg);
        style.bFaint = sample.bFaint;
        vecSegments.push_back(MakeTextSegment(sample.pText, style));
    }
    return FlowSection(cols, U"contrast", vecSegments, kSegmentGap);
}

std::vector<ShowcaseLine> MakeUnderlines(int cols)
{
    std::vector<Segment> vecSegments;
    for (const UnderlineSample& sample : kUnderlineSamples)
    {
        TerminalCell style = MakeStyle(kForeground);
        style.eUnderline = sample.eUnderline;
        style.underlineColor = sample.color;
        vecSegments.push_back(MakeTextSegment(sample.pName, style));
    }
    return FlowSection(cols, U"underline", vecSegments, kSegmentGap);
}

std::vector<ShowcaseLine> MakeWide(int cols)
{
    const TerminalCell korean = MakeStyle(TerminalRgb{140, 220, 255});
    const TerminalCell japanese = MakeStyle(TerminalRgb{255, 190, 120});
    const TerminalCell chinese = MakeStyle(TerminalRgb{190, 255, 160});
    const std::vector<Segment> vecSegments = {
        MakeTextSegment(U"한국어", korean),     MakeTextSegment(U"안녕하세요", korean), MakeTextSegment(U"日本語", japanese),
        MakeTextSegment(U"ひらがな", japanese), MakeTextSegment(U"カタカナ", japanese), MakeTextSegment(U"中文", chinese),
        MakeTextSegment(U"汉字", chinese),      MakeTextSegment(U"漢字", chinese)};
    return FlowSection(cols, U"wide", vecSegments, kSegmentGap);
}

std::vector<ShowcaseLine> MakeEmoji(int cols)
{
    const TerminalCell plain = MakeStyle(kForeground);
    const TerminalCell tinted = MakeStyle(kGrayEmojiTint);
    std::vector<Segment> vecSegments;
    for (const char32_t* pEmoji : kEmoji)
        vecSegments.push_back(MakeTextSegment(pEmoji, plain));
    for (const GrayEmoji& gray : kGrayEmoji)
        vecSegments.push_back(gray.bGrapheme ? MakeGraphemeSegment(gray.pText, tinted) : MakeTextSegment(gray.pText, tinted));
    vecSegments.push_back(MakeTextSegment(U"zwj:", MakeStyle(kLabelColor)));
    for (const char32_t* pCluster : kEmojiClusters)
        vecSegments.push_back(MakeGraphemeSegment(pCluster, plain));
    return FlowSection(cols, U"emoji", vecSegments, kSegmentGap);
}

// 3줄짜리 상자 묶음을 옆으로 놓는다. 묶음 하나가 다 들어가지 않으면 거기서 멈춘다
std::vector<ShowcaseLine> MakeBox(int cols)
{
    const int iLabelColumns = GetLabelColumns(cols);
    std::vector<LineBuilder> vecBuilders(kBoxLines, LineBuilder(cols));
    for (int iLine = 0; iLine < kBoxLines; ++iLine)
        PutLabel(vecBuilders[size_t(iLine)], iLine == 0 ? U"box" : U"", iLabelColumns);
    bool bFirst = true;
    for (const BoxSample& sample : kBoxSamples)
    {
        const int iGap = bFirst ? 0 : kSegmentGap;
        if (iGap + MeasureText(sample.arrLines[0]) > vecBuilders[0].GetRoom())
            break;
        for (int iLine = 0; iLine < kBoxLines; ++iLine)
        {
            LineBuilder& builder = vecBuilders[size_t(iLine)];
            builder.PadTo(builder.GetWidth() + iGap);
            builder.PutText(sample.arrLines[iLine], MakeStyle(sample.color));
        }
        bFirst = false;
    }
    std::vector<ShowcaseLine> vecLines;
    for (LineBuilder& builder : vecBuilders)
        vecLines.push_back(builder.Take());
    return vecLines;
}

std::vector<ShowcaseLine> MakeBlocks(int cols)
{
    const std::vector<Segment> vecSegments = {MakeTextSegment(U"█▓▒░", MakeStyle(TerminalRgb{200, 200, 210})),
                                              MakeTextSegment(U"▀▄▌▐", MakeStyle(TerminalRgb{120, 200, 255})),
                                              MakeTextSegment(U"▖▗▘▝▚▞▙▛▜▟", MakeStyle(TerminalRgb{255, 180, 100})),
                                              MakeTextSegment(U"▁▂▃▄▅▆▇█", MakeStyle(TerminalRgb{140, 230, 140})),
                                              MakeTextSegment(U"▏▎▍▌▋▊▉█", MakeStyle(TerminalRgb{230, 140, 230})),
                                              MakeTextSegment(U"▔▕", MakeStyle(TerminalRgb{230, 230, 120}))};
    return FlowSection(cols, U"blocks", vecSegments, kSegmentGap);
}

std::vector<ShowcaseLine> MakeSelection(int cols, int* pBegin, int* pEnd)
{
    const std::u32string strBefore = U"The ";
    const std::u32string strSelected = U"quick brown fox";
    const std::u32string strAfter = U" jumps over the lazy dog";
    const int iLabelColumns = GetLabelColumns(cols);
    LineBuilder builder(cols);
    PutLabel(builder, U"selection", iLabelColumns);
    const TerminalCell plain = MakeStyle(kForeground);
    builder.PutText(strBefore, plain);
    const int iBegin = builder.GetWidth();
    builder.PutText(strSelected, plain);
    const int iEnd = builder.GetWidth();
    builder.PutText(strAfter, plain);
    *pBegin = iBegin;
    *pEnd = iEnd;
    return {builder.Take()};
}

std::vector<ShowcaseLine> MakeLive(int cols, uint32_t tick, const std::u32string& clock)
{
    const int iLabelColumns = GetLabelColumns(cols);
    LineBuilder builder(cols);
    PutLabel(builder, U"live", iLabelColumns);
    // 들어가는 묶음만 놓는다(넘치면 그 묶음을 건너뛴다)
    const auto funcPut = [&builder](const Segment& segment)
    {
        if (MeasureSegment(segment) <= builder.GetRoom())
            PutSegment(builder, segment);
    };

    funcPut(MakeTextSegment(std::u32string(1, kSpinnerFrames[tick % std::size(kSpinnerFrames)]) + U" ",
                            MakeStyle(MakeHueColor(double(tick % kSpinnerHueSteps) * kSpinnerHueStepDegrees))));

    const int iPercent = int(tick % kPercentSteps);
    const int iBarCells = std::clamp((cols - iLabelColumns - kSpinnerCells) / kBarShareDivisor, kBarMinCells, kBarMaxCells);
    const int iEighths = iPercent * iBarCells * kEighthsPerCell / kPercentMax;
    std::u32string strDone(size_t(iEighths / kEighthsPerCell), U'█');
    if (iEighths % kEighthsPerCell != 0)
        strDone += kLeftEighths[iEighths % kEighthsPerCell];
    const std::u32string strRest(size_t(iBarCells - MeasureText(strDone)), U'░');
    funcPut(Segment{Piece{strDone, MakeStyle(kBarDoneColor), false}, Piece{strRest, MakeStyle(kBarRestColor), false}});
    funcPut(MakeTextSegment(QStringLiteral(" %1%").arg(iPercent, kPercentWidth).toStdU32String(), MakeStyle(kForeground)));

    funcPut(MakeTextSegment(U"  " + (clock.empty() ? std::u32string(kNoClockText) : clock), MakeStyle(kClockColor)));

    Segment vecSpark{Piece{U"  ", MakeStyle(kForeground), false}};
    for (int iBar = 0; iBar < kSparkBars; ++iBar)
    {
        const double dWave = std::sin((double(tick) + iBar) * kSparkPhaseStep) * 0.5 + 0.5;
        const int iLevel = std::clamp(int(std::lround(dWave * kSparkTopLevel)), 0, kSparkTopLevel);
        vecSpark.push_back(Piece{std::u32string(1, char32_t(kSparkFirstGlyph + iLevel)), MakeStyle(MakeHueColor(kSparkHueBase + iBar * kSparkHueStep)), false});
    }
    funcPut(vecSpark);
    funcPut(MakeTextSegment(QStringLiteral("  #%1").arg(tick).toStdU32String(), MakeStyle(kLabelColor)));
    return {builder.Take()};
}

// 커서를 놓을 자리와 조합 글자 2칸이 남는 가장 긴 프롬프트
std::vector<ShowcaseLine> MakePrompt(int cols, int* pWidth)
{
    TerminalCell user = MakeStyle(kUserColor);
    user.bBold = true;
    TerminalCell path = MakeStyle(kPathColor);
    path.bBold = true;
    const TerminalCell plain = MakeStyle(kForeground);
    const std::vector<Segment> vecVariants = {
        Segment{Piece{U"user@terminalviewer", user, false}, Piece{U":", plain, false}, Piece{U"~", path, false},
                Piece{U"$ ", plain, false}, Piece{U"echo 안녕", plain, false}},
        Segment{Piece{U"~", path, false}, Piece{U"$ ", plain, false}, Piece{U"echo 안녕", plain, false}},
        Segment{Piece{U"$ ", plain, false}}};
    const Segment* pChosen = &vecVariants.back();
    for (const Segment& variant : vecVariants)
    {
        if (MeasureSegment(variant) + kPromptReserveCells <= cols)
        {
            pChosen = &variant;
            break;
        }
    }
    LineBuilder builder(cols);
    PutSegment(builder, *pChosen);
    *pWidth = builder.GetWidth();
    return {builder.Take()};
}

// HH:mm:ss.zzz
QString FormatClock(int ms)
{
    const QLatin1Char cZero('0');
    return QStringLiteral("%1:%2:%3.%4")
        .arg(ms / kMsPerHour, kClockFieldWidth, kDecimalBase, cZero)
        .arg(ms / kMsPerMinute % kMinutesPerHour, kClockFieldWidth, kDecimalBase, cZero)
        .arg(ms / kMsPerSecond % kSecondsPerMinute, kClockFieldWidth, kDecimalBase, cZero)
        .arg(ms % kMsPerSecond, kClockMsWidth, kDecimalBase, cZero);
}

ShowcaseLine MakeLogLine(int cols, int index)
{
    uint32_t uHash = uint32_t(index) * kLogHashMultiplier + kLogHashOffset;
    uHash ^= uHash >> kLogHashFirstShift;
    uHash *= kLogHashMixMultiplier;
    uHash ^= uHash >> kLogHashSecondShift;
    const QString strTime = FormatClock(kLogStartHour * kMsPerHour + index * kLogMsPerLine + int(uHash % kLogJitterMs));
    // 메시지에 넣을 가짜 숫자
    const int iRandomA = int(uHash % kLogRandomARange);
    const int iRandomB = int((uHash >> kLogRandomBShift) % kLogRandomBRange);
    TerminalCell level = MakeStyle(kLogInfoColor);
    std::u32string strLevel = U"INFO ";
    QString strMessage;
    if (index % kLogErrorPeriod == kLogErrorPhase)
    {
        level = MakeStyle(kLogErrorColor);
        level.bBold = true;
        strLevel = U"ERROR";
        strMessage = QStringLiteral("checksum mismatch in chunk %1, retrying").arg(iRandomA);
    }
    else if (index % kLogWarnPeriod == kLogWarnPhase)
    {
        level = MakeStyle(kLogWarnColor);
        strLevel = U"WARN ";
        strMessage = QStringLiteral("slow response from upstream (%1 ms)").arg(200 + iRandomA);
    }
    else if (index % kLogDebugPeriod == 0)
    {
        level = MakeStyle(kLogDebugColor);
        level.bFaint = true;
        strLevel = U"DEBUG";
        strMessage = QStringLiteral("cache lookup key=%1 hit=%2")
                         .arg(uHash % 65536, 4, 16, QLatin1Char('0'))
                         .arg(QLatin1String(iRandomB % 2 ? "yes" : "no"));
    }
    else
    {
        switch (uHash % kLogInfoKinds)
        {
        case 0:
            strMessage = QStringLiteral("processed batch %1 (%2 items) in %3.%4 ms")
                             .arg(index)
                             .arg(iRandomA % 256)
                             .arg(iRandomB % 40)
                             .arg(iRandomA % 10);
            break;
        case 1:
            strMessage = QStringLiteral("GET /api/v1/items/%1 200 %2 B").arg(iRandomA).arg(iRandomA * 7 + iRandomB);
            break;
        case 2:
            strMessage = QStringLiteral("빌드 완료: 대상 %1개, %2.%3초").arg(iRandomB % 30 + 1).arg(iRandomA % 9).arg(iRandomB % 10);
            break;
        default:
            strMessage = QStringLiteral("frame %1 presented (%2 quads)").arg(index * 3).arg(iRandomA + 400);
            break;
        }
    }
    const TerminalCell dim = MakeStyle(kLabelColor);
    LineBuilder builder(cols);
    PutSegment(builder, Segment{Piece{QStringLiteral("[%1] ").arg(index, kLogIndexWidth, kDecimalBase, QLatin1Char('0')).toStdU32String(), dim, false},
                                Piece{strTime.toStdU32String() + U" ", dim, false},
                                Piece{strLevel, level, false},
                                Piece{U" ", dim, false},
                                Piece{kLogSources[uHash % std::size(kLogSources)], MakeStyle(kLogSourceColor), false},
                                Piece{U"  ", dim, false},
                                Piece{strMessage.toStdU32String(), MakeStyle(kForeground), false}});
    return builder.Take();
}

ShowcaseLine MakeCommandLine(int cols)
{
    TerminalCell user = MakeStyle(kUserColor);
    user.bBold = true;
    TerminalCell path = MakeStyle(kPathColor);
    path.bBold = true;
    const TerminalCell plain = MakeStyle(kForeground);
    LineBuilder builder(cols);
    PutSegment(builder, Segment{Piece{U"user@terminalviewer", user, false}, Piece{U":", plain, false}, Piece{U"~", path, false},
                                Piece{U"$ ", plain, false}, Piece{U"./showcase --all", plain, false}});
    return builder.Take();
}

bool IsSameCursor(const TerminalCursor& a, const TerminalCursor& b)
{
    return a.bVisible == b.bVisible && a.bBlinking == b.bBlinking && a.eShape == b.eShape && a.iCol == b.iCol && a.iRow == b.iRow &&
           a.bWide == b.bWide && a.optColor == b.optColor;
}

bool IsSamePreedit(const std::optional<TerminalPreedit>& a, const std::optional<TerminalPreedit>& b)
{
    if (a.has_value() != b.has_value())
        return false;
    if (!a)
        return true;
    if (a->iCaretCell != b->iCaretCell || a->vecCells.size() != b->vecCells.size())
        return false;
    for (size_t uCell = 0; uCell < a->vecCells.size(); ++uCell)
    {
        if (a->vecCells[uCell].uCodepoint != b->vecCells[uCell].uCodepoint || a->vecCells[uCell].eWidth != b->vecCells[uCell].eWidth)
            return false;
    }
    return true;
}
}   // namespace

ShowcaseScene::ShowcaseScene()
    : m_iCols(0)
    , m_iRows(0)
    , m_vecLines()
    , m_vecSections(size_t(EShowcaseSection::COUNT))
    , m_iPromptWidth(0)
    , m_iSelectBegin(-1)
    , m_iSelectEnd(-1)
    , m_iImeStep(kInitialImeStep)
    , m_eCursorShape(ECursorShape::BLOCK)
    , m_uLiveTick(0)
    , m_strLiveClock()
    , m_cursorSent()
    , m_optPreeditSent()
    , m_bCursorSent(false)
{
}

void ShowcaseScene::Fill(FakeFrameSource& source, int cols, int rows)
{
    m_iCols = std::max(1, cols);
    m_iRows = std::max(1, rows);
    const int iTotal = kScrollbackLines + m_iRows;

    std::vector<std::pair<EShowcaseSection, std::vector<ShowcaseLine>>> vecBuilt;
    vecBuilt.emplace_back(EShowcaseSection::TITLE, MakeTitle(m_iCols));
    vecBuilt.emplace_back(EShowcaseSection::ANSI_FG, MakeAnsiForeground(m_iCols));
    vecBuilt.emplace_back(EShowcaseSection::ANSI_BG, MakeAnsiBackground(m_iCols));
    vecBuilt.emplace_back(EShowcaseSection::PALETTE_256, MakePalette(m_iCols));
    vecBuilt.emplace_back(EShowcaseSection::TRUECOLOR, MakeTrueColor(m_iCols));
    vecBuilt.emplace_back(EShowcaseSection::STYLES, MakeStyles(m_iCols));
    vecBuilt.emplace_back(EShowcaseSection::CONTRAST, MakeContrast(m_iCols));
    vecBuilt.emplace_back(EShowcaseSection::UNDERLINE, MakeUnderlines(m_iCols));
    vecBuilt.emplace_back(EShowcaseSection::WIDE, MakeWide(m_iCols));
    vecBuilt.emplace_back(EShowcaseSection::EMOJI, MakeEmoji(m_iCols));
    vecBuilt.emplace_back(EShowcaseSection::BOX, MakeBox(m_iCols));
    vecBuilt.emplace_back(EShowcaseSection::BLOCKS, MakeBlocks(m_iCols));
    vecBuilt.emplace_back(EShowcaseSection::SELECTION, MakeSelection(m_iCols, &m_iSelectBegin, &m_iSelectEnd));
    vecBuilt.emplace_back(EShowcaseSection::LIVE, MakeLive(m_iCols, m_uLiveTick, m_strLiveClock));
    vecBuilt.emplace_back(EShowcaseSection::PROMPT, MakePrompt(m_iCols, &m_iPromptWidth));

    int iShowcaseLines = 0;
    for (const auto& [eSection, vecLines] : vecBuilt)
        iShowcaseLines += int(vecLines.size());

    // 맨 아래(프롬프트)부터 채운다. 남은 위쪽은 로그 줄, 그 맨 끝(절 바로 위)은 쇼케이스를 띄운 명령
    m_vecLines.assign(size_t(iTotal), ShowcaseLine());
    int iLine = iTotal - iShowcaseLines;
    for (int iLog = 0; iLog < iLine; ++iLog)
        m_vecLines[size_t(iLog)] = iLog + 1 == iLine ? MakeCommandLine(m_iCols) : MakeLogLine(m_iCols, iLog);
    for (auto& [eSection, vecLines] : vecBuilt)
    {
        Section& section = m_vecSections[size_t(eSection)];
        section.iFirstLine = -1;
        section.iLineCount = int(vecLines.size());
        for (ShowcaseLine& line : vecLines)
        {
            if (iLine >= 0)
            {
                if (section.iFirstLine < 0)
                    section.iFirstLine = iLine;
                m_vecLines[size_t(iLine)] = std::move(line);
            }
            ++iLine;
        }
    }

    source.SetColors(kBackground, kForeground);
    for (int iWrite = 0; iWrite < iTotal; ++iWrite)
        WriteLine(source, iWrite);
    const int iSelectLine = m_vecSections[size_t(EShowcaseSection::SELECTION)].iFirstLine;
    if (iSelectLine >= 0)
        source.SetSelection(iSelectLine, m_iSelectBegin, m_iSelectEnd);
    SendCursor(source, true);
}

void ShowcaseScene::UpdateLive(FakeFrameSource& source, uint32_t tick, const std::u32string& clock)
{
    m_uLiveTick = tick;
    m_strLiveClock = clock;
    const int iLine = m_vecSections[size_t(EShowcaseSection::LIVE)].iFirstLine;
    if (m_iCols <= 0 || iLine < 0)
        return;
    m_vecLines[size_t(iLine)] = MakeLive(m_iCols, tick, clock).front();
    WriteLine(source, iLine);
}

bool ShowcaseScene::SetImeStep(FakeFrameSource& source, int step)
{
    const int iStep = ((step % kImeSteps) + kImeSteps) % kImeSteps;
    if (iStep == m_iImeStep)
        return false;
    m_iImeStep = iStep;
    return m_iCols > 0 && SendCursor(source, false);
}

ECursorShape ShowcaseScene::CycleCursorShape(FakeFrameSource& source)
{
    switch (m_eCursorShape)
    {
    case ECursorShape::BLOCK:
        m_eCursorShape = ECursorShape::BAR;
        break;
    case ECursorShape::BAR:
        m_eCursorShape = ECursorShape::UNDERLINE;
        break;
    case ECursorShape::UNDERLINE:
        m_eCursorShape = ECursorShape::BLOCK_HOLLOW;
        break;
    case ECursorShape::BLOCK_HOLLOW:
        m_eCursorShape = ECursorShape::BLOCK;
        break;
    }
    if (m_iCols > 0)
        SendCursor(source, false);
    return m_eCursorShape;
}

bool ShowcaseScene::SyncCursor(FakeFrameSource& source)
{
    return m_iCols > 0 && SendCursor(source, false);
}

int ShowcaseScene::GetCols() const
{
    return m_iCols;
}

int ShowcaseScene::GetRows() const
{
    return m_iRows;
}

int ShowcaseScene::GetSectionLine(EShowcaseSection section) const
{
    return section < EShowcaseSection::COUNT ? m_vecSections[size_t(section)].iFirstLine : -1;
}

int ShowcaseScene::GetSectionLineCount(EShowcaseSection section) const
{
    return section < EShowcaseSection::COUNT ? m_vecSections[size_t(section)].iLineCount : 0;
}

const std::vector<ShowcaseLine>& ShowcaseScene::GetLines() const
{
    return m_vecLines;
}

int ShowcaseScene::GetPromptWidth() const
{
    return m_iPromptWidth;
}

std::pair<int, int> ShowcaseScene::GetSelection() const
{
    return {m_iSelectBegin, m_iSelectEnd};
}

TerminalCursor ShowcaseScene::GetCursor() const
{
    return m_cursorSent;
}

std::optional<TerminalPreedit> ShowcaseScene::GetPreedit() const
{
    return m_optPreeditSent;
}

std::optional<TerminalPreedit> ShowcaseScene::MakeImePreedit() const
{
    char32_t codepoint = 0;
    if (m_iImeStep == kImeJamoStep)
        codepoint = U'ㅎ';
    else if (m_iImeStep >= kImeSyllableFirstStep)
        codepoint = U'하';
    if (codepoint == 0)
        return std::nullopt;
    TerminalPreedit preedit;
    TerminalCell cell = MakeStyle(kForeground, kPreeditBackground);
    cell.uCodepoint = uint32_t(codepoint);
    cell.eWidth = ECellWidth::WIDE;
    TerminalCell tail = MakeStyle(kForeground, kPreeditBackground);
    tail.eWidth = ECellWidth::SPACER_TAIL;
    preedit.vecCells = {cell, tail};
    preedit.iCaretCell = kPreeditCells;
    return preedit;
}

bool ShowcaseScene::SendCursor(FakeFrameSource& source, bool force)
{
    const int iPromptLine = m_vecSections[size_t(EShowcaseSection::PROMPT)].iFirstLine;
    const int iRow = iPromptLine - source.GetViewportTop();
    const bool bVisible = iPromptLine >= 0 && iRow >= 0 && iRow < m_iRows;
    TerminalCursor cursor;
    cursor.bVisible = bVisible;
    cursor.bBlinking = true;
    cursor.eShape = m_eCursorShape;
    cursor.iCol = std::clamp(m_iPromptWidth, 0, m_iCols - 1);
    cursor.iRow = bVisible ? iRow : 0;   // 숨긴 동안은 뷰포트가 움직여도 같은 값(바뀜으로 세지 않는다)
    const std::optional<TerminalPreedit> optPreedit = bVisible ? MakeImePreedit() : std::nullopt;
    if (!force && m_bCursorSent && IsSameCursor(cursor, m_cursorSent) && IsSamePreedit(optPreedit, m_optPreeditSent))
        return false;
    source.SetCursor(cursor);
    source.SetPreedit(optPreedit);
    m_cursorSent = cursor;
    m_optPreeditSent = optPreedit;
    m_bCursorSent = true;
    return true;
}

void ShowcaseScene::WriteLine(FakeFrameSource& source, int line)
{
    const ShowcaseLine& content = m_vecLines[size_t(line)];
    source.SetLine(line, content.vecCells, content.vecGraphemes);
}
