#include "TerminalViewer/UI/TerminalSurfaceNative.h"

#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalPlatform.h"

#include <QWidget>

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

namespace
{
void ReleaseLayer(void* pLayer)
{
    if (pLayer)
        CFRelease(pLayer);
}
}

TerminalNativeHandle CreateNativeHandle(QWidget* pNativeWidget, qreal devicePixelRatio)
{
    TerminalNativeHandle handle;
    // winId() 가 NSView 인 것은 cocoa QPA 뿐이다(offscreen 테스트 등에서는 빈 핸들 → CPU 로)
    if (!pNativeWidget || !IsNativeSurfaceSupported())
        return handle;
    NSView* pView = reinterpret_cast<NSView*>(pNativeWidget->winId());
    if (!pView)
        return handle;
    @autoreleasepool
    {
        CAMetalLayer* pLayer = [CAMetalLayer layer];
        pLayer.contentsScale = devicePixelRatio;
        pLayer.opaque = YES;
        // contentsGravity 는 정하지 않는다: MoltenVK 가 스왑체인을 만들 때 present scaling 값으로 덮어쓴다(스펙 §8, 부록 A S1 (e)).
        pView.wantsLayer = YES;
        pView.layer = pLayer;
        // surface 가 지워질 때까지 우리가 retain 한다(NSView 가 먼저 사라져도 surface 가 유효하다, 스펙 §10).
        handle.pHandle = const_cast<void*>(CFBridgingRetain(pLayer));
    }
    handle.funcRelease = &ReleaseLayer;
    return handle;
}

void UpdateNativeScale(const TerminalNativeHandle& handle, qreal devicePixelRatio)
{
    if (!handle.pHandle)
        return;
    CAMetalLayer* pLayer = (__bridge CAMetalLayer*)handle.pHandle;
    pLayer.contentsScale = devicePixelRatio;
}
