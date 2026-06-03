cmake_minimum_required(VERSION 3.14)
project(AngelLSP VERSION 1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    enable_language(ASM_MASM)
endif()

include(FetchContent)

FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_MakeAvailable(json)

FetchContent_Declare(fmt GIT_REPOSITORY https://github.com/fmtlib/fmt.git GIT_TAG 10.2.1)
FetchContent_MakeAvailable(fmt)

FetchContent_Declare(angelscript_repo GIT_REPOSITORY https://github.com/anjo76/angelscript.git GIT_TAG master)

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_Populate(angelscript_repo)

file(GLOB AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/angelscript/source/*.cpp")
list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/add_on/scriptbuilder/scriptbuilder.cpp")
list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/add_on/scriptstdstring/scriptstdstring.cpp")
list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/add_on/scriptstdstring/scriptstdstring_utils.cpp")
list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/add_on/scriptarray/scriptarray.cpp")
list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/add_on/scriptdictionary/scriptdictionary.cpp")
list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/add_on/scriptmath/scriptmath.cpp")

if(MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND AS_SOURCES "${angelscript_repo_SOURCE_DIR}/sdk/angelscript/source/as_callfunc_x64_msvc_asm.asm")
endif()

add_library(AngelScriptLib STATIC ${AS_SOURCES})
target_include_directories(AngelScriptLib PUBLIC 
    "${angelscript_repo_SOURCE_DIR}/sdk/angelscript/include"
    "${angelscript_repo_SOURCE_DIR}/sdk/add_on"
)

add_executable(angel_lsp 
    src/main.cpp
    src/LspServer.cpp
    src/ScriptEngine.cpp
    src/TokenHarvester.cpp
)

target_include_directories(angel_lsp PRIVATE include)

target_link_libraries(angel_lsp PRIVATE 
    nlohmann_json::nlohmann_json 
    AngelScriptLib
    fmt::fmt
)