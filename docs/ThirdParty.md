# 서드파티 라이브러리와 에셋

TerminalViewer 저장소에는 서드파티 **소스 코드**를 두지 않는다. `TERMINALVIEWER_GPU=ON` 이면 CMake 구성 단계가 감사한 버전을
정확히 내려받는다(`cmake/TerminalViewerDependencies.cmake`). 저장소에 남긴 서드파티 파일은 테스트용 Cascadia Mono 글꼴뿐이다.

- 이 문서의 감사 내용은 Ocean `docs/ThirdParty.md`(Ocean `main` 6d539cc + 작업 트리)의 "터미널 GPU 렌더러 의존성 (2026-10 감사)" 절과
  "Cascadia Mono" 절에서 옮겼다. 버전 · URL · SHA256 은 바꾸지 않았다.
- 상용 배포 라이선스 정책: README §9, Ocean `docs/LicenseCompliance.md`

| 이름 | 얻는 곳 | 고정 | 라이선스 | 수정 |
| --- | --- | --- | --- | --- |
| Vulkan-Headers (`TERMINALVIEWER_GPU`) | 내려받기 | 태그 `v1.4.365` | Apache-2.0 OR MIT | 없음 |
| vk-bootstrap (`TERMINALVIEWER_GPU`) | 내려받기 | 태그 `v1.4.365` | MIT | 없음 |
| VulkanMemoryAllocator (`TERMINALVIEWER_GPU`) | 내려받기 | 태그 `v3.4.0` | MIT | 없음 |
| MoltenVK (`TERMINALVIEWER_GPU`, macOS) | 내려받기(미리 만든 dylib) | 릴리스 `v1.4.2` `MoltenVK-macos.tar` | Apache-2.0 (+ 정적으로 들어간 코드, 아래) | 없음 |
| Cascadia Mono (테스트만) | 저장소 `tests/fonts/` | 2407.24 (Regular, Bold, Italic, BoldItalic, 글꼴 버전 2407.024) | OFL-1.1 | 없음 |

## 내려받기

- 각 라이브러리는 GitHub 소스 압축(`archive/refs/tags/<태그>.tar.gz`)이나 릴리스 파일을 `URL_HASH SHA256` 으로 고정해 내려받는다. 해시가 다르면 구성이 멈춘다.
- 서드파티의 CMake 는 돌리지 않는다(`SOURCE_SUBDIR` 에 없는 폴더). 빌드 대상은 `cmake/TerminalViewerDependencies.cmake` 가 만든다.
- FetchContent 이름은 `tv_vulkanheaders` · `tv_vkbootstrap` · `tv_vma` · `tv_moltenvk`, 대상 이름은 `TerminalViewerVulkanHeaders` · `TerminalViewerVkBootstrap` · `TerminalViewerVma` 다.
  서브모듈로 붙인 부모 프로젝트의 이름과 겹치지 않게 앞에 붙였다. FetchContent 이름은 내려받기 단계의 경로에 네 번 들어가므로 짧게 둔다(Windows 경로 260자 한도).
- 처음 구성 뒤에는 `-DFETCHCONTENT_FULLY_DISCONNECTED=ON` 으로 인터넷 없이 구성할 수 있다.
- `OFF` 이면 하나도 내려받지 않는다. 기본값은 Windows `OFF`, 그 밖 `ON` 이다(Windows 에서 GPU 렌더러는 Windows 빌드 확인 후 켠다).
- 패치하는 라이브러리는 없다.

### 버전을 올릴 때

1. Ocean `docs/LicenseCompliance.md` §2 의 감사 절차를 새 버전에 다시 밟는다.
2. `cmake/TerminalViewerDependencies.cmake` 의 URL 과 SHA256 을 바꾼다.
3. 이 문서의 표와 아래 절을 고친다.

## 터미널 GPU 렌더러 의존성 (2026-10 감사, `TERMINALVIEWER_GPU=ON`)

