#pragma once

#include <algorithm>
#include <array>
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
        "int", "int8", "int16", "int32", "int64",
        "uint", "uint8", "uint16", "uint32", "uint64",
    };

    /** @brief The floating point types. */
    inline constexpr std::array<std::string_view, 2> k_floats = { "float", "double" };

    /** @brief Everything that carries a number. */
    inline constexpr std::array<std::string_view, 12> k_numeric = {
        "int", "int8", "int16", "int32", "int64",
        "uint", "uint8", "uint16", "uint32", "uint64",
        "float", "double",
    };

    /** @brief Every primitive the VM has, `void` included. */
    inline constexpr std::array<std::string_view, 14> k_all = {
        "int", "int8", "int16", "int32", "int64",
        "uint", "uint8", "uint16", "uint32", "uint64",
        "float", "double", "bool", "void",
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
     * @brief A primitive that can hold a value, so `null` can never be assigned to it.
     *
     * Everything but `void`, which holds nothing at all - the distinction ControlFlowChecker draws
     * when it decides whether returning `null` is a type error.
     */
    [[nodiscard]] constexpr bool IsNonNullable(std::string_view name) noexcept
    {
        return IsPrimitive(name) && name != "void";
    }
}
