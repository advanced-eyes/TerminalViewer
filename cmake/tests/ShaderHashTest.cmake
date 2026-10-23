# TerminalViewer: cmake/CheckShaders.cmake 가 셰이더 원본이 한 글자만 바뀌어도 구성을 멈추는지 시험한다.
# 사용: cmake -DCHECK_SCRIPT=<CheckShaders.cmake> -DSHADER_DIR=<resources/shaders> -DWORK_DIR=<임시 폴더> -P ShaderHashTest.cmake
#       (CTest: TerminalViewerShaderHashTest)
cmake_minimum_required(VERSION 3.24)
include("${CHECK_SCRIPT}")   # TERMINALVIEWER_SHADER_SOURCES, TERMINALVIEWER_SHADER_HASH_FILE

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")
set_property(GLOBAL PROPERTY TERMINALVIEWER_SHADER_TEST_FAILURES "")

# 저장소의 셰이더 폴더(원본, .spv, 해시 목록)를 caseName 폴더로 복사한다.
function(terminalviewer_shader_copy caseName outDir)
    set(dir "${WORK_DIR}/${caseName}")
    file(MAKE_DIRECTORY "${dir}")
    foreach(name IN LISTS TERMINALVIEWER_SHADER_SOURCES)
        file(COPY "${SHADER_DIR}/${name}" "${SHADER_DIR}/${name}.spv" DESTINATION "${dir}")
    endforeach()
    file(COPY "${SHADER_DIR}/${TERMINALVIEWER_SHADER_HASH_FILE}" DESTINATION "${dir}")
    set(${outDir} "${dir}" PARENT_SCOPE)
endfunction()

# 검사를 별도 cmake -P 프로세스로 돌린다(FATAL_ERROR 는 그 프로세스만 멈춘다). 출력의 공백·줄바꿈은 한 칸으로 줄인다.
function(terminalviewer_shader_check dir outExit outLog)
    # ENCODING UTF-8: Windows 의 기본값(AUTO: 콘솔 또는 ANSI 코드 페이지)으로 읽으면 자식 cmake 의 UTF-8 출력(한국어 안내문)이 깨진다.
    # 다른 플랫폼은 ENCODING 을 무시한다.
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DSHADER_DIR=${dir}" -P "${CHECK_SCRIPT}"
                    RESULT_VARIABLE exitCode OUTPUT_VARIABLE log ERROR_VARIABLE log ENCODING UTF-8)
    string(REGEX REPLACE "[ \t\r\n]+" " " log "${log}")
    set(${outExit} "${exitCode}" PARENT_SCOPE)
    set(${outLog} "${log}" PARENT_SCOPE)
endfunction()

function(terminalviewer_shader_report caseName passed detail)
    if(passed)
        message(STATUS "PASS ${caseName}")
    else()
        message(STATUS "FAIL ${caseName}: ${detail}")
        set_property(GLOBAL APPEND PROPERTY TERMINALVIEWER_SHADER_TEST_FAILURES "${caseName}")
    endif()
endfunction()

# 멈춰야 하는 경우: 종료 코드가 0 이 아니고 안내문(다시 만드는 방법)을 낸다.
function(terminalviewer_shader_expect_stop caseName dir)
    terminalviewer_shader_check("${dir}" exitCode log)
    set(passed FALSE)
    if(NOT exitCode EQUAL 0)
        string(FIND "${log}" "셰이더 원본이 바뀌었다." messageAt)
        string(FIND "${log}" "tools/shaders/BuildShaders.cmake" toolAt)
        if(messageAt GREATER_EQUAL 0 AND toolAt GREATER_EQUAL 0)
            set(passed TRUE)
        endif()
    endif()
    terminalviewer_shader_report("${caseName}" ${passed} "exit ${exitCode}: ${log}")
endfunction()

# 1) 저장소 그대로면 통과한다(커밋한 해시 목록이 원본과 맞다).
terminalviewer_shader_copy(unchanged dir)
terminalviewer_shader_check("${dir}" exitCode log)
if(exitCode EQUAL 0)
    terminalviewer_shader_report(unchangedPasses TRUE "")
else()
    terminalviewer_shader_report(unchangedPasses FALSE "exit ${exitCode}: ${log}")
endif()

# 2) 원본마다 한 글자를 바꾸면 멈춘다("main" 의 마지막 글자 n → x, 길이는 그대로).
foreach(name IN LISTS TERMINALVIEWER_SHADER_SOURCES)
    terminalviewer_shader_copy("changed-${name}" dir)
    file(READ "${dir}/${name}" content)
    string(FIND "${content}" "main" mainAt)
    if(mainAt LESS 0)
        terminalviewer_shader_report("oneCharacterChanged-${name}" FALSE "no 'main' in ${name}")
        continue()
    endif()
    math(EXPR changeAt "${mainAt} + 3")
    math(EXPR tailAt "${changeAt} + 1")
    string(SUBSTRING "${content}" 0 ${changeAt} head)
    string(SUBSTRING "${content}" ${tailAt} -1 tail)
    set(changed "${head}x${tail}")
    string(LENGTH "${content}" lengthBefore)
    string(LENGTH "${changed}" lengthAfter)
    if(NOT lengthBefore EQUAL lengthAfter OR changed STREQUAL content)
        terminalviewer_shader_report("oneCharacterChanged-${name}" FALSE "setup: change was not a single character")
        continue()
    endif()
    file(WRITE "${dir}/${name}" "${changed}")
    terminalviewer_shader_expect_stop("oneCharacterChanged-${name}" "${dir}")
endforeach()

# 3) 해시 목록이 없으면 멈춘다.
terminalviewer_shader_copy(missingList dir)
file(REMOVE "${dir}/${TERMINALVIEWER_SHADER_HASH_FILE}")
terminalviewer_shader_expect_stop(missingHashList "${dir}")

get_property(failures GLOBAL PROPERTY TERMINALVIEWER_SHADER_TEST_FAILURES)
list(LENGTH failures failureCount)
if(failureCount GREATER 0)
    message(FATAL_ERROR "TerminalViewerShaderHashTest: ${failureCount} failed: ${failures}")
endif()
message(STATUS "TerminalViewerShaderHashTest: all passed")
