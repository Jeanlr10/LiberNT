# SPDX-License-Identifier: GPL-3.0-or-later
#
# Optional llama.cpp nested cross-build producing a self-contained llama.dll
# against LiberNT's ReactOS SDK + libc++ + ucrtbase runtime.

option(ENABLE_LIBERNT_AI "Build LiberNT AI subsystem" ON)
option(ENABLE_LIBERNT_AI_LLAMACPP "Build llama.dll from submodules/llama.cpp" ON)
if(NOT ENABLE_LIBERNT_AI OR NOT ENABLE_LIBERNT_AI_LLAMACPP)
    return()
endif()

set(LLAMACPP_SOURCE_DIR "${REACTOS_SOURCE_DIR}/submodules/llama.cpp")
if(NOT EXISTS "${LLAMACPP_SOURCE_DIR}/CMakeLists.txt")
    message(STATUS "llama.cpp: submodules/llama.cpp not checked out; aisvc will use built-in GGUF engine.")
    return()
endif()

if(ARCH STREQUAL "amd64")
    set(_llama_sdk_arch x86_64)
    set(_llama_triple x86_64-w64-mingw32)
    set(_llama_arch_flags -mlong-double-64)
    set(_llama_simd_args -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON)
elseif(ARCH STREQUAL "arm64")
    set(_llama_sdk_arch aarch64)
    set(_llama_triple aarch64-w64-mingw32)
    set(_llama_arch_flags "")
    set(_llama_simd_args "")
else()
    message(STATUS "llama.cpp: skipping on ARCH ${ARCH}")
    return()
endif()

find_program(LLAMACPP_NINJA NAMES ninja REQUIRED)
find_program(LLAMACPP_STRIP NAMES llvm-strip HINTS "${REACTOS_CLANG_LLVM_MINGW_ROOT}/bin" NO_CACHE REQUIRED)

include("${REACTOS_SOURCE_DIR}/sdk/cmake/nested-build.cmake")
include(ExternalProject)

set(LLAMACPP_WORK_DIR "${REACTOS_BINARY_DIR}/submodules/llama-cpp")
set(LLAMACPP_SDK_LIBDIR "${LLAMACPP_WORK_DIR}/reactos-sdk-lib")
set(LLAMACPP_BUILD_DIR "${LLAMACPP_WORK_DIR}/build")
set(LLAMACPP_BUILD_DLL "${LLAMACPP_BUILD_DIR}/llama.dll")
set(LLAMACPP_DLL "${LLAMACPP_WORK_DIR}/llama.dll")

set(_llama_sdk_targets
    msvcrtex libucrtbase libmsvcrt libkernel32 libntdll ucrtoldnames libuser32 libgdi32 libwinspool
    libshell32 libole32 liboleaut32 uuid libcomdlg32 libadvapi32 libpsapi)
export_sdk_libraries(llamacpp_sdk_libs "${LLAMACPP_SDK_LIBDIR}"
    LIBRARIES ${_llama_sdk_targets}
    DEPENDS ${_llama_sdk_targets} ucrtoldnames_target)
string(JOIN " " _llama_sdk_arch_flags ${_llama_arch_flags})

ExternalProject_Add(llamacpp-build
    EXCLUDE_FROM_ALL TRUE
    PREFIX "${LLAMACPP_WORK_DIR}/prefix"
    SOURCE_DIR "${REACTOS_SOURCE_DIR}/base/ai/llama_bridge"
    BINARY_DIR "${LLAMACPP_BUILD_DIR}"
    DOWNLOAD_COMMAND ""
    UPDATE_COMMAND ""
    PATCH_COMMAND ""
    CMAKE_GENERATOR Ninja
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_TRY_COMPILE_TARGET_TYPE:STRING=EXECUTABLE
        -DCMAKE_MAKE_PROGRAM:FILEPATH=${LLAMACPP_NINJA}
        -DCMAKE_C_COMPILER_LAUNCHER=
        -DCMAKE_CXX_COMPILER_LAUNCHER=
        -DCMAKE_TOOLCHAIN_FILE:FILEPATH=${REACTOS_SOURCE_DIR}/submodules/reactos-sdk.cmake
        -DREACTOS_CLANG_LLVM_MINGW_ROOT:PATH=${REACTOS_CLANG_LLVM_MINGW_ROOT}
        -DREACTOS_SDK_ARCH:STRING=${_llama_sdk_arch}
        -DREACTOS_SDK_TRIPLE:STRING=${_llama_triple}
        "-DREACTOS_SDK_ARCH_FLAGS:STRING=${_llama_sdk_arch_flags}"
        -DREACTOS_SDK_SOURCE_DIR:PATH=${REACTOS_SOURCE_DIR}
        -DREACTOS_SDK_BUILD_DIR:PATH=${REACTOS_BINARY_DIR}
        -DREACTOS_SDK_LIBDIR:PATH=${LLAMACPP_SDK_LIBDIR}
        -DREACTOS_SDK_CXX_RUNTIME:PATH=${REACTOS_LIBCXX_ROOT}
        -DLLAMA_CPP_SOURCE_DIR:PATH=${LLAMACPP_SOURCE_DIR}
        ${_llama_simd_args}
    BUILD_COMMAND ${CMAKE_COMMAND} -E env CCACHE_DISABLE=1 SCCACHE_DISABLE=1
        ${REACTOS_NESTED_BUILD} <BINARY_DIR> --target llama_dll
    INSTALL_COMMAND ""
    BUILD_BYPRODUCTS "${LLAMACPP_BUILD_DLL}"
    USES_TERMINAL_CONFIGURE TRUE
    USES_TERMINAL_BUILD TRUE)
ExternalProject_Add_StepDependencies(llamacpp-build configure llamacpp_sdk_libs)

add_custom_command(
    OUTPUT "${LLAMACPP_DLL}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${LLAMACPP_BUILD_DLL}" "${LLAMACPP_DLL}"
    COMMAND ${LLAMACPP_STRIP} --strip-debug "${LLAMACPP_DLL}"
    DEPENDS llamacpp-build "${LLAMACPP_BUILD_DLL}"
    VERBATIM)
add_custom_target(libernt_llama_dll DEPENDS "${LLAMACPP_DLL}")
add_cd_file(FILE "${LLAMACPP_DLL}" TARGET libernt_llama_dll DESTINATION reactos/system32 FOR all)
