#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalPlatform.h"

#ifdef TERMINALVIEWER_GPU
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanLoader.h"
#endif

#include <QGuiApplication>
#include <QString>

bool IsNativeSurfaceSupported()
{
#if defined(Q_OS_MACOS)
    // winId() 가 NSView 인 것은 cocoa QPA 뿐이다
    return QGuiApplication::platformName() == QLatin1String("cocoa");
#elif defined(Q_OS_WIN)
    return QGuiApplication::platformName() == QLatin1String("windows");
#else
    return false;   // 이 플랫폼은 GPU 로 화면에 그리지 않는다(범위 밖)
#endif
}

bool IsGpuLibraryAvailable()
{
#ifdef TERMINALVIEWER_GPU
    return IsVulkanLibraryAvailable();
#else
    return false;   // GPU 를 끈 빌드
#endif
}
