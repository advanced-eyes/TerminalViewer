#pragma once

// --terminal-renderer 값(스펙 §11). AUTO: GPU 화면을 처음 붙일 때 Vulkan 을 시도하고 쓸 수 없으면 CPU 로 그린다.
// CPU: Vulkan 을 시도하지 않는다. 앱(Application)이 렌더 스레드의 무거운 헤더 없이 쓸 수 있게 따로 둔다.
enum class ETerminalRendererMode { AUTO, CPU };
