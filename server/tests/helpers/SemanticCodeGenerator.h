#pragma once

#include <cstdint>
#include <random>
#include <string>

namespace angel_lsp::test
{

/**
 * @brief Metadata and source code for synthetic primitive function tests.
 */
struct PrimitiveScriptInfo
{
    std::string script;
    std::string funcName;
    std::string varName;
    std::string typeName;
    uint32_t varLine = 0;
    uint32_t varCol = 0;
};

/**
 * @brief Metadata and source code for multi-level class inheritance tests.
 */
struct InheritanceScriptInfo
{
    std::string script;
    std::string baseName;
    std::string derivedName;
    std::string leafName;
    std::string baseField;
    std::string baseMethod;
    std::string derivedField;
    std::string derivedMethod;
    std::string leafField;
    std::string leafMethod;
    std::string objVarName;
    uint32_t dotLine = 0;
    uint32_t dotCol = 0;
    uint32_t baseFieldHoverLine = 0;
    uint32_t baseFieldHoverCol = 0;
};

/**
 * @brief Metadata and source code for engine stubs integration tests.
 */
struct EngineScriptInfo
{
    std::string script;
    std::string funcName;
    std::string paramName;
    uint32_t playerHoverLine = 0;
    uint32_t playerHoverCol = 0;
    uint32_t playerDotLine = 0;
    uint32_t playerDotCol = 0;
};

/**
 * @brief Generates an AngelScript source snippet featuring functions returning primitives and local variables.
 * @param[in,out] rng Seeded pseudo-random number generator.
 * @return Populated PrimitiveScriptInfo with coordinates of target variable.
 */
PrimitiveScriptInfo GeneratePrimitiveFunctionsScript(std::mt19937_64& rng);

/**
 * @brief Generates an AngelScript source snippet featuring a 3-tier class inheritance hierarchy.
 * @param[in,out] rng Seeded pseudo-random number generator.
 * @return Populated InheritanceScriptInfo with member names and completion trigger coordinates.
 */
InheritanceScriptInfo GenerateInheritanceHierarchyScript(std::mt19937_64& rng);

/**
 * @brief Generates an AngelScript source snippet consuming Sven Co-op engine classes (CBasePlayer@, Vector, etc.).
 * @param[in,out] rng Seeded pseudo-random number generator.
 * @return Populated EngineScriptInfo with hover and dot completion coordinates.
 */
EngineScriptInfo GenerateEngineHookScript(std::mt19937_64& rng);

/**
 * @brief Generates a comprehensive multi-construct script suitable for random coordinate bombardment.
 * @param[in,out] rng Seeded pseudo-random number generator.
 * @return Valid multi-construct AngelScript source code.
 */
std::string GenerateFuzzBombardmentScript(std::mt19937_64& rng);

} // namespace angel_lsp::test
