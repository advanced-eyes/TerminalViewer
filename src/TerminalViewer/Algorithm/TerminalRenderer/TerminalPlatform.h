#pragma once

#include <QString>
#include <QtGlobal>

#include <vector>

class QScreen;

// 이 플랫폼·QPA 에 GPU 화면(네이티브 surface)을 만드는 구현이 있는가: macOS cocoa(CAMetalLayer), Windows windows(HWND).
// 그 밖(offscreen 테스트, Linux)은 거짓이다. 렌더 스레드(새 화면이 쓸 Strategy 를 알릴 때)와 UI(네이티브 핸들을 만들 때)가
// 함께 보므로 Algorithm 층에 둔다. 아무 스레드에서나 부른다(QGuiApplication::platformName 만 읽는다).
bool IsNativeSurfaceSupported();

// Vulkan 라이브러리를 열 수 있는가(TERMINALVIEWER_VK_DISABLE=1 이면 거짓, GPU 를 끈 빌드도 거짓). GPU 를 시도하기 전에 AUTO 가
// CPU 로 알릴지 정할 때 쓴다. 라이브러리를 열고 닫아 보기만 한다: Vulkan 함수를 부르지 않고 인스턴스를 만들지 않는다.
bool IsGpuLibraryAvailable();

// 활성 표시 경로 하나(Windows QueryDisplayConfig): 원본의 GDI 장치 이름과 대상의 주사율(유리수, Hz = 분자 / 분모)
struct TerminalDisplayRefreshPath
{
    QString strGdiDeviceName;     // 예: \\.\DISPLAY1 (Windows QPA 의 QScreen::name() 과 같다)
    quint32 uRefreshNumerator = 0;
    quint32 uRefreshDenominator = 0;
};

// 그 GDI 장치 이름(대소문자 무시)의 경로 중 주사율이 있는 첫 경로의 주사율(Hz). 없으면 0
double FindDisplayRefreshRate(const QString& gdiDeviceName, const std::vector<TerminalDisplayRefreshPath>& paths);
// Qt 값과 정확한 값 중 무엇을 쓸지: 정확한 값이 있고(유한한 양수) Qt 값과 1Hz 안이면(Qt 5.15 의 Windows 는 정수로 버린다:
// 59.949 → 59) 정확한 값, Qt 값이 없으면(0 이하) 정확한 값, 그 밖(정확한 값 없음, 1Hz 이상 다름: 다른 모니터를 잡았을 수 있다)은 Qt 값
double ChooseRefreshRate(double qtHz, double exactHz);
// Windows: QueryDisplayConfig 의 활성 경로(원본 GDI 장치 이름 + 대상 주사율). 그 밖의 플랫폼·실패: 빈 목록. GUI 스레드에서 부른다
std::vector<TerminalDisplayRefreshPath> QueryDisplayRefreshPaths();
// 화면의 주사율(Hz). Windows 는 그 QScreen 의 모니터의 정확한 값(못 찾으면 Qt 값), 그 밖은 QScreen::refreshRate(). nullptr 은 0
double ScreenRefreshRate(const QScreen* pScreen);
