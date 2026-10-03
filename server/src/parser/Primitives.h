#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>

/**
 * @file
 * @brief The primitive types, once, with every subset derived from the one list.
 *
 * There were five copies of this vocabulary - in SemanticHelpers.h, utils/Utils.cpp,
 * ControlFlowChecker.cpp, TypeConversionChecker.cpp and CompletionHandler.cpp - and unlike the
 * three keyword lists that preceded them here, they did NOT disagree. Each was a correct subset of
 * the same fourteen names: all of them, all but `void`, the numeric ones, the integers, the two
 * floats. Nothing was broken; there were simply five places to edit when a type is added, and no
 * way to see that the "all but void" list and the "all" list were meant to be related.
 *
 * So the subsets are spelled out as what they are - a filter over one base - rather than as five
 * hand-written lists that happen to agree today.
 */
namespace angel_lsp::parser::primitives
{
/** @brief The integers, signed and unsigned. `int32`/`uint32` are the explicit spellings of
 *         `int`/`uint`; the parser returns whichever the source wrote, so both are listed. */
inline constexpr std::array<std::string_view, 10> k_integers = {
    "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64",
};

/** @brief The floating point types. */
inline constexpr std::array<std::string_view, 2> k_floats = {"float", "double"};

/** @brief Everything that carries a number. */
inline constexpr std::array<std::string_view, 12> k_numeric = {
    "int", "int8", "int16", "int32", "int64", "uint", "uint8", "uint16", "uint32", "uint64", "float", "double",
};

/** @brief Every primitive the VM has, `void` included. */
inline constexpr std::array<std::string_view, 14> k_all = {
    "int",    "int8",   "int16",  "int32", "int64",  "uint", "uint8",
    "uint16", "uint32", "uint64", "float", "double", "bool", "void",
};

[[nodiscard]] constexpr bool IsInteger(std::string_view name) noexcept
{
    return std::find(k_integers.begin(), k_integers.end(), name) != k_integers.end();
}

[[nodiscard]] constexpr bool IsFloatingPoint(std::string_view name) noexcept
{
    return std::find(k_floats.begin(), k_floats.end(), name) != k_floats.end();
}

/** @brief A number, so never a bool and never a condition on its own. */
[[nodiscard]] constexpr bool IsNumeric(std::string_view name) noexcept
{
    return std::find(k_numeric.begin(), k_numeric.end(), name) != k_numeric.end();
}

/** @brief Any primitive at all. */
[[nodiscard]] constexpr bool IsPrimitive(std::string_view name) noexcept
{
    return std::find(k_all.begin(), k_all.end(), name) != k_all.end();
}

/**
 * @brief Documentation metadata and value ranges for primitive types.
 */
struct PrimitiveDocInfo
{
    std::string_view name;
    std::string_view description;
    std::string_view range;
};

inline constexpr std::array<PrimitiveDocInfo, 14> k_primitiveDocTable = {{
    {"int8", "8-bit signed integer", "-128 to 127"},
    {"uint8", "8-bit unsigned integer", "0 to 255 (0x00 to 0xFF)"},
    {"int16", "16-bit signed integer", "-32,768 to 32,767"},
    {"uint16", "16-bit unsigned integer", "0 to 65,535 (0x0000 to 0xFFFF)"},
    {"int", "32-bit signed integer", "-2,147,483,648 to 2,147,483,647"},
    {"int32", "32-bit signed integer", "-2,147,483,648 to 2,147,483,647"},
    {"uint", "32-bit unsigned integer", "0 to 4,294,967,295 (0x0 to 0xFFFFFFFF)"},
    {"uint32", "32-bit unsigned integer", "0 to 4,294,967,295 (0x0 to 0xFFFFFFFF)"},
    {"int64", "64-bit signed integer", "-9,223,372,036,854,775,808 to 9,223,372,036,854,775,807"},
    {"uint64", "64-bit unsigned integer", "0 to 18,446,744,073,709,551,615 (0x0 to 0xFFFFFFFFFFFFFFFF)"},
    {"float", "32-bit single-precision floating-point (IEEE 754)", "\u00B11.17549435e-38 to \u00B13.40282347e+38"},
    {"double", "64-bit double-precision floating-point (IEEE 754)",
     "\u00B12.2250738585072014e-308 to \u00B11.7976931348623157e+308"},
    {"bool", "Boolean type", "true or false"},
    {"void", "Absence of type / value", ""},
}};

/**
 * @brief Retrieves documentation description and range for a primitive type.
 * @param name Primitive type name.
 * @return PrimitiveDocInfo if matched, std::nullopt otherwise.
 */
[[nodiscard]] constexpr std::optional<PrimitiveDocInfo> GetPrimitiveDocInfo(std::string_view name) noexcept
{
    for (const auto& item : k_primitiveDocTable)
    {
        if (item.name == name)
        {
            return item;
        }
    }
    return std::nullopt;
}

/**
 * @brief A primitive that can hold a value, so `null` can never be assigned to it.
 *
 * Everything but `void`, which holds nothing at all - the distinction ControlFlowChecker draws
 * when it decides whether returning `null` is a type error.
 */
[[nodiscard]] constexpr bool IsNonNullable(std::string_view name) noexcept
{
    return IsPrimitive(name) && name != "void";
}
} // namespace angel_lsp::parser::primitives
