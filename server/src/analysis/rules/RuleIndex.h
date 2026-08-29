#pragma once

#include "analysis/SymbolTable.h"

#include <memory>
#include <string>
#include <vector>
#include <ankerl/unordered_dense.h>

namespace angel_lsp::analysis::rules
{
    /** @brief What the declaration rules need to know about one container's members. */
    struct ContainerMembers
    {
        ankerl::unordered_dense::set<std::string> methodNames;
        ankerl::unordered_dense::set<std::string> finalMethodNames;
        bool hasNestedType = false;

        /**
         * @brief SymbolTable keys of this container's members, for callers that need the symbols.
         *
         * The name sets above answer the declaration rules' yes/no questions. Completion needs the
         * declarations themselves - a method's signature, a field's type - and was getting them by
         * walking the entire workspace table once per type in the inheritance chain, on every
         * keystroke. Holding the keys instead lets it probe FindSymbolsPtr per member: the index
         * costs one string per member and is rebuilt only when the table's version moves.
         */
        std::vector<std::string> memberKeys;
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
         * @brief Property names behind `get_X` / `set_X` members, for the undeclared-identifier rule.
         *
         * A virtual property is reached by a name nothing declares. `class C { int get_Up() const
         * property { … } void T() { int v = Up; } }` compiles, and no symbol is ever named `Up` -
         * the member is `C::get_Up`. Without these two sets every such use was reported.
         *
         * Two sets because the engine has two answers. asEP_PROPERTY_ACCESSOR_MODE 2 makes any
         * `get_`/`set_` member the property; mode 3, the engine's own default, requires the
         * `property` keyword, and without it `c.V` really is an error. Which set a rule reads is a
         * per-request question (EngineProperties::propertyAccessorMode); which names go in either
         * is not, and this index is shared across requests, so both are built and the choice is
         * left to the caller.
         *
         * Registered globally rather than per class, for the same reason the template parameters
         * below are: correct scoping would mean threading a per-container name set through the
         * identifier check, and a missed shadowing costs nothing here while a false "Undeclared
         * identifier" costs trust in every other diagnostic.
         */
        ankerl::unordered_dense::set<std::string> accessorPropertyNames;
        ankerl::unordered_dense::set<std::string> keywordAccessorPropertyNames;

        /**
         * @brief Every declared name in the workspace, for the undeclared-identifier rule.
         *
         * That rule genuinely needs all of them - a name declared in any indexed file counts - but
         * it does not need the set rebuilt per document. Built here, it survives until the table
         * changes instead of costing one full walk and fifty thousand insertions per keystroke.
         */
        ankerl::unordered_dense::set<std::string> allNames;

        /**
         * @brief Reverse inheritance edges: base type name -> the types that derive from it.
         *
         * Inheritance is written the other way round - a class names its bases - so answering
         * "who derives from this?" used to mean scanning every symbol in the workspace. Worse, the
         * caller needed the *transitive* set, so it re-scanned in a fixpoint loop until no new
         * derived class appeared: O(depth x whole table) per call, on a table this codebase's own
         * comments size at fifty thousand symbols. With the edges reversed once per table version
         * it is an ordinary breadth-first walk.
         *
         * Keys are cleaned base names as written in the declaration; values are qualified names.
         */
        struct DerivedType
        {
            std::string qualifiedName;
            std::string name;   ///< Unqualified spelling; callers record both.
        };

        ankerl::unordered_dense::map<std::string, std::vector<DerivedType>> derivedByBase;

        /** @brief Members of one container, or an empty set of them when it declares none. */
        const ContainerMembers &Members(const std::string &containerName) const;

        /** @brief Walks the table once and returns the index it yields. */
        static std::shared_ptr<const RuleIndex> Build(const SymbolTable &table);
    };
}
