#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalFrame.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

enum class EFakePattern { BLANK, MIXED, FLOOD };

// 테스트·도구 전용 화면 공급원. 아무 스레드에서나 부를 수 있다(내부 잠금).
class FakeFrameSource : public ITerminalFrameSource
{
public:
    FakeFrameSource(int cols, int rows, int scrollbackLines);

    void SetText(int line, int col, const std::u32string& text, const TerminalCell& style);   // 2칸 글자는 WIDE + SPACER_TAIL
    void SetCell(int line, int col, const TerminalCell& cell);
    void SetCursor(const TerminalCursor& cursor);
    void SetPreedit(const std::optional<TerminalPreedit>& preedit);
    void SetSelection(int line, int begin, int end);
    void SetColors(TerminalRgb background, TerminalRgb foreground);
    void FillPattern(EFakePattern pattern, uint32_t seed);
    void Resize(int cols, int rows);
    int GetViewportTop() const;               // 화면 맨 위 줄의 절대 번호
    int GetCaptureCount() const;
    TerminalCaptureRequest GetLastRequest() const;
    void CaptureFrame(const TerminalCaptureRequest& request, TerminalFrame& frame) override;
    int ScrollViewport(int rows) override;

private:
    struct Line
    {
        std::vector<TerminalCell> vecCells;
        std::vector<std::u32string> vecGraphemes;
        uint32_t uVersion = 0;
        int iSelectBegin = -1;
        int iSelectEnd = -1;
    };

    void ClearLine(Line& line);
    void SetTextLocked(int line, int col, const std::u32string& text, const TerminalCell& style);
    void FillMixedLine(int line, uint32_t seed);

    mutable std::mutex m_mutex;
    int m_iCols;
    int m_iRows;
    int m_iScrollback;
    int m_iViewportTop = 0;
    std::vector<Line> m_vecLines;
    TerminalCursor m_cursor;
    std::optional<TerminalPreedit> m_optPreedit;
    TerminalRgb m_background{0, 0, 0};
    TerminalRgb m_foreground{255, 255, 255};
    std::unordered_map<int, uint32_t> m_hashSentVersions;
    uint32_t m_uNextVersion = 0;   // 모든 줄이 함께 쓰는 단조 증가 버전: 행 id 가 다시 쓰이지 않는다
    bool m_bNeedFull = true;
    bool m_bAllDirty = false;
    bool m_bMetaDirty = false;
    int m_iCaptureCount = 0;
    TerminalCaptureRequest m_lastRequest;
};
