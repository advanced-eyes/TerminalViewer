#include "TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VulkanLoader.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QtGlobal>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace
{
constexpr const char* kDisableVariable = "OCEAN_VK_DISABLE";
constexpr const char* kValidationVariable = "OCEAN_VK_VALIDATION";
constexpr const char* kGetInstanceProcAddrName = "vkGetInstanceProcAddr";

constexpr const char* kSdkVariable = "VULKAN_SDK";

// Vulkan 로더(레이어를 켤 수 있다) 검색 이름
QStringList LoaderNames()
{
#if defined(Q_OS_WIN)
    return {QStringLiteral("vulkan-1.dll")};
#elif defined(Q_OS_MACOS)
    // 이름만 주면 dyld 는 DYLD_* 경로와 /usr/lib 만 본다(macOS 13 이후 /usr/local/lib 는 기본 대체 경로가 아니다).
    // Vulkan SDK 는 로더를 /usr/local/lib(시스템 설치) 또는 $VULKAN_SDK/lib 에 둔다.
    QStringList listNames = {QStringLiteral("libvulkan.1.dylib"), QStringLiteral("libvulkan.dylib")};
    const QString strSdk = qEnvironmentVariable(kSdkVariable);
    if (!strSdk.isEmpty())
        listNames << QDir(strSdk).absoluteFilePath(QStringLiteral("lib/libvulkan.1.dylib"));
    listNames << QStringLiteral("/usr/local/lib/libvulkan.1.dylib");
    return listNames;
#else
    return {QStringLiteral("libvulkan.so.1"), QStringLiteral("libvulkan.so")};
#endif
}

// 시스템 기본 검색 이름: 로더, 그다음 macOS 는 시스템에 설치된 MoltenVK(로더 없이 바로 쓴다. 레이어는 켤 수 없다)
QStringList SystemLibraryNames()
{
    QStringList listNames = LoaderNames();
#if defined(Q_OS_MACOS)
    listNames << QStringLiteral("libMoltenVK.dylib") << QStringLiteral("/usr/local/lib/libMoltenVK.dylib");
#endif
    return listNames;
}

// 라이브러리 핸들(HMODULE / dlopen 핸들)을 연다. 못 열면 nullptr
void* OpenLibraryHandle(const QString& path)
{
#if defined(Q_OS_WIN)
    // DLL 심기를 막는다: 현재 폴더·PATH 는 보지 않고 System32 와 기본 안전 경로(앱 폴더, AddDllDirectory)만 본다.
    return LoadLibraryExW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), nullptr,
                          LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
#else
    return dlopen(QFile::encodeName(path).constData(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void CloseLibraryHandle(void* pHandle)
{
#if defined(Q_OS_WIN)
    FreeLibrary(static_cast<HMODULE>(pHandle));
#else
    dlclose(pHandle);
#endif
}

PFN_vkGetInstanceProcAddr FindGetInstanceProcAddr(void* pHandle)
{
#if defined(Q_OS_WIN)
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(pHandle), kGetInstanceProcAddrName)));
#else
    return reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(pHandle, kGetInstanceProcAddrName));
#endif
}

// 라이브러리를 열고 vkGetInstanceProcAddr 를 찾는다. 연 라이브러리는 닫지 않는다
// (vk-bootstrap 이 함수 포인터를 프로세스 동안 보관한다).
PFN_vkGetInstanceProcAddr OpenLibrary(const QString& path)
{
    void* pHandle = OpenLibraryHandle(path);
    if (!pHandle)
        return nullptr;
    const PFN_vkGetInstanceProcAddr fp = FindGetInstanceProcAddr(pHandle);
    if (!fp)
        CloseLibraryHandle(pHandle);
    return fp;
}

// 라이브러리를 열어 vkGetInstanceProcAddr 가 있는지 보고 바로 닫는다(Vulkan 함수는 부르지 않는다)
bool CanOpenLibrary(const QString& path)
{
    void* pHandle = OpenLibraryHandle(path);
    if (!pHandle)
        return false;
    const bool bFound = FindGetInstanceProcAddr(pHandle) != nullptr;
    CloseLibraryHandle(pHandle);
    return bFound;
}

// 앱 번들에 넣은 MoltenVK(macOS 배포판). 없으면 빈 문자열
QString BundledLibraryPath()
{
#if defined(Q_OS_MACOS)
    if (QCoreApplication::instance())
    {
        const QString strBundled =
            QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("../Frameworks/libMoltenVK.dylib"));
        if (QFileInfo::exists(strBundled))
            return QDir::cleanPath(strBundled);
    }
#endif
    return QString();
}

PFN_vkGetInstanceProcAddr OpenFirst(const QStringList& listNames, QString* pLibraryUsed)
{
    for (const QString& strName : listNames)
    {
        if (PFN_vkGetInstanceProcAddr fp = OpenLibrary(strName))
        {
            if (pLibraryUsed)
                *pLibraryUsed = strName;
            return fp;
        }
    }
    return nullptr;
}
}

PFN_vkGetInstanceProcAddr LoadVulkanLibrary(QString* pLibraryUsed, bool* pValidation)
{
    if (pValidation)
        *pValidation = false;
    if (pLibraryUsed)
        pLibraryUsed->clear();

    if (qEnvironmentVariableIntValue(kDisableVariable) == 1)
    {
        if (pLibraryUsed)
            *pLibraryUsed = QStringLiteral("%1=1: Vulkan 을 쓰지 않는다").arg(QLatin1String(kDisableVariable));
        return nullptr;
    }

    if (qEnvironmentVariableIntValue(kValidationVariable) == 1)
    {
        // 검증 레이어는 Vulkan 로더(SDK)를 거쳐야 켤 수 있다. MoltenVK 를 바로 여는 경로는 레이어를 못 켜므로 여기서 보지 않는다.
        // 레이어가 실제로 켜졌는지는 VulkanContext 가 인스턴스를 만들 때 정한다(IsValidationEnabled).
        if (PFN_vkGetInstanceProcAddr fp = OpenFirst(LoaderNames(), pLibraryUsed))
        {
            if (pValidation)
                *pValidation = true;
            return fp;
        }
        qWarning("Vulkan: %s=1 but no Vulkan loader was found; continuing without validation", kValidationVariable);
    }

    // 배포판(macOS): 앱 번들에 넣은 MoltenVK 를 로더 없이 바로 쓴다.
    const QString strBundled = BundledLibraryPath();
    if (!strBundled.isEmpty())
    {
        if (PFN_vkGetInstanceProcAddr fp = OpenLibrary(strBundled))
        {
            if (pLibraryUsed)
                *pLibraryUsed = strBundled;
            return fp;
        }
    }

    if (PFN_vkGetInstanceProcAddr fp = OpenFirst(SystemLibraryNames(), pLibraryUsed))
        return fp;

    if (pLibraryUsed)
        *pLibraryUsed = QStringLiteral("Vulkan 라이브러리를 찾지 못했다(%1)").arg(SystemLibraryNames().join(QStringLiteral(", ")));
    return nullptr;
}

bool IsVulkanLibraryAvailable()
{
    if (qEnvironmentVariableIntValue(kDisableVariable) == 1)
        return false;
    // LoadVulkanLibrary 가 볼 곳을 같은 순서로 본다(검증 레이어용 로더 이름은 시스템 기본 검색에 들어 있다)
    QStringList listNames = SystemLibraryNames();
    const QString strBundled = BundledLibraryPath();
    if (!strBundled.isEmpty())
        listNames.prepend(strBundled);
    for (const QString& strName : listNames)
    {
        if (CanOpenLibrary(strName))
            return true;
    }
    return false;
}
