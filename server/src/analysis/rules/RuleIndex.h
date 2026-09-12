#pragma once

#include "analysis/SymbolTable.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <ankerl/unordered_dense.h>

namespace angel_lsp::analysis::rules
{
    /**
     * @brief Reverse inheritance / mixin edges: base type name -> the types that derive from it.
     */
    struct DerivedType
    {
        std::string qualifiedName;
        std::string name;   ///< Unqualified spelling; callers record both.
    };

    /** @brief What the declaration rules need to know about one container's members. */
    struct ContainerMembers
    {
        ankerl::unordered_dense::set<std::string> methodNames;
        ankerl::unordered_dense::set<std::string> finalMethodNames;
        ankerl::unordered_dense::set<std::string> allMemberNames;
        bool hasNestedType = false;

        /**
         * @brief SymbolTable keys of this container's members, for callers that need the symbols.
         */
        std::vector<std::string> memberKeys;

        /** @brief O(1) set of member keys for fast containment checks. */
        ankerl::unordered_dense::set<std::string, TransparentStringHash, std::equal_to<>> memberKeySet;

        // Refcounts for incremental maintenance
        ankerl::unordered_dense::map<std::string, uint32_t> methodCounts;
        ankerl::unordered_dense::map<std::string, uint32_t> finalMethodCounts;
        ankerl::unordered_dense::map<std::string, uint32_t> allMemberCounts;
        ankerl::unordered_dense::map<std::string, uint32_t> memberKeyCounts;
        uint32_t nestedTypeCount = 0;
    };

    /**
     * @brief Represents the symbol index contributions from a single document.
     */
    struct RuleIndexPartial
    {
        std::string fileUri;

        struct ContainerContribution
        {
            std::vector<std::string> methodNames;
            std::vector<std::string> finalMethodNames;
            std::vector<std::string> allMemberNames;
            std::vector<std::string> memberKeys;
            uint32_t nestedTypeCount = 0;
        };

        ankerl::unordered_dense::map<std::string, ContainerContribution> byContainer;
        std::vector<std::pair<std::string, Symbol>> enumMembers;
        std::vector<std::pair<std::string, std::string>> qualifiedTypes;
        std::vector<std::string> accessorProperties;
        std::vector<std::string> keywordAccessorProperties;
        std::vector<std::string> allNames;
        std::vector<std::pair<std::string, DerivedType>> derivedByBase;
        std::vector<std::pair<std::string, DerivedType>> hostClassesByMixin;

        void Merge(RuleIndexPartial &&other);
    };

    /**
     * @brief One pass over the symbol table, answering the questions the rules used to re-walk for.
     */
    struct RuleIndex
    {
        using DerivedType = angel_lsp::analysis::rules::DerivedType;

        ankerl::unordered_dense::map<std::string, ContainerMembers, TransparentStringHash, std::equal_to<>> byContainer;
        ankerl::unordered_dense::set<std::string> enumMemberNames;
        ankerl::unordered_dense::map<std::string, uint32_t> enumMemberCounts;

        /**
         * @brief Symbols of enums containing each member name.
         * Allows fast O(1) lookup of enum symbols when resolving unqualified enum member references.
         */
        ankerl::unordered_dense::map<std::string, std::vector<Symbol>> enumSymbolsByMemberName;

        /**
         * @brief Qualified names of types (classes, interfaces, enums, typedefs, funcdefs) keyed by short name.
         * Allows fast lookup of types when referenced without their enclosing namespace.
         */
        ankerl::unordered_dense::map<std::string, std::vector<std::string>> qualifiedTypesByShortName;
        ankerl::unordered_dense::map<std::string, ankerl::unordered_dense::map<std::string, uint32_t>> qualifiedTypeCounts;

        ankerl::unordered_dense::set<std::string> accessorPropertyNames;
        ankerl::unordered_dense::map<std::string, uint32_t> accessorPropertyCounts;

        ankerl::unordered_dense::set<std::string> keywordAccessorPropertyNames;
        ankerl::unordered_dense::map<std::string, uint32_t> keywordAccessorPropertyCounts;

        /**
         * @brief Every declared name in the workspace, with refcounts for incremental updates.
         */
        ankerl::unordered_dense::map<std::string, uint32_t, TransparentStringHash, std::equal_to<>> allNames;

        ankerl::unordered_dense::map<std::string, std::vector<DerivedType>> derivedByBase;
        ankerl::unordered_dense::map<std::string, std::vector<DerivedType>> hostClassesByMixin;

        /** @brief Members of one container, or an empty set of them when it declares none. */
        const ContainerMembers &Members(std::string_view containerName) const;
        const ContainerMembers &Members(const std::string &containerName) const;
        const ContainerMembers &Members(const char *containerName) const
        {
            return Members(std::string_view(containerName));
        }

        /** @brief Applies the contributions of a partial index to this index. */
        void ApplyPartial(const RuleIndexPartial &partial);

        /** @brief Removes the contributions of a partial index from this index. */
        void RemovePartial(const RuleIndexPartial &partial);

        /** @brief Extracts a partial index from the given symbols belonging to fileUri. */
        static RuleIndexPartial BuildPartial(const std::string &fileUri, const std::vector<Symbol> &symbols);

        /** @brief Walks the table once and returns the index it yields. */
        static std::shared_ptr<RuleIndex> Build(const SymbolTable &table);
    };
}
