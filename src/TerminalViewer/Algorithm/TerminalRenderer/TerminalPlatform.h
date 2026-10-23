#pragma once

// 이 플랫폼·QPA 에 GPU 화면(네이티브 surface)을 만드는 구현이 있는가: macOS cocoa(CAMetalLayer), Windows windows(HWND).
// 그 밖(offscreen 테스트, Linux)은 거짓이다. 렌더 스레드(새 화면이 쓸 Strategy 를 알릴 때)와 UI(네이티브 핸들을 만들 때)가
// 함께 보므로 Algorithm 층에 둔다. 아무 스레드에서나 부른다(QGuiApplication::platformName 만 읽는다).
bool IsNativeSurfaceSupported();

// Vulkan 라이브러리를 열 수 있는가(OCEAN_VK_DISABLE=1 이면 거짓, GPU 를 끈 빌드도 거짓). GPU 를 시도하기 전에 AUTO 가
// CPU 로 알릴지 정할 때 쓴다. 라이브러리를 열고 닫아 보기만 한다: Vulkan 함수를 부르지 않고 인스턴스를 만들지 않는다.
bool IsGpuLibraryAvailable();
