#include "analysis/SemanticAnalyzer.h"
#include "analysis/TypeConversionChecker.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/rules/TypeRules.h"
#include "spdlog/fmt/fmt.h"

namespace angel_lsp::analysis
{
    SemanticAnalyzer::SemanticAnalyzer(angel_lsp::utils::LspLogger *logger)
        : m_logger(logger)
    {
    }

    std::vector<Diagnostic> SemanticAnalyzer::Analyze(const SemanticAnalysisRequest &request) const
    {
        std::vector<Diagnostic> diagnostics;

        if (m_logger)
        {
            m_logger->LogInfo(fmt::format("=== [SYMBOL COLLECTOR OUTPUT] Document: {} ===", request.fileUri));
            request.symbolTable.ForEachSymbol(
                [&](const std::string &qualifiedName, const std::vector<Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.fileUri == request.fileUri)
                        {
                            m_logger->LogInfo(fmt::format("  -> Symbol: [{}] Name: \"{}\" | Container: \"{}\" | Range: L{}:C{}-L{}:C{}",
                                SymbolTypeToString(sym.type),
                                sym.name,
                                sym.containerName,
                                sym.startLine + 1,
                                sym.startCharacter + 1,
                                sym.endLine + 1,
                                sym.endCharacter + 1));
                        }
                    }
                });
        }

        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            CheckNullAssignedToNonHandle(request.symbolTable, ctx);
            CheckDeclarationRules(request.symbolTable, ctx);
        }

        if (request.scopeRoot)
        {
            ankerl::unordered_dense::set<std::string> knownGlobalNames;
            request.symbolTable.ForEachSymbol(
                [&](const std::string &, const std::vector<Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                        knownGlobalNames.insert(sym.name);
                });

            DiagnosticContext ctx{request, diagnostics, m_logger};
            CheckUndefinedIdentifiers(request.scopeRoot.get(), knownGlobalNames, ctx);

            ankerl::unordered_dense::set<const LocalDefinition *> used;
            CollectUsedDefinitions(request.scopeRoot.get(), used);
            CheckUnusedVariables(request.scopeRoot.get(), used, ctx);

            CheckNullAssignedToNonHandleInScope(request.scopeRoot.get(), ctx);
        }

        // Needs the tree, not just the symbol table: an initializer or a cast is an expression, and
        // expressions are exactly what the symbol table does not record.
        if (request.enableTypeConversionChecks && request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const TypeConversionCheckRequest conversionRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get()
            };
            CheckTypeConversions(conversionRequest, ctx);
        }

        return diagnostics;
    }

    void SemanticAnalyzer::CheckDeclarationRules(const SymbolTable &symbolTable, DiagnosticContext &ctx) const
    {
        symbolTable.ForEachSymbol(
            [&](const std::string &, const std::vector<Symbol> &symbols)
            {
                // Whether a name is redeclared is a property of the whole overload bucket, so this
                // one is handed the set rather than each member of it.
                rules::ValidateDuplicates(symbols, ctx);

                for (const auto &sym : symbols)
                {
                    // Only the document under analysis is reported on. Its module's other files are
                    // indexed alongside it so their declarations resolve, but diagnosing them here
                    // would attach findings to files the user did not open.
                    if (sym.fileUri != ctx.request.fileUri)
                    {
                        continue;
                    }

                    switch (sym.type)
                    {
                    case SymbolType::Class:
                        rules::ValidateClass(sym, ctx);
                        break;
                    case SymbolType::Interface:
                        rules::ValidateClass(sym, ctx);
                        rules::ValidateInterfaceMembers(sym, ctx);
                        break;
                    case SymbolType::Typedef:
                        rules::ValidateTypedef(sym, ctx);
                        break;
                    case SymbolType::Funcdef:
                        rules::ValidateFuncdef(sym, ctx);
                        break;
                    case SymbolType::Enum:
                        rules::ValidateEnum(sym, ctx);
                        break;
                    default:
                        break;
                    }
                }
            });
    }

    void SemanticAnalyzer::CheckUndefinedIdentifiers(const Scope *scope, const ankerl::unordered_dense::set<std::string> &knownGlobalNames, DiagnosticContext &ctx) const
    {
        for (const auto &ref : scope->references)
        {
            if (ref.isMemberAccess)
                continue;

            if (ResolveInScope(scope, ref.name) != nullptr)
                continue;

            if (knownGlobalNames.contains(ref.name))
                continue;

            ctx.EmitAtRange(ref.startLine, ref.startCharacter, ref.endLine, ref.endCharacter,
                             "as-err-undeclared-identifier", ref.name, DiagnosticSeverity::Warning);
        }

        for (const auto &child : scope->children)
            CheckUndefinedIdentifiers(child.get(), knownGlobalNames, ctx);
    }

    void SemanticAnalyzer::CollectUsedDefinitions(const Scope *scope, ankerl::unordered_dense::set<const LocalDefinition *> &used) const
    {
        for (const auto &ref : scope->references)
        {
            if (ref.isMemberAccess)
                continue;

            const LocalDefinition *def = ResolveInScope(scope, ref.name);
            if (!def)
                continue;

            bool isSelfOccurrence = ref.startLine == def->startLine && ref.startCharacter == def->startCharacter
                                  && ref.endLine == def->endLine && ref.endCharacter == def->endCharacter;
            if (isSelfOccurrence)
                continue;

            used.insert(def);
        }

        for (const auto &child : scope->children)
            CollectUsedDefinitions(child.get(), used);
    }

    void SemanticAnalyzer::CheckUnusedVariables(const Scope *scope, const ankerl::unordered_dense::set<const LocalDefinition *> &used, DiagnosticContext &ctx) const
    {
        // A Variable-kind definition only counts as a true local (as opposed to a module/
        // namespace-scope global, which LOCALS_QUERY's @local.definition.var captures under the
        // identical kind - its own comment says "Variables (locals and globals)") if it or some
        // ancestor scope was opened by a function/lambda. Globals need cross-file visibility
        // (another file in the same workspace, or the engine itself, may reference them) that a
        // single document's Scope tree can never see - confirmed by corpus spot-check, where
        // module-level declarations like "string WPN_NAME = ...;" were being flagged "unused"
        // even though "unused" isn't decidable from this file alone for those.
        bool isFunctionNested = false;
        for (const Scope *ancestor = scope; ancestor != nullptr; ancestor = ancestor->parent)
        {
            if (ancestor->isFunctionScope)
            {
                isFunctionNested = true;
                break;
            }
        }

        if (isFunctionNested)
        {
            for (const auto &def : scope->definitions)
            {
                if (def.kind != LocalDefinitionKind::Variable)
                    continue;

                if (used.contains(&def))
                    continue;

                ctx.EmitAtRange(def.startLine, def.startCharacter, def.endLine, def.endCharacter,
                                 "as-warn-unused-variable", def.name, DiagnosticSeverity::Warning);
            }
        }

        for (const auto &child : scope->children)
            CheckUnusedVariables(child.get(), used, ctx);
    }

    namespace
    {
        /**
         * @brief True only for the VM-intrinsic numeric/bool kinds, where null is unconditionally
         *        invalid under any AngelScript engine configuration - unlike a class/object type
         *        (TypeKind::Object/Unknown/String/...), which could be app-registered with a
         *        converting constructor or opAssign(int) that legitimately accepts null (e.g. this
         *        corpus's SvenCoop "EHandle eView = null;" - confirmed by corpus audit and by
         *        reading the real declaration: EHandle(pView) is a registered converting
         *        constructor, invisible to this analyzer since no predefined stub exists for it).
         */
        bool IsUnconditionallyNonNullablePrimitive(TypeKind kind)
        {
            switch (kind)
            {
                case TypeKind::Int8: case TypeKind::Int16: case TypeKind::Int32: case TypeKind::Int64:
                case TypeKind::UInt16: case TypeKind::UInt32: case TypeKind::UInt64:
                case TypeKind::Double: case TypeKind::Bool:
                    return true;
                default:
                    return false;
            }
        }
    }

    void SemanticAnalyzer::CheckNullAssignedToNonHandle(const SymbolTable &symbolTable, DiagnosticContext &ctx) const
    {
        symbolTable.ForEachSymbol(
            [&](const std::string &, const std::vector<Symbol> &symbols)
            {
                for (const auto &sym : symbols)
                {
                    if (sym.fileUri != ctx.request.fileUri)
                        continue;

                    if (!std::holds_alternative<VariableSignature>(sym.signature))
                        continue;

                    const VariableSignature &varSig = sym.GetVariable();
                    if (varSig.isVirtualProperty || !varSig.hasNullInitializer || varSig.modifiers.isHandle)
                        continue;

                    if (!IsUnconditionallyNonNullablePrimitive(varSig.typeKind))
                        continue;

                    ctx.Emit(sym, "as-err-null-non-handle", varSig.typeName, DiagnosticSeverity::Warning);
                }
            });
    }

    void SemanticAnalyzer::CheckNullAssignedToNonHandleInScope(const Scope *scope, DiagnosticContext &ctx) const
    {
        // Same isFunctionScope ancestor-walk as CheckUnusedVariables: a Variable-kind definition
        // only counts as a true function-body local (not a module/namespace/class-scope global or
        // field, which LOCALS_QUERY's @local.definition.var captures under the identical kind) if
        // it or some ancestor scope was opened by a function/lambda. Module/class-scope
        // declarations are already covered by CheckNullAssignedToNonHandle via SymbolTable.
        bool isFunctionNested = false;
        for (const Scope *ancestor = scope; ancestor != nullptr; ancestor = ancestor->parent)
        {
            if (ancestor->isFunctionScope)
            {
                isFunctionNested = true;
                break;
            }
        }

        if (isFunctionNested)
        {
            for (const auto &def : scope->definitions)
            {
                if (def.kind != LocalDefinitionKind::Variable)
                    continue;

                if (!def.hasNullInitializer || def.isHandleType)
                    continue;

                if (!IsUnconditionallyNonNullablePrimitive(def.typeKind))
                    continue;

                ctx.EmitAtRange(def.startLine, def.startCharacter, def.endLine, def.endCharacter,
                                 "as-err-null-non-handle", def.typeName, DiagnosticSeverity::Warning);
            }
        }

        for (const auto &child : scope->children)
            CheckNullAssignedToNonHandleInScope(child.get(), ctx);
    }
}
