#include "TerminalViewer/Algorithm/TerminalRenderer/TerminalPlatform.h"

#ifdef TERMINALVIEWER_GPU
#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanLoader.h"
#endif

#include <QGuiApplication>
#include <QScreen>
#include <QString>

#ifdef Q_OS_WIN
// QueryDisplayConfig · DisplayConfigGetDeviceInfo 는 Windows 7(0x0601)부터 있다. MinGW 8.1 의 기본 WINVER(0x0502)에서는
// 선언이 숨으므로 이 번역 단위에서만 올린다(windows.h 보다 먼저. Qt 헤더는 windows.h 를 include 하지 않는다).
#if !defined(WINVER) || WINVER < 0x0601
#undef WINVER
#define WINVER 0x0601
#endif
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0601
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include <windows.h>
#endif

#include <cmath>

namespace
{
constexpr double kMaxQtRefreshDifferenceHz = 1.0;   // Qt 값과 정확한 값이 이만큼 다르면 다른 모니터를 잡았다고 본다
#ifdef Q_OS_WIN
constexpr int kMaxDisplayConfigQueries = 3;          // 크기를 잰 뒤 화면 구성이 바뀌어 버퍼가 모자라면 다시 잰다
#endif
}

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

double FindDisplayRefreshRate(const QString& gdiDeviceName, const std::vector<TerminalDisplayRefreshPath>& paths)
{
    if (gdiDeviceName.isEmpty())
    {
        return 0.0;
    }
    for (const TerminalDisplayRefreshPath& path : paths)
    {
        if (path.uRefreshNumerator == 0 || path.uRefreshDenominator == 0 ||
            path.strGdiDeviceName.compare(gdiDeviceName, Qt::CaseInsensitive) != 0)
        {
            continue;
        }
        return double(path.uRefreshNumerator) / double(path.uRefreshDenominator);
    }
    return 0.0;
}

double ChooseRefreshRate(double qtHz, double exactHz)
{
    if (!std::isfinite(exactHz) || exactHz <= 0.0)
    {
        return qtHz;
    }
    if (!(qtHz > 0.0))
    {
        return exactHz;
    }
    return std::abs(exactHz - qtHz) < kMaxQtRefreshDifferenceHz ? exactHz : qtHz;
}

std::vector<TerminalDisplayRefreshPath> QueryDisplayRefreshPaths()
{
    std::vector<TerminalDisplayRefreshPath> vecResult;
#ifdef Q_OS_WIN
    std::vector<DISPLAYCONFIG_PATH_INFO> vecPaths;
    std::vector<DISPLAYCONFIG_MODE_INFO> vecModes;
    LONG lResult = ERROR_INSUFFICIENT_BUFFER;
    for (int iQuery = 0; iQuery < kMaxDisplayConfigQueries && lResult == ERROR_INSUFFICIENT_BUFFER; ++iQuery)
    {
        UINT32 uPaths = 0;
        UINT32 uModes = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &uPaths, &uModes) != ERROR_SUCCESS)
        {
            return vecResult;
        }
        vecPaths.resize(uPaths);
        vecModes.resize(uModes);
        lResult = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &uPaths, vecPaths.data(), &uModes, vecModes.data(), nullptr);
        vecPaths.resize(uPaths);
    }
    if (lResult != ERROR_SUCCESS)
    {
        return vecResult;
    }
    for (const DISPLAYCONFIG_PATH_INFO& path : vecPaths)
    {
        // 원본(어댑터 + id)의 GDI 장치 이름: QScreen::name() 과 같은 \\.\DISPLAYn
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS)
        {
            continue;
        }
        TerminalDisplayRefreshPath entry;
        entry.strGdiDeviceName = QString::fromWCharArray(source.viewGdiDeviceName);
        entry.uRefreshNumerator = path.targetInfo.refreshRate.Numerator;
        entry.uRefreshDenominator = path.targetInfo.refreshRate.Denominator;
        vecResult.push_back(entry);
    }
#endif
    return vecResult;
}

double ScreenRefreshRate(const QScreen* pScreen)
{
    if (!pScreen)
    {
        return 0.0;
    }
    const double dQtHz = pScreen->refreshRate();
#ifdef Q_OS_WIN
    // Qt 5.15 의 windows QPA 는 주사율을 정수로 버린다(GetDeviceCaps VREFRESH, 59.949 -> 59). 그 모니터의 정확한 값을 쓴다
    return ChooseRefreshRate(dQtHz, FindDisplayRefreshRate(pScreen->name(), QueryDisplayRefreshPaths()));
#else
    return dQtHz;
#endif
}
