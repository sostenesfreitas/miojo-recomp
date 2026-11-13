cmake_minimum_required(VERSION 3.20)
project(f2i_immediate LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT DEFINED REPO_ROOT OR NOT DEFINED SPIRV_HEADERS_ROOT)
    message(FATAL_ERROR "REPO_ROOT and SPIRV_HEADERS_ROOT are required")
endif()
file(TO_CMAKE_PATH "${REPO_ROOT}" REPO_ROOT)
file(TO_CMAKE_PATH "${SPIRV_HEADERS_ROOT}" SPIRV_HEADERS_ROOT)
set(SHADER "${REPO_ROOT}/gpu/vk/eden/shader_recompiler")
file(GLOB_RECURSE BACKEND_SOURCES CONFIGURE_DEPENDS "${SHADER}/backend/spirv/*.cpp")
file(GLOB_RECURSE IR_SOURCES CONFIGURE_DEPENDS "${SHADER}/frontend/ir/*.cpp")
file(GLOB_RECURSE OPTIMIZATION_SOURCES CONFIGURE_DEPENDS "${SHADER}/ir_opt/*.cpp")
file(GLOB_RECURSE SIRIT_SOURCES CONFIGURE_DEPENDS "${REPO_ROOT}/gpu/vk/sirit/src/*.cpp")
set(IMPL "${SHADER}/frontend/maxwell/translate/impl")
add_executable(f2i_immediate
    "${REPO_ROOT}/tests/f2i_immediate/fixture.cpp"
    "${SHADER}/frontend/maxwell/decode.cpp"
    "${SHADER}/frontend/maxwell/opcodes.cpp" "${IMPL}/common_funcs.cpp"
    "${IMPL}/impl.cpp" "${IMPL}/register_map_nx2.cpp" "${IMPL}/move_register.cpp"
    "${IMPL}/floating_point_conversion_integer.cpp" "${IMPL}/integer_floating_point_conversion.cpp"
    "${IMPL}/floating_point_compare_and_set.cpp" "${IMPL}/floating_point_min_max.cpp"
    "${IMPL}/logic_operation.cpp"
    ${BACKEND_SOURCES} ${IR_SOURCES} ${OPTIMIZATION_SOURCES} ${SIRIT_SOURCES})
target_include_directories(f2i_immediate PRIVATE
    "${REPO_ROOT}/gpu/vk/compat" "${REPO_ROOT}/gpu/compat"
    "${REPO_ROOT}/gpu/vk/eden" "${REPO_ROOT}/gpu/third_party/suyu"
    "${REPO_ROOT}/gpu/vk/sirit/include" "${REPO_ROOT}/gpu/vk/sirit/src"
    "${SPIRV_HEADERS_ROOT}/include")
target_compile_definitions(f2i_immediate PRIVATE NDEBUG MKWVK_INCLUDES=2 VK_NO_PROTOTYPES)
if(MSVC)
    target_compile_options(f2i_immediate PRIVATE /O1 /EHsc)
else()
    target_compile_options(f2i_immediate PRIVATE -O1)
endif()
