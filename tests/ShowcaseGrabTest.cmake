# TerminalViewer: 예제 TerminalShowcase 를 끝에서 끝까지 시험한다. CPU 로 그린 첫 프레임을 PNG 로 남기고 0 으로 끝나야 한다.
# 기본 실행(GRAB_FILE)과 --min-contrast 4.5 실행(<GRAB_FILE 이름>-contrast.png) 두 번 돈다.
# 사용: cmake -DSHOWCASE_EXE=<TerminalShowcase 실행 파일> -DGRAB_FILE=<남길 .png> -P ShowcaseGrabTest.cmake   (CTest: TerminalShowcaseGrabTest)
#       손으로 돌리면 QT_QPA_PLATFORM=offscreen 을 정하고, Windows 에서는 Qt · MinGW bin 폴더를 PATH 에 두고
#       QT_QPA_FONTDIR=C:/Windows/Fonts 를 정한다(CTest 는 CMakeLists.txt 가 정한다).
cmake_minimum_required(VERSION 3.24)

if(NOT SHOWCASE_EXE OR NOT GRAB_FILE)
    message(FATAL_ERROR "usage: cmake -DSHOWCASE_EXE=<exe> -DGRAB_FILE=<png> -P ShowcaseGrabTest.cmake")
endif()

# 잡아서 grabFile 에 PNG 를 남기고 0 으로 끝나는지 본다. ARGN 은 더 줄 인자
function(terminalviewer_expect_grab grabFile)
    # 지난 실행이 남긴 파일로 통과하지 않게 먼저 지운다.
    file(REMOVE "${grabFile}")
    execute_process(COMMAND "${SHOWCASE_EXE}" --renderer cpu ${ARGN} --grab "${grabFile}"
                    RESULT_VARIABLE exitCode OUTPUT_VARIABLE log ERROR_VARIABLE log ENCODING UTF-8 TIMEOUT 60)
    if(NOT exitCode EQUAL 0)
        message(FATAL_ERROR "TerminalShowcaseGrabTest: exit ${exitCode} (want 0): ${log}")
    endif()
    if(NOT EXISTS "${grabFile}")
        message(FATAL_ERROR "TerminalShowcaseGrabTest: ${grabFile} was not written: ${log}")
    endif()
    # PNG 서명(89 50 4E 47 0D 0A 1A 0A)
    file(READ "${grabFile}" signature LIMIT 8 HEX)
    if(NOT signature STREQUAL "89504e470d0a1a0a")
        message(FATAL_ERROR "TerminalShowcaseGrabTest: ${grabFile} is not a PNG (${signature})")
    endif()
    string(JOIN " " extraArgs ${ARGN})
    message(STATUS "PASS TerminalShowcaseGrabTest ${extraArgs}: ${log}")
endfunction()

terminalviewer_expect_grab("${GRAB_FILE}")
# 최소 대비를 켠 실행(--min-contrast 의 맞는 값을 받아들이는지): <이름>-contrast.png
string(REGEX REPLACE "\\.png$" "" grabStem "${GRAB_FILE}")
terminalviewer_expect_grab("${grabStem}-contrast.png" --min-contrast 4.5)
