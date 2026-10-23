#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct TerminalRgb
{
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;

    bool operator==(const TerminalRgb& other) const { return r == other.r && g == other.g && b == other.b; }
    bool operator!=(const TerminalRgb& other) const { return !(*this == other); }
};

enum class EUnderline { NONE, SINGLE, DOUBLE, CURLY, DOTTED, DASHED };
enum class ECellWidth { NARROW, WIDE, SPACER_TAIL };   // WIDE 다음 칸은 SPACER_TAIL
enum class ECursorShape { BAR, BLOCK, UNDERLINE, BLOCK_HOLLOW };
enum class EFrameDirty { NONE, PARTIAL, FULL };

struct TerminalCell
{
    uint32_t uCodepoint = 0;    // 0 = 빈칸
    int32_t iGrapheme = -1;     // -1, 아니면 행의 vecGraphemes 인덱스(여러 코드포인트로 된 글자)
    TerminalRgb fg{255, 255, 255};
    TerminalRgb bg{0, 0, 0};
    // 엔진 팔레트·기본색까지 풀어 놓은 값. 렌더러는 밑줄 색을 반전·선택으로 바꾸지 않는다: 밑줄 색을 따로 정하지 않은 칸은
    // ① 이 반전·선택을 반영한 최종 글자색을 넣는다(또는 ① 의 변환에 "기본 밑줄색" 표시를 더해 계약을 넓힌다, 스펙 부록 C)
    TerminalRgb underlineColor{255, 255, 255};
    bool bBgDefault = true;     // 기본 배경이면 배경 사각형을 만들지 않는다
    bool bBold = false;
    bool bItalic = false;
    bool bFaint = false;
    bool bInverse = false;
    bool bInvisible = false;
    bool bStrikethrough = false;
    EUnderline eUnderline = EUnderline::NONE;
    ECellWidth eWidth = ECellWidth::NARROW;
};

struct TerminalRow
{
    uint64_t llId = 0;          // 엔진의 행 id. 재사용되지 않는다
    int iViewportY = 0;         // 0..rows-1, 위 여유 줄은 음수, 아래 여유 줄은 rows 이상
    bool bDirty = false;
    int iSelectBegin = -1;      // 선택 칸 범위 [begin, end), 없으면 -1
    int iSelectEnd = -1;
    std::vector<TerminalCell> vecCells;   // bDirty 인 줄만 채운다. 깨끗한 줄은 비어도 된다
    std::vector<std::u32string> vecGraphemes;
};

struct TerminalCursor
{
    bool bVisible = false;
    bool bBlinking = false;
    ECursorShape eShape = ECursorShape::BLOCK;
    int iCol = 0;               // 뷰포트 좌표
    int iRow = 0;
    bool bWide = false;
    std::optional<TerminalRgb> optColor;  // 프로그램이 정한 커서 색(OSC 12)
};

// IME 조합 중인 글자. 커서 위치부터 그린다. 행과 달리 vecGraphemes 가 없어서 여러 코드포인트로 된 칸은 첫 코드포인트만
// 그린다: ① 은 미리 합친(precomposed) 글자를 넣는다(필요하면 계약을 넓힌다, 스펙 부록 C)
struct TerminalPreedit
{
    std::vector<TerminalCell> vecCells;
    int iCaretCell = 0;
};

struct TerminalFrame
{
    int iCols = 0;
    int iRows = 0;
    EFrameDirty eDirty = EFrameDirty::NONE;
    TerminalRgb background{0, 0, 0};      // 기본색 (여백과 기본 배경 칠하기)
    TerminalRgb foreground{255, 255, 255};
    std::vector<TerminalRow> vecRows;     // 위 → 아래, 여유 줄 포함
    TerminalCursor cursor;
    std::optional<TerminalPreedit> optPreedit;
};

struct TerminalCaptureRequest
{
    int iOverscanAbove = 0;
    int iOverscanBelow = 0;
    bool bFull = false;         // 참이면 깨끗한 줄도 vecCells 를 채우고 eDirty = FULL
};

class ITerminalFrameSource
{
public:
    virtual ~ITerminalFrameSource() = default;
    // 렌더 스레드에서 부른다. 최신 상태를 frame 에 채운다. 바뀐 것이 없으면 eDirty = NONE.
    // 엔진 잠금을 오래 잡지 않는다(2단계 갱신: 잠금 → begin → 해제 → end). 렌더 스레드는 모든 화면이 함께 쓰고,
    // Stop 은 4초 기다린 뒤 스레드를 강제로 끝낸다(스펙 부록 C)
    virtual void CaptureFrame(const TerminalCaptureRequest& request, TerminalFrame& frame) = 0;
    // 뷰포트를 줄 단위로 옮긴다(픽셀 스크롤이 줄 경계를 넘을 때). 렌더 스레드에서 부른다.
    // 양수 = 아래(최근 출력) 쪽. 실제로 옮긴 줄 수를 돌려준다(스크롤백 맨 위·맨 아래에서는 요청보다 적다).
    virtual int ScrollViewport(int rows) = 0;
};
