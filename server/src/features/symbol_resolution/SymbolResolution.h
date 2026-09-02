#pragma once

#include "analysis/ScopeTree.h"
#include "analysis/SymbolTable.h"

#include <lsp/messages.h>
#include <tree_sitter/api.h>

#include <optional>
#include <string>
#include <vector>

namespace angel_lsp::features::resolution
{
    /** @brief What kind of thing the cursor is on, which decides where its occurrences can be. */
    enum class TargetKind
    {
        Local,           ///< A variable or parameter, confined to one function scope.
        ClassMember,     ///< A field or method, reachable through a class hierarchy.
        NamespaceSymbol, ///< Declared inside a namespace and qualified by it.
        GlobalSymbol     ///< Everything else: a free function, a type, a global variable.
    };

    /** @brief The symbol under the cursor, resolved far enough to look its occurrences up. */
    struct TargetDescriptor
    {
        TargetKind kind = TargetKind::GlobalSymbol;
        std::string name;
        std::string qualifiedName;

        const analysis::Scope *definingScope = nullptr;
        analysis::LocalDefinition localDef;
        std::string localUri;

        std::string declaringClass;
        std::vector<std::string> relatedClasses;

        std::string declaringNamespace;
    };

    /**
     * @brief True for a name that is a legal AngelScript identifier and not reserved.
     *
     * Rename needs it for the *new* name as well, which is why it is exported rather than private
     * to this module.
     */
    bool IsValidIdentifier(std::string_view name);

    /**
     * @brief Works out what the cursor is on.
     *
     * @param outNode Receives the identifier node the position landed on.
     * @return The target, or nullopt when the position is not on a renameable symbol.
     */
    std::optional<TargetDescriptor> ResolveTargetSymbol(
        const std::string &uri,
        const std::string &sourceCode,
        TSTree *tree,
        lsp::Position position,
        const analysis::SymbolTable &symbolTable,
        const analysis::ScopeIndex &scopeIndex,
        TSNode &outNode);

    /**
     * @brief Every place the resolved target appears, across every indexed document.
     *
     * This and ResolveTargetSymbol are the whole of what rename and find-references share, and they
     * shared it by having a copy each: ~780 lines whose only non-mechanical difference was the
     * guard on the token under the cursor. Duplication was not the defect - a resolution fix
     * applied to one and not the other was, because rename would then edit a different set of
     * occurrences than find-references had shown, and the user is told what will change while
     * something else changes. No diagnostic covers that and neither file's own tests would have
     * noticed, since each would still pass.
     *
     * RenameReferencesParityTest sweeps every identifier position in a set of samples and requires
     * the two features to return the same ranges. It caught nothing when it was written - they
     * agreed everywhere, 303 comparisons - which is exactly why extracting this was safe to do,
     * and why the sweep stays: a shared core can still be given a caller-specific branch by
     * mistake.
     *
     * @param includeDeclaration Whether the declaration itself is one of the occurrences. This is
     *        the one place the two callers genuinely disagreed, and the sweep above could not have
     *        found it: find-references honours the LSP request flag through eight separate guards
     *        and a set of declaration ranges, and rename never had them, because a rename that
     *        skipped the declaration would leave the symbol half renamed. Defaults to rename's
     *        answer. This body is find-references' copy, which is the superset - taking rename's
     *        and adding the flag would have meant re-deriving those eight sites by hand.
     */
    std::vector<lsp::Location> CollectOccurrences(
        const TargetDescriptor &target,
        const std::string &currentUri,
        const std::string &sourceCode,
        TSTree *tree,
        const analysis::SymbolTable &symbolTable,
        const analysis::ScopeIndex &scopeIndex,
        bool includeDeclaration = true);
}
