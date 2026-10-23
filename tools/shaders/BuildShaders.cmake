# TerminalViewer: 터미널 셰이더(resources/shaders/*.vert, *.frag)를 SPIR-V 로 다시 만들고 원본 해시 목록(shaders.sha256)을 새로 쓴다.
#   셰이더 원본을 고친 뒤에만 돌린다. glslc(Vulkan SDK 또는 shaderc)가 필요하다. 만든 .spv 와 shaders.sha256 을 함께 커밋한다.
#   사용: cmake -DGLSLC=<glslc 경로> -P tools/shaders/BuildShaders.cmake
cmake_minimum_required(VERSION 3.24)

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/CheckShaders.cmake")   # TERMINALVIEWER_SHADER_SOURCES, terminalviewer_check_shaders

if(NOT GLSLC)
    message(FATAL_ERROR "glslc 경로를 준다: cmake -DGLSLC=<glslc 경로> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()
if(NOT EXISTS "${GLSLC}")
    message(FATAL_ERROR "glslc 가 없다: ${GLSLC}")
endif()

get_filename_component(shaderDir "${CMAKE_CURRENT_LIST_DIR}/../../resources/shaders" ABSOLUTE)
execute_process(COMMAND "${GLSLC}" --version OUTPUT_VARIABLE glslcVersion ERROR_VARIABLE glslcVersion)
string(REGEX REPLACE "\n.*" "" glslcVersion "${glslcVersion}")
message(STATUS "glslc: ${glslcVersion}")

set(hashList "")
foreach(name IN LISTS TERMINALVIEWER_SHADER_SOURCES)
    # SPIR-V 1.0(Vulkan 1.0 환경)으로 만든다. Vulkan 1.3 장치는 모두 읽는다.
    execute_process(COMMAND "${GLSLC}" -O -o "${shaderDir}/${name}.spv" "${shaderDir}/${name}"
                    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "glslc 가 ${name} 을 만들지 못했다:\n${output}")
    endif()
    file(SHA256 "${shaderDir}/${name}" hash)
    string(APPEND hashList "${hash}  ${name}\n")
    message(STATUS "built ${name}.spv")
endforeach()
file(WRITE "${shaderDir}/${TERMINALVIEWER_SHADER_HASH_FILE}" "${hashList}")
terminalviewer_check_shaders("${shaderDir}")
message(STATUS "wrote ${shaderDir}/${TERMINALVIEWER_SHADER_HASH_FILE}")