`cmake/TerminalViewerDependencies.cmake` 에서 내려받는다(MoltenVK 는 `if(APPLE)`).

| 이름 | 압축 SHA256 | 쓰는 파일 | 라이선스 | 의무 (동봉할 전문 파일) |
| --- | --- | --- | --- | --- |
| Vulkan-Headers v1.4.365 | `ed832b3292cce324ebba6522fd2d3004bf1b797fa49bfbcad0b302ff23d56abc` | `include/vulkan/`, `include/vk_video/` (모든 파일에 `SPDX-License-Identifier: Apache-2.0 OR MIT`) | Apache-2.0 OR MIT | 저작권(Khronos) 고지 + 전문 → `LICENSE.Vulkan-Headers` (Apache-2.0 전문). NOTICE 파일 없음 |
| vk-bootstrap v1.4.365 | `35eef8e046ca0a63f2ffc180a52561081d71bd18d19395a45fb78eb095423b4b` | `src/VkBootstrap*.{h,cpp,inl}` | MIT | 저작권 + 허가 문구 → `LICENSE.vk-bootstrap-MIT` |
| VulkanMemoryAllocator v3.4.0 | `822aa850c6ce77346ae96a8a1d351d52e77e85929f35363849a0a4e638e0a2a1` | `include/vk_mem_alloc.h` | MIT | 저작권(AMD) + 허가 문구 → `LICENSE.VulkanMemoryAllocator-MIT` |
| MoltenVK v1.4.2 `MoltenVK-macos.tar` | `f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e` | `MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib` (SHA256 `aef00b13bcc808adf15b85bef9ae67393d92be7ed5dfe41cad16fa809e4a4c5f`) | Apache-2.0 | 전문 → `LICENSE.MoltenVK-Apache-2.0`, 정적으로 들어간 코드의 고지 → `LICENSE.MoltenVK-embedded.txt`(`resources/licenses/`) |

- 위 전문 파일은 쓰는 쪽이 배포물에 넣는다. `terminalviewer_install_licenses(<설치 폴더>)` 가 이 이름 그대로 설치한다(README §6).
- Vulkan-Headers: 파일 50개(`include/`)가 모두 SPDX 줄을 가진다. GPL/LGPL 문구 없음. `LICENSE.md` 가 말하는 예외(`parse_dependency.py`)는 `registry/` 의 스크립트라 쓰지 않는다.
- vk-bootstrap: 컴파일하는 파일은 `src/` 의 5개뿐이고 모두 MIT 머리말이다(`VkBootstrapDispatch.h` 는 Cody Goodson 저작권 줄이 추가). 생성 코드(`VkBootstrapDispatch.h`, `*FeatureChain*`)는 Vulkan 레지스트리에서 만든 자체 코드로 외부 코드 인용 문구가 없다. GPL/"taken from" 검색 결과 없음.
- VMA: 헤더 한 개, MIT. GPL 문구 없음. 구현은 `src/TerminalViewer/Algorithm/TerminalRenderer/Strategy/Vulkan/VmaImplementation.cpp` 한 곳에서 만든다.
- MoltenVK: 미리 만든 dylib 을 그대로 앱 번들 `Contents/Frameworks/libMoltenVK.dylib` 에 넣는다(쓰는 쪽이 `TERMINALVIEWER_MOLTENVK_DYLIB` 로 복사한다). 동적으로 연결한다(앱이 `dlopen`/로더로 불러옴). v1.4.2 `ExternalRevisions/` 기준 정적으로 들어간 코드와 고지 의무:
  - SPIRV-Cross `6c09849` (Apache-2.0 OR MIT, Arm), SPIRV-Tools `0d6fd73` (Apache-2.0, NOTICE 파일 없음), SPIRV-Headers `29981f6` (Khronos MIT 계열), Vulkan-Headers `e3b1eec` (Apache-2.0 OR MIT), cereal `a56bad8` (BSD-3-Clause: 바이너리 배포에 저작권·조건·면책 문구를 문서로 동봉).
  - volk, Vulkan-Tools 는 MoltenVK 의 Cube 데모 전용이라 dylib 에 들어가지 않는다(`ExternalRevisions/README.md`). glslang 은 목록에 없고 dylib 의 문자열에도 SPIR-V 생성기 이름뿐이다.
  - dylib 의 `strings` 에 GPL/GNU General 문구는 없다. 확인한 것: LICENSE(Apache-2.0), 위 저장소들의 LICENSE, 동적 의존성(시스템 프레임워크뿐). 확인하지 못한 것: 바이너리 안의 개별 오브젝트 단위 구성(소스 목록과 문자열로만 대조), MoltenVK 가 내부에서 쓰는 다른 헤더 인용.
  - 서명: 릴리스의 dylib 은 ad-hoc 서명이다. 앱 번들에 넣은 뒤 `codesign --force --deep -s - <앱>.app` 또는 배포 서명으로 다시 서명한다.

