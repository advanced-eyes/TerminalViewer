#pragma once

#include <TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalFrame.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

class FakeFrameSource;

// 쇼케이스 화면의 절(위 -> 아래 순서). COUNT 는 개수
enum class EShowcaseSection
{
    TITLE,
    ANSI_FG,
    ANSI_BG,
    PALETTE_256,
    TRUECOLOR,
    STYLES,
    CONTRAST,
    UNDERLINE,
    WIDE,
    EMOJI,
    BOX,
    BLOCKS,
    SELECTION,
    LIVE,
    PROMPT,
    COUNT
};

// 공급원 한 줄에 쓴 내용
struct ShowcaseLine
{
    std::vector<TerminalCell> vecCells;         // 앞에서부터 쓴 칸(열 수 이하). 그 뒤는 빈칸
    std::vector<std::u32string> vecGraphemes;   // TerminalCell::iGrapheme 가 가리키는 여러 코드포인트 글자
};

// FakeFrameSource 를 쇼케이스 내용으로 채운다. 창 · 위젯과 무관한 순수 로직이다(GUI 스레드 하나에서만 쓴다).
// 공급원 줄 0 .. kScrollbackLines + rows - 1 을 모두 쓴다: 맨 아래 줄이 프롬프트, 그 위가 실시간 줄, 그 위로 절들이 오고,
// 남은 위쪽(스크롤백 포함)은 로그 줄로 채운다. 열이 모자라면 절의 조각을 다음 줄로 넘기고, 그래도 넘치면 칸 경계에서 자른다.
class ShowcaseScene
{
public:
    static constexpr int kScrollbackLines = 400;
    // IME 흉내: 0..5 조합 없음(커서가 깜빡인다), 6 "ㅎ", 7..9 "하"(커서 대신 조합 글자와 캐럿)
    static constexpr int kImeSteps = 10;
    static constexpr int kInitialImeStep = 7;
    static constexpr TerminalRgb kBackground{22, 24, 30};   // 터미널 배경색(창도 첫 프레임 전에 이 색으로 칠한다)

    ShowcaseScene();

public:
    // source: cols x rows, 스크롤백 kScrollbackLines 로 만들었거나 Resize(cols, rows) 한 공급원. 모든 줄 · 선택 · 커서 · 조합을 다시 쓴다
    void Fill(FakeFrameSource& source, int cols, int rows);
    // 실시간 줄만 바꾼다(스피너 · 진행 막대 · 시각 · 막대 그래프). clock 은 보일 시각 문자열
    void UpdateLive(FakeFrameSource& source, uint32_t tick, const std::u32string& clock);
    // IME 단계(step % kImeSteps)를 바꾼다. 커서 · 조합만 바뀐다. 바뀌었으면 참
    bool SetImeStep(FakeFrameSource& source, int step);
    // BLOCK -> BAR -> UNDERLINE -> BLOCK_HOLLOW -> BLOCK. 새 모양을 돌려준다
    ECursorShape CycleCursorShape(FakeFrameSource& source);
    // 공급원의 뷰포트 위치에 맞춰 커서 줄을 다시 정한다(프롬프트 줄이 화면 밖이면 커서 · 조합을 숨긴다). 보낸 값이 바뀌었으면 참
    bool SyncCursor(FakeFrameSource& source);

public:
    int GetCols() const;
    int GetRows() const;
    int GetSectionLine(EShowcaseSection section) const;        // 그 절의 첫 줄(공급원의 절대 줄 번호). 없으면 -1
    int GetSectionLineCount(EShowcaseSection section) const;
    const std::vector<ShowcaseLine>& GetLines() const;          // 공급원 줄 번호 순서
    int GetPromptWidth() const;                                 // 프롬프트 글자의 칸 수(= 커서 열)
    std::pair<int, int> GetSelection() const;                  // 선택 줄의 [begin, end) 칸
    TerminalCursor GetCursor() const;                           // 마지막으로 공급원에 보낸 커서
    std::optional<TerminalPreedit> GetPreedit() const;          // 마지막으로 공급원에 보낸 조합

private:
    struct Section
    {
        int iFirstLine = -1;
        int iLineCount = 0;
    };

    std::optional<TerminalPreedit> MakeImePreedit() const;
    bool SendCursor(FakeFrameSource& source, bool force);
    void WriteLine(FakeFrameSource& source, int line);

    int m_iCols;
    int m_iRows;
    std::vector<ShowcaseLine> m_vecLines;
    std::vector<Section> m_vecSections;                         // EShowcaseSection 순서
    int m_iPromptWidth;
    int m_iSelectBegin;
    int m_iSelectEnd;
    int m_iImeStep;
    ECursorShape m_eCursorShape;
    uint32_t m_uLiveTick;
    std::u32string m_strLiveClock;
    TerminalCursor m_cursorSent;
    std::optional<TerminalPreedit> m_optPreeditSent;
    bool m_bCursorSent;
};
