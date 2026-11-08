#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarNames.h"
#include "parser/Primitives.h"
#include "utils/Utils.h"

#include <algorithm>
#include <string>
#include <vector>

namespace angel_lsp::analysis
{

static bool CheckEnumConstruction(TSNode argument, const std::string& sourceBase, const std::string& targetType,
                                  DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    if (!ResolvesToEnum(targetType, table))
    {
        return false;
    }
    if (!IsNumericPrimitive(sourceBase) && !ResolvesToEnum(sourceBase, table))
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(argument), "as-err-no-explicit-conversion", {sourceBase, targetType});
    }
    return true;
}

static bool IsConstructibleFrom(const std::string& sourceBase, const std::string& targetType,
                                const TemplateBinding& binding, const DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    bool constructible = false;
    ForEachConstructor(targetType, table,
                       [&](const Symbol& sym)
                       {
                           const auto& parameters = sym.GetFunction().parameters;
                           if (!AcceptsSingleArgument(parameters))
                           {
                               return false;
                           }

                           std::string parameterType = SingleArgumentType(parameters);
                           for (size_t i = 0; i < binding.parameters.size(); ++i)
                           {
                               parameterType =
                                   SubstituteTypeParam(parameterType, binding.parameters[i], binding.arguments[i]);
                           }

                           if (IsConvertible(sourceBase, parameterType, ctx, 1))
                           {
                               constructible = true;
                               return true;
                           }
                           return false;
                       });
    return constructible;
}

static bool IsTargetTypeIgnoredForConstruction(const std::string& targetType, const SymbolTable& table,
                                               const DiagnosticContext& ctx)
{
    if (IsBuiltInValueType(targetType, ctx))
    {
        return true;
    }
    return !FindTypeDeclaration(targetType, table).found && !ResolvesToEnum(targetType, table);
}

static bool IsSourceCompatibleWithTarget(const std::string& sourceBaseName, const std::string& targetType,
                                         const SymbolTable& table)
{
    return AreHierarchyRelated(sourceBaseName, targetType, table) ||
           DeclaresConversionTo(sourceBaseName, targetType, table, false);
}

void CheckConstruction(TSNode argumentListNode, const std::string& targetType, const Scope* scope,
                       DiagnosticContext& ctx)
{
    const TSNode argument = SyntaxOperands(argumentListNode, ctx.request.nodeIndex).OnlyNamed();
    if (ts_node_is_null(argument))
    {
        return;
    }
    const ExpressionType source = ResolveValueType(argument, scope, ctx);
    if (!source.known || source.baseName.empty() || IsSameType(source.baseName, targetType))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    if (IsTargetTypeIgnoredForConstruction(targetType, table, ctx) ||
        IsSourceCompatibleWithTarget(source.baseName, targetType, table))
    {
        return;
    }

    const TemplateBinding binding = BindTemplateArguments(targetType, table);
    if (binding.isTemplate && !binding.usable)
    {
        return;
    }

    if (CheckEnumConstruction(argument, source.baseName, targetType, ctx))
    {
        return;
    }

    if (!IsConstructibleFrom(source.baseName, targetType, binding, ctx))
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(argument), "as-err-no-explicit-conversion",
                            {source.baseName, targetType});
    }
}

static ZeroArgConstructorStatus InspectZeroArgConstructor(const std::vector<Symbol>& constructors)
{
    ZeroArgConstructorStatus status;
    for (const auto& ctor : constructors)
    {
        const auto& sig = ctor.GetFunction();
        bool canTakeZero = sig.parameters.empty();
        if (!canTakeZero)
        {
            canTakeZero = std::all_of(sig.parameters.begin(), sig.parameters.end(),
                                      [](const ParameterInformation& p) { return !p.defaultValue.empty(); });
        }
        if (canTakeZero)
        {
            status.hasZeroArg = true;
            status.isDeleted = sig.modifiers.isDelete;
            break;
        }
    }
    return status;
}

void CheckDefaultConstructor(TSNode declaratorNode, const std::string& typeName, DiagnosticContext& ctx)
{
    if (utils::IsPredefinedFile(ctx.request.fileUri, ctx.request.predefinedFileExtension))
    {
        return;
    }

    if (typeName.empty() || IsBuiltInValueType(typeName, ctx))
    {
        return;
    }

    const SymbolTable& table = ctx.request.symbolTable;
    const TypeDeclarationInfo decl = FindTypeDeclaration(typeName, table);
    if (!decl.found || !decl.isClass || decl.isTemplate)
    {
        return;
    }

    std::vector<Symbol> constructors;
    ForEachConstructor(typeName, table,
                       [&](const Symbol& sym)
                       {
                           constructors.push_back(sym);
                           return false;
                       });

    if (constructors.empty())
    {
        return;
    }

    const auto [hasZeroArg, isDeleted] = InspectZeroArgConstructor(constructors);

    TSNode nameNode =
        SyntaxOperands(declaratorNode, ctx.request.nodeIndex).Field(parser::GetGrammarSymbols().fieldName);
    TSNode targetNode = ts_node_is_null(nameNode) ? declaratorNode : nameNode;

    if (isDeleted)
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(targetNode), "as-err-deleted-method-called", {typeName, typeName});
    }
    else if (!hasZeroArg && !ctx.request.AlwaysImplementsDefaultConstruct())
    {
        ctx.EmitAtNodeRange(ToOptionalSourceRange(targetNode), "as-err-no-default-constructor", {typeName});
    }
}

static void CheckConstructorDelegationStatement(TSNode stmt, const std::string& className, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_symbol(stmt) != gs.symExpressionStatement)
    {
        return;
    }

    const SyntaxOperands exprOperands = SyntaxOperands(stmt, ctx.request.nodeIndex).FirstNamedOperands();
    if (exprOperands.IsNull())
    {
        return;
    }
    const TSSymbol exprSym = exprOperands.Symbol();
    if (exprSym == gs.symCallExpression || exprSym == gs.symConstructCallExpression)
    {
        TSNode callee = exprOperands.Callee();
        if (!ts_node_is_null(callee) && parser::NodeText(callee, ctx.request.sourceCode) == className)
        {
            ctx.EmitAtNodeRange(ToOptionalSourceRange(exprOperands.Node()), "as-err-constructor-delegation-disallowed");
        }
    }
}

void CheckConstructorDelegation(TSNode funcNode, const NodeIndex& index, DiagnosticContext& ctx)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSNode parent = index.EnclosingDeclaration(funcNode);
    if (ts_node_is_null(parent))
    {
        return;
    }

    TSNode classNameNode = SyntaxOperands(parent, &index).Field(gs.fieldName);
    if (ts_node_is_null(classNameNode))
    {
        return;
    }
    const std::string className = parser::NodeText(classNameNode, ctx.request.sourceCode);

    const SyntaxOperands function(funcNode, &index);
    const TSNode funcNameNode = function.Field(gs.fieldName);
    if (ts_node_is_null(funcNameNode) || parser::NodeText(funcNameNode, ctx.request.sourceCode) != className)
    {
        return;
    }

    const SyntaxOperands body = function.FieldOperands(gs.fieldBody);
    if (body.IsNull())
    {
        return;
    }

    body.ForEachNamed(
        [&](TSNode statement)
        {
            CheckConstructorDelegationStatement(statement, className, ctx);
            return true;
        });
}

} // namespace angel_lsp::analysis
