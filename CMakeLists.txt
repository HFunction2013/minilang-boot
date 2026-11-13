cmake_minimum_required(VERSION 3.15)
project(minilang C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Boot compiler sources. runtime.c *is* linked in: `run -e` dlopen's user
# extensions that call ml_* helpers, and the executable must provide those
# symbols (on Windows there is no --export-dynamic equivalent).
set(SOURCES
    value.c
    lexer.c
    parser.c
    bytecode.c
    vm.c
    c_gen.c
    milc_io.c
    native.c
    runtime.c
    main.c
)

add_executable(minilang ${SOURCES})

# Export the ml_* runtime helpers so a prebuilt extension DLL can resolve them
# against this executable (MINI_API in minilang_runtime.h maps to dllexport).
target_compile_definitions(minilang PRIVATE MINILANG_EXPORTS)

# MSVC-specific: disable warnings about CRT deprecation (strdup etc. are
# mapped to _strdup in minilang.h, but MSVC still warns).
if(MSVC)
    target_compile_definitions(minilang PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(minilang PRIVATE /W3)
endif()

# On non-Windows, link math library for sqrt etc.
if(NOT WIN32)
    target_link_libraries(minilang PRIVATE m)
endif()
