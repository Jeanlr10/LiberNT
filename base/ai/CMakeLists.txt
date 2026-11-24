# LiberNT AI Subsystem

include_directories(
    include
    store
    ${REACTOS_SOURCE_DIR}/sdk/lib/3rdparty/sqlite)

# Portable SQLite FTS5 + embedding store (static lib linked into aisvc)
add_library(ai_store STATIC
    store/ai_store.c
    ${REACTOS_SOURCE_DIR}/sdk/lib/3rdparty/sqlite/sqlite3.c)
target_compile_definitions(ai_store PRIVATE
    SQLITE_OS_WIN=1
    SQLITE_ENABLE_FTS5=1
    SQLITE_THREADSAFE=1)
if(NOT MSVC)
    set_source_files_properties(
        ${REACTOS_SOURCE_DIR}/sdk/lib/3rdparty/sqlite/sqlite3.c
        PROPERTIES COMPILE_FLAGS "-w")
endif()

# libai.dll - ALPC client SDK + NTFS ADS folder description helpers
spec2def(libai.dll libai/libai.spec ADD_IMPORTLIB)
add_library(libai MODULE
    libai/libai.c
    ${CMAKE_CURRENT_BINARY_DIR}/libai.def)
set_module_type(libai win32dll UNICODE)
add_importlibs(libai msvcrt kernel32 ntdll)
add_cd_file(TARGET libai DESTINATION reactos/system32 FOR all)

# aisvc.exe - Session 0 NT SCM service + ALPC broker + live NTFS indexer
add_executable(aisvc
    aisvc/main.c
    aisvc/indexer.c)
set_module_type(aisvc win32cui UNICODE)
target_link_libraries(aisvc ai_store)
add_importlibs(aisvc libai advapi32 msvcrt kernel32 ntdll)
add_cd_file(TARGET aisvc DESTINATION reactos/system32 FOR all)
add_registry_inf(aisvc/aisvc_reg.inf)

# ai.exe - Command-line interface for semantic search, indexing, and folder descriptions
add_executable(ai ai_cli/main.c)
set_module_type(ai win32cui UNICODE)
add_importlibs(ai libai msvcrt kernel32 ntdll)
add_cd_file(TARGET ai DESTINATION reactos/system32 FOR all)
