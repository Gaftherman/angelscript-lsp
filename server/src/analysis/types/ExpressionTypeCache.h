#pragma once

#include "utils/Constants.h"
#include "utils/TransparentHash.h"

#include <ankerl/unordered_dense.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Sentinel type representing an initializer list expression.
 */
inline constexpr std::string_view k_initializerListTypeSentinel = constants::k_initializerListTypeSentinel;

/**
 * @brief Sentinel type representing a poisoned / broken expression type.
 *        When an expression is broken or has syntax errors, it is mapped to k_poisonType
 */
inline constexpr std::string_view k_poisonType = "<poison>";
inline constexpr std::string_view k_unknownType = "unknown";

/**
 * @brief Checks whether the given type is empty, unresolved, void, or poisoned.
 * @param[in] type Type name or string view.
 * @return True if the type is unresolved or poisoned, false otherwise.
 */
[[nodiscard]] inline constexpr bool IsUnresolvedOrPoisonType(std::string_view type) noexcept
{
    return type.empty() || type == k_unknownType || type == "void" || type.find(k_poisonType) != std::string_view::npos;
}

/**
 * @brief Checks whether the given type is the poisoned sentinel type.
 * @param[in] type Type name or string view.
 * @return True if the type is poisoned, false otherwise.
 */
[[nodiscard]] inline constexpr bool IsPoisonType(std::string_view type) noexcept
{
    if (type.size() < 8)
    {
        return false;
    }
    if (type.starts_with(k_poisonType))
    {
        return true;
    }
    return type.find('<') != std::string_view::npos && type.find(k_poisonType) != std::string_view::npos;
}

/**
 * @brief Cache key uniquely identifying an expression AST node for memoization.
 */
struct ExpressionCacheKey
{
    uint32_t startByte = 0; ///< Start byte offset in the document source.
    uint32_t endByte = 0;   ///< End byte offset in the document source.
    uint16_t symbol = 0;    ///< Grammar symbol ID from tree-sitter.

    bool operator==(const ExpressionCacheKey& other) const noexcept
    {
        return startByte == other.startByte && endByte == other.endByte && symbol == other.symbol;
    }
};

/**
 * @brief Fast hash functor for ExpressionCacheKey without memory allocations.
 */
struct ExpressionCacheKeyHash
{
    using is_avalanching = void;

    /**
     * @brief Computes 64-bit avalanche hash from AST node byte offsets and grammar symbol.
     * @param[in] k Key to hash.
     * @return 64-bit hash value.
     */
    uint64_t operator()(const ExpressionCacheKey& k) const noexcept
    {
        const uint64_t h = (static_cast<uint64_t>(k.startByte) << 32) | static_cast<uint64_t>(k.endByte);
        return h ^ (static_cast<uint64_t>(k.symbol) * 0x9e3779b97f4a7c15ULL);
    }
};

/**
 * @brief Memoization cache for resolved expression types and type hierarchies during analysis.
 */
struct ExpressionTypeCache
{
    static constexpr std::string_view k_poisonType = "<poison>";

    [[nodiscard]] static constexpr bool IsPoisonType(std::string_view type) noexcept
    {
        return angel_lsp::analysis::IsPoisonType(type);
    }

    ExpressionTypeCache()
    {
        types.reserve(512);
        hierarchies.reserve(64);
        expandedMixinHierarchies.reserve(32);
        receiverTypes.reserve(256);
        inferredMixinHostByContainer.reserve(16);
    }

    /**
     * @brief Map of AST node keys to evaluated type strings.
     */
    ankerl::unordered_dense::map<ExpressionCacheKey, std::string, ExpressionCacheKeyHash> types;

    /**
     * @brief Map of type names to their resolved base class/interface inheritance hierarchy.
     */
    ankerl::unordered_dense::map<std::string, std::vector<std::string>> hierarchies;

    /**
     * @brief Map of mixin type names to their expanded host class hierarchies.
     */
    ankerl::unordered_dense::map<std::string, std::vector<std::string>, TransparentStringHash, std::equal_to<>>
        expandedMixinHierarchies;

    /**
     * @brief Map of receiver AST node byte spans ((startByte << 32) | endByte) to resolved receiver types.
     */
    ankerl::unordered_dense::map<uint64_t, std::string> receiverTypes;

    /**
     * @brief Map of container names to inferred mixin host class.
     */
    ankerl::unordered_dense::map<std::string, std::string> inferredMixinHostByContainer;
};
} // namespace angel_lsp::analysis
