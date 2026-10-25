#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawList.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphCache.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalContrast.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalFrame.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/TerminalTheme.h"

#include <unordered_map>
#include <vector>

enum class EBuildResult { OK, NEED_FULL_FRAME };

struct DrawListBuildStats { int iRebuiltRows = 0; int iReusedRows = 0; };

class DrawListBuilder           // 화면 하나 전용(행 캐시를 가진다)
{
public:
    EBuildResult Build(const TerminalFrame& frame, const TerminalTheme& theme, bool focused, bool cursorBlinkOn,
                       int scrollOffsetPx, GlyphCache& cache, DrawList& out, DrawListBuildStats& stats);
    void Invalidate();              // 크기·배율·글꼴·테마가 바뀜 → 캐시를 버림
    bool NeedsFullFrame() const;    // 다음 캡처를 bFull 로 해야 하는지

private:
    // 행 안 좌표로 저장한 세 층. iRow 는 조립할 때 기록한다.
    struct CachedRow
    {
        std::vector<DrawQuad> vecBackground;
        std::vector<DrawQuad> vecGlyph;
        std::vector<DrawQuad> vecDecoration;
        std::vector<TerminalCell> vecCells;          // 커서가 그 칸 글자를 다시 그릴 때 쓴다
        std::vector<std::u32string> vecGraphemes;
        int iViewportY = 0;                          // 마지막으로 그린 뷰포트 행
    };

    // 캐시 재사용 조건 중 프레임·테마·셀 크기 부분
    struct Context
    {
        TerminalTheme theme;
        TerminalRgb background;
        TerminalRgb foreground;
        int iCols = 0;
        const GlyphCache* pCache = nullptr;          // 다른 GlyphCache(글꼴·배율)면 캐시를 버린다
        CellMetrics metrics;
        bool operator==(const Context& other) const;
    };

    void DropCache();   // 행 캐시를 버리고 다음 캡처를 FULL 로, 다음 빌드를 전체 다시 그리기로 만든다

    bool m_bNeedFull = true;
    bool m_bHasContext = false;
    Context m_context;
    uint32_t m_uCacheGeneration = 0;
    std::unordered_map<uint64_t, CachedRow> m_hashRows;
    int m_iPrevCursorRow = -1;       // 이전 프레임에 커서를 그린 행, 없으면 -1
    int m_iPrevPreeditRow = -1;
    int m_iPrevScrollOffsetPx = 0;
    bool m_bHasPrevBuild = false;
    ContrastCache m_contrast;        // 테마의 최소 대비로 고친 글자색((글자색, 배경색) 짝마다)
};
