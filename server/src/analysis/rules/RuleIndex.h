#pragma once

#include "analysis/SymbolTable.h"

#include <memory>
#include <string>
#include <ankerl/unordered_dense.h>

namespace angel_lsp::analysis::rules
{
    /** @brief What the declaration rules need to know about one container's members. */
    struct ContainerMembers
    {
        ankerl::unordered_dense::set<std::string> methodNames;
        ankerl::unordered_dense::set<std::string> finalMethodNames;
        bool hasNestedType = false;
    };

    /**
     * @brief One pass over the symbol table, answering the questions the rules used to re-walk for.
     *
     * "Which methods does this class declare?", "is this name some enum's member?" and "does this
     * mixin nest a type?" were each answered by a full ForEachSymbol walk, run once per symbol
     * being validated. On a workspace-sized table that is quadratic, and it dominated analysis by
     * an order of magnitude - 218 ms per file against 43 ms for parsing, collection and scopes put
     * together. Built once per analysis, the same answers cost one walk.
     */
    struct RuleIndex
    {
        ankerl::unordered_dense::map<std::string, ContainerMembers> byContainer;
        ankerl::unordered_dense::set<std::string> enumMemberNames;

        /**
         * @brief Every declared name in the workspace, for the undeclared-identifier rule.
         *
         * That rule genuinely needs all of them - a name declared in any indexed file counts - but
         * it does not need the set rebuilt per document. Built here, it survives until the table
         * changes instead of costing one full walk and fifty thousand insertions per keystroke.
         */
        ankerl::unordered_dense::set<std::string> allNames;

        /** @brief Members of one container, or an empty set of them when it declares none. */
        const ContainerMembers &Members(const std::string &containerName) const;

        /** @brief Walks the table once and returns the index it yields. */
        static std::shared_ptr<const RuleIndex> Build(const SymbolTable &table);
    };
}
