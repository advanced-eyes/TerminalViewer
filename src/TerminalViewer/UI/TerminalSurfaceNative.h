#pragma once

#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalRenderer.h"

#include <QtGlobal>

class QWidget;

// 네이티브 창 → 렌더 스레드에 넘길 핸들(스펙 §10). GUI 스레드에서만 부른다. Vulkan 은 부르지 않는다.
// macOS(TerminalSurfaceMac.mm): winId() 의 NSView 에 wantsLayer 를 켜고 새 CAMetalLayer 를 layer 로 붙여 retain 한다.
//   contentsScale = 배율, opaque = YES. contentsGravity 는 MoltenVK 가 스왑체인을 만들 때 덮어쓰므로 정하지 않는다.
//   funcRelease = CFRelease(렌더 스레드가 surface 를 지운 뒤 부른다).
// Windows(TerminalSurface.cpp): pHandle = HWND, funcRelease 는 아무것도 하지 않는다.
// 그 밖의 플랫폼이나 네이티브 창을 만들 수 없는 QPA(offscreen 등)는 빈 핸들(pHandle == nullptr)을 돌려준다.
// 핸들을 만들 수 있는지는 IsNativeSurfaceSupported()(Algorithm/TerminalRenderer/TerminalPlatform.h)가 정한다.
TerminalNativeHandle CreateNativeHandle(QWidget* pNativeWidget, qreal devicePixelRatio);
void UpdateNativeScale(const TerminalNativeHandle& handle, qreal devicePixelRatio);
