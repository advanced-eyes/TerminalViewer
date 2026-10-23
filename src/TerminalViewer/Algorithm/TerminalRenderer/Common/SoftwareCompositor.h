#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/Common/DrawList.h"
#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphAtlas.h"

#include <QImage>
#include <QRegion>

namespace SoftwareCompositor
{
// target: Format_ARGB32_Premultiplied, 장치 픽셀. targetSize 가 유효하면 target 크기가 다를 때 그 크기로 다시 만들고
// 전체를 합성한다(보통 화면 크기). targetSize 가 비어 있으면 target 크기를 그대로 쓰고, target 이 비어 있을 때만
// 목록에서 (iCols*셀 너비+2*여백) × (iRows*셀 높이+2*여백) 를 쓴다.
// 전체 합성은 target 전체(여백·오른쪽/아래 나머지 포함)를 clearColor 로 지운다. bFullRedraw 가 거짓이면
// vecDirtyRows 의 행 띠(target 가로 전체 × 셀 높이, 여백·스크롤 오프셋 반영, target 으로 자름)만 지우고 다시 합성한다.
// 띠와 겹치는 모든 쿼드를 띠로 잘라 그린다. 반환: 다시 그린 영역(장치 픽셀)
QRegion Compose(const DrawList& list, const GlyphAtlas& mono, const GlyphAtlas& color, QImage& target,
                const QSize& targetSize = QSize());
}