### 셰이더

- `resources/shaders/terminal.vert`, `terminal.frag` 는 이 저장소의 코드(Ocean 에서 옮김)다. 서드파티가 아니다.
- 만든 SPIR-V(`*.spv`)를 저장소에 함께 둔다. 일반 빌드는 glslc 가 필요 없다. 원본을 고치면 `cmake -DGLSLC=<glslc 경로> -P tools/shaders/BuildShaders.cmake` 로 다시 만든다(glslc 는 Vulkan SDK 또는 shaderc 의 도구로, 빌드 결과물에 들어가지 않는다).

## Cascadia Mono (저장소에 남긴 글꼴, 테스트만)

- 원본: https://github.com/microsoft/cascadia-code 릴리스 `v2407.24` 의 `CascadiaCode-2407.24.zip`(zip SHA256 `e67a68ee3386db63f48b9054bd196ea752bc6a4ebb4df35adce6733da50c8474`) 안
  `ttf/static/CascadiaMono-{Regular,Bold,Italic,BoldItalic}.ttf`. 글꼴 버전은 모두 `2407.024` 다. 공식 배포가 150MB zip 뿐이라 내려받지 않고 저장소에 둔다.
- 파일 SHA256: Regular `06520d032ec274fa5040b22c6f4a1d829081b24ba40b2da56dae89bf10c7b481`, Bold `b22cb603ed23cac36e8444846e1841caca21719a5e852780a8d37ae7a49b0a36`,
  Italic `8d98a33e7617c14dd1d200b4900bc6c2d5d7165abb5c09e9d2fd51c38ce9c9ff`, BoldItalic `ecaeab55e6334408f8ff1fde78a99ddaeb3bb7fad4fcd177df8475c88a7d240f`
  (Ocean `resources/fonts/` 와 바이트까지 같다).
- 글꼴 이름 테이블: 저작권 `© 2021 Microsoft Corporation`, 라이선스 설명 nameID 13, URL nameID 14 `https://scripts.sil.org/OFL`. 네 파일이 글자까지 같다.
- 라이선스: SIL OFL 1.1 (`tests/fonts/OFL.txt`). 원본 그대로 쓴다(수정·이름 변경 없음).
- **테스트 실행 파일에만 들어간다**: `tests/fonts/TestFonts.qrc`(`:/terminalviewer/testfonts/`)를 `TerminalViewerTests` 에 넣고 `TestFonts::Register()`(`tests/TestFonts.cpp`)가 한 번만 등록한다.
  라이브러리 `TerminalViewer` 와 `TerminalViewerTestSupport` 에는 넣지 않는다. 터미널 글꼴을 쓰는 앱(Ocean)은 자기 글꼴을 직접 넣고 고지한다(Ocean 은 `:/fonts/` 와 `LICENSE.CascadiaMono-OFL`).
