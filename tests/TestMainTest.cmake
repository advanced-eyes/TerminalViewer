# TerminalViewer: TerminalViewerTests(tests/TestMain.cpp)가 첫 인자(클래스 이름)를 어떻게 다루는지 시험한다.
# 사용: cmake -DTEST_EXE=<TerminalViewerTests 실행 파일> -P TestMainTest.cmake   (CTest: TerminalViewerTestMainTest)
#       손으로 돌리면 QT_QPA_PLATFORM=offscreen 을 정하고, Windows 에서는 Qt bin 폴더를 PATH 에 두고 QT_FORCE_STDERR_LOGGING=1 을 정한다
#       (CTest 는 CMakeLists.txt 가 정한다).
cmake_minimum_required(VERSION 3.24)

set_property(GLOBAL PROPERTY TERMINALVIEWER_TEST_MAIN_FAILURES "")

# 실행 파일을 인자와 함께 돌려 종료 코드와 출력(표준 출력과 표준 오류를 합친 것)을 받는다.
function(terminalviewer_run outExit outLog)
    execute_process(COMMAND "${TEST_EXE}" ${ARGN} RESULT_VARIABLE exitCode OUTPUT_VARIABLE log ERROR_VARIABLE log
                    ENCODING UTF-8)
    set(${outExit} "${exitCode}" PARENT_SCOPE)
    set(${outLog} "${log}" PARENT_SCOPE)
endfunction()

function(terminalviewer_report caseName passed detail)
    if(passed)
        message(STATUS "PASS ${caseName}")
    else()
        message(STATUS "FAIL ${caseName}: ${detail}")
        set_property(GLOBAL APPEND PROPERTY TERMINALVIEWER_TEST_MAIN_FAILURES "${caseName}")
    endif()
endfunction()

# 맞는 클래스가 없으면 아무것도 돌지 않는다. 통과로 보이지 않게 0 이 아닌 코드로 끝나고, 아는 클래스 이름을 알려 준다.
function(terminalviewer_expect_unknown caseName)
    terminalviewer_run(exitCode log ${ARGN})
    set(passed FALSE)
    if(NOT exitCode EQUAL 0)
        string(FIND "${log}" "Unknown test class" messageAt)
        string(FIND "${log}" "CellMetricsTest" firstClassAt)
        string(FIND "${log}" "TerminalSurfaceTest" lastClassAt)
        if(messageAt GREATER_EQUAL 0 AND firstClassAt GREATER_EQUAL 0 AND lastClassAt GREATER_EQUAL 0)
            set(passed TRUE)
        endif()
    endif()
    terminalviewer_report("${caseName}" ${passed} "exit ${exitCode}: ${log}")
endfunction()

# 1) 클래스 이름을 잘못 쓴 경우
terminalviewer_expect_unknown(misspelledClass CellMetricTest)

# 2) QTest 옵션을 클래스 이름보다 먼저 쓴 경우(-o 가 클래스 이름 자리에 온다)
terminalviewer_expect_unknown(optionFirst -o TestMainTest-output.txt,txt)

# 3) 맞는 이름이면 그 클래스만 돌고 0 으로 끝난다.
terminalviewer_run(exitCode log ChannelTest)
set(passed FALSE)
if(exitCode EQUAL 0)
    string(FIND "${log}" "Start testing of ChannelTest " startAt)
    string(FIND "${log}" "Totals:" totalsAt)
    string(FIND "${log}" "Start testing of CellMetricsTest " otherAt)
    if(startAt GREATER_EQUAL 0 AND totalsAt GREATER_EQUAL 0 AND otherAt EQUAL -1)
        set(passed TRUE)
    endif()
endif()
terminalviewer_report(knownClass ${passed} "exit ${exitCode}: ${log}")

get_property(failures GLOBAL PROPERTY TERMINALVIEWER_TEST_MAIN_FAILURES)
if(failures)
    message(FATAL_ERROR "TerminalViewerTestMainTest failed: ${failures}")
endif()
