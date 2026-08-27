#include "analysis/SemanticAnalyzer.h"
#include "analysis/AccessChecker.h"
#include "analysis/CallChecker.h"
#include "analysis/ConstChecker.h"
#include "analysis/ControlFlowChecker.h"
#include "analysis/DefiniteAssignmentChecker.h"
#include "analysis/LValueChecker.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeConversionChecker.h"
#include "analysis/rules/ClassRules.h"
#include "analysis/rules/FunctionRules.h"
#include "analysis/rules/OperatorRules.h"
#include "analysis/rules/TypeRules.h"
#include "analysis/rules/VariableRules.h"
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

        // A dump of the document's own symbols, at Debug because that is what it is - the client's
        // log is not the place to narrate every declaration on every keystroke. Read through
        // ForEachSymbolInFile for the same reason the rules are: walking the whole workspace to
        // print one file's symbols was the last full-table walk left in the analysis path.
        if (m_logger)
        {
            m_logger->LogDebug(fmt::format("=== [SYMBOL COLLECTOR OUTPUT] Document: {} ===", request.fileUri));
            request.symbolTable.ForEachSymbolInFile(
                request.fileUri,
                [&](const std::string &qualifiedName, const std::vector<Symbol> &symbols)
                {
                    for (const auto &sym : symbols)
                    {
                        if (sym.fileUri == request.fileUri)
                        {
                            m_logger->LogDebug(fmt::format("  -> Symbol: [{}] Name: \"{}\" | Container: \"{}\" | Range: L{}:C{}-L{}:C{}",
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
            DiagnosticContext ctx{request, diagnostics, m_logger};

            // Taken from the version-cached index rather than rebuilt here. The set has to hold
            // every name in the workspace for this rule to be right, but it does not have to be
            // built afresh for each document - which is what it was, one full walk and fifty
            // thousand insertions per analysis.
            CheckUndefinedIdentifiers(request.scopeRoot.get(), request.GetRuleIndex().allNames, ctx);

            ankerl::unordered_dense::set<const LocalDefinition *> used;
            CollectUsedDefinitions(request.scopeRoot.get(), used);
            CheckUnusedVariables(request.scopeRoot.get(), used, ctx);

            CheckNullAssignedToNonHandleInScope(request.scopeRoot.get(), ctx);
            CheckLocalVariableDeclarations(request.scopeRoot.get(), ctx);
        }

        // Statements, not declarations: whether a break sits inside a loop or a path falls off the
        // end of a function is nowhere in the symbol table.
        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const ControlFlowCheckRequest flowRequest{ts_tree_root_node(request.tree), request.sourceCode};
            CheckControlFlow(flowRequest, ctx);
        }

        // The one pass that judges a use rather than a declaration, so it needs both the tree that
        // holds the expression and the table that holds what the expression reaches.
        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const AccessCheckRequest accessRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get()
            };
            CheckMemberAccess(accessRequest, ctx);
        }

        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const ConstCheckRequest constRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get()
            };
            CheckConstCorrectness(constRequest, ctx);
        }

        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const LValueCheckRequest lvalueRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get()
            };
            CheckLValues(lvalueRequest, ctx);
        }

        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const CallCheckRequest callRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get()
            };
            CheckCallArguments(callRequest, ctx);
        }

        if (request.tree && !request.sourceCode.empty())
        {
            DiagnosticContext ctx{request, diagnostics, m_logger};
            const DefiniteAssignmentCheckRequest assignRequest{
                ts_tree_root_node(request.tree),
                request.sourceCode,
                request.scopeRoot.get()
            };
            CheckDefiniteAssignment(assignRequest, ctx);
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
        // Only the buckets this document touches. Every rule below either filters to the analysed
        // file or, in ValidateDuplicates' case, needs the whole bucket - which it still gets. The
        // rest of the workspace's fifty thousand symbols have nothing to contribute here.
        symbolTable.ForEachSymbolInFile(
            ctx.request.fileUri,
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
                    case SymbolType::Function:
                        rules::ValidateFunction(sym, ctx);
                        rules::ValidateOperator(sym, ctx);
                        break;
                    case SymbolType::Funcdef:
                        rules::ValidateFuncdef(sym, ctx);
                        // A funcdef's parameter list obeys the same rules as a function's, minus
                        // everything that presumes a body or a container.
                        rules::ValidateParameters(sym, sym.GetFuncdef().parameters, true, ctx);
                        break;
                    case SymbolType::Enum:
                        rules::ValidateEnum(sym, ctx);
                        break;
                    case SymbolType::Variable:
                    case SymbolType::Property:
                        rules::ValidateVariable(sym, ctx);
                        break;
                    default:
                        break;
                    }
                }
            });
    }

    void SemanticAnalyzer::CheckUndefinedIdentifiers(const Scope *scope, const ankerl::unordered_dense::set<std::string> &knownGlobalNames, DiagnosticContext &ctx) const
    {
        std::vector<std::pair<uint32_t, uint32_t>> mixinRanges;
        ctx.request.symbolTable.ForEachSymbolInFile(ctx.request.fileUri, [&](const std::string &, const std::vector<Symbol> &symbols) {
            for (const auto &sym : symbols)
            {
                if (sym.type == SymbolType::Class && sym.GetClass().modifiers.isMixin)
                {
                    mixinRanges.push_back({ sym.startLine, sym.endLine });
                }
            }
        });

        for (const auto &ref : scope->references)
        {
            if (ref.isMemberAccess)
                continue;

            if (ref.name == "this" || ref.name == "value")
                continue;

            bool isInsideMixin = false;
            for (const auto &r : mixinRanges)
            {
                if (ref.startLine >= r.first && ref.endLine <= r.second)
                {
                    isInsideMixin = true;
                    break;
                }
            }
            if (isInsideMixin)
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
        symbolTable.ForEachSymbolInFile(
            ctx.request.fileUri,
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

    void SemanticAnalyzer::CheckLocalVariableDeclarations(const Scope *scope, DiagnosticContext &ctx) const
    {
        if (!scope)
        {
            return;
        }

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
                if (def.kind == LocalDefinitionKind::Variable)
                {
                    std::string base = CleanBaseType(def.typeName);
                    if (def.typeName == "void" || base == "void")
                    {
                        ctx.EmitAtRange(def.startLine, def.startCharacter, def.endLine, def.endCharacter,
                                        "as-err-void-variable", def.name, DiagnosticSeverity::Error);
                    }
                    else if (IsMixinClass(base, ctx.request.symbolTable))
                    {
                        ctx.EmitAtRange(def.startLine, def.startCharacter, def.endLine, def.endCharacter,
                                        "as-err-mixin-not-a-type", base, DiagnosticSeverity::Error);
                    }
                    else if (def.isHandleType && IsPrimitiveTypeName(base))
                    {
                        ctx.EmitAtRange(def.startLine, def.startCharacter, def.endLine, def.endCharacter,
                                        "as-err-handle-on-primitive", base, DiagnosticSeverity::Error);
                    }
                    else if (!base.empty() && base != "auto" && !IsPrimitiveTypeName(base) &&
                             !ctx.request.symbolTable.HasSymbolAnywhere(base) &&
                             !ctx.request.IsRegisteredSymbol(base))
                    {
                        ctx.EmitAtRange(def.startLine, def.startCharacter, def.endLine, def.endCharacter,
                                        "as-err-unresolved-type", base, DiagnosticSeverity::Error);
                    }
                }
            }
        }

        for (const auto &child : scope->children)
        {
            CheckLocalVariableDeclarations(child.get(), ctx);
        }
    }
}
