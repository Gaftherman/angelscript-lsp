# ── AngelScript Compiler ──────────────────────────────────────────────────
FetchContent_Declare(angelscript_repo GIT_REPOSITORY https://github.com/anjo76/angelscript.git GIT_TAG master)
FetchContent_MakeAvailable(angelscript_repo)

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
