# TerminalViewer: 예제 TerminalShowcase 의 명령행 처리를 시험한다. 잘못된 인자는 모두 2 로 끝나야 한다
# (1 은 --grab 의 "시간 안에 프레임 없음"이라 구분할 수 없게 된다). --help 류는 도움말을 찍고 0 으로 끝난다.
# 사용: cmake -DSHOWCASE_EXE=<TerminalShowcase 실행 파일> -P ShowcaseArgsTest.cmake   (CTest: TerminalShowcaseArgsTest)
#       손으로 돌리면 QT_QPA_PLATFORM=offscreen 을 정하고, Windows 에서는 Qt · MinGW bin 폴더를 PATH 에 둔다
#       (CTest 는 CMakeLists.txt 가 정한다).
cmake_minimum_required(VERSION 3.24)

if(NOT SHOWCASE_EXE)
    message(FATAL_ERROR "usage: cmake -DSHOWCASE_EXE=<exe> -P ShowcaseArgsTest.cmake")
endif()

set_property(GLOBAL PROPERTY TERMINALVIEWER_SHOWCASE_ARGS_FAILURES "")

# 인자를 주고 돌려 종료 코드가 expectedExit 이고 출력(표준 출력 + 표준 오류)에 expectedText 가 있는지 본다.
# 창을 띄우는 경우는 없다: 잘못된 인자와 도움말은 창을 만들기 전에 끝난다.
function(terminalviewer_expect_exit caseName expectedExit expectedText)
    execute_process(COMMAND "${SHOWCASE_EXE}" ${ARGN}
                    RESULT_VARIABLE exitCode OUTPUT_VARIABLE log ERROR_VARIABLE log ENCODING UTF-8 TIMEOUT 30)
    set(passed FALSE)
    if("${exitCode}" STREQUAL "${expectedExit}")
        string(FIND "${log}" "${expectedText}" textAt)
        if(textAt GREATER_EQUAL 0)
            set(passed TRUE)
        endif()
    endif()
    if(passed)
        message(STATUS "PASS ${caseName}")
    else()
        message(STATUS "FAIL ${caseName}: exit ${exitCode} (want ${expectedExit}, text '${expectedText}'): ${log}")
        set_property(GLOBAL APPEND PROPERTY TERMINALVIEWER_SHOWCASE_ARGS_FAILURES "${caseName}")
    endif()
endfunction()

# 1) QCommandLineParser 가 거르는 오류(모르는 옵션, 값이 빠진 옵션)
terminalviewer_expect_exit(unknownOption 2 "bogus" --bogus)
terminalviewer_expect_exit(missingValue 2 "font-px" --font-px)
terminalviewer_expect_exit(minContrastMissingValue 2 "min-contrast" --min-contrast)

# 2) 값 검사 오류(이미 2 로 끝나던 경우)
terminalviewer_expect_exit(outOfRange 2 "--font-px must be" --font-px 3)
terminalviewer_expect_exit(notANumber 2 "--cols must be" --cols x)
terminalviewer_expect_exit(badRenderer 2 "--renderer must be" --renderer gpu)
terminalviewer_expect_exit(positional 2 "unexpected argument" extra)

# 최소 대비는 1..21(WCAG 대비비). 범위 밖 · 숫자 아님 · nan 은 2
terminalviewer_expect_exit(minContrastTooLow 2 "--min-contrast must be 1..21" --min-contrast 0.5)
terminalviewer_expect_exit(minContrastTooHigh 2 "--min-contrast must be 1..21" --min-contrast 21.5)
terminalviewer_expect_exit(minContrastNotANumber 2 "--min-contrast must be 1..21" --min-contrast abc)
terminalviewer_expect_exit(minContrastNan 2 "--min-contrast must be 1..21" --min-contrast nan)

# 3) 도움말은 0 으로 끝난다
terminalviewer_expect_exit(help 0 "--grab <file.png>" --help)
terminalviewer_expect_exit(helpShort 0 "--grab <file.png>" -h)
terminalviewer_expect_exit(helpAll 0 "--grab <file.png>" --help-all)
terminalviewer_expect_exit(helpMinContrast 0 "--min-contrast <R>" --help)

get_property(failures GLOBAL PROPERTY TERMINALVIEWER_SHOWCASE_ARGS_FAILURES)
if(failures)
    message(FATAL_ERROR "TerminalShowcaseArgsTest failed: ${failures}")
endif()
