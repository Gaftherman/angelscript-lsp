#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/DeclarationSiteFacts.h"
#include "analysis/resolution/ClassHierarchyResolver.h"
#include "analysis/resolution/ScopeSymbolLookup.h"
#include "utils/Utils.h"

#include <algorithm>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Validates explicit construction from a source type to an enum target type.
 * @param[in] anchor Source range anchoring diagnostics when conversion is invalid.
 * @param[in] sourceBase Base name of the source expression type.
 * @param[in] targetType Target enum type name.
 * @param[in,out] ctx Diagnostic context for error emission.
 * @return True if targetType is an enum (handled), false otherwise.
 */
static bool CheckEnumConstruction(const SourceRange& anchor, std::string_view sourceBase, std::string_view targetType,
                                  DiagnosticContext& ctx)
{
    const SymbolTable& table = ctx.request.symbolTable;
    if (!ResolvesToEnum(targetType, table))
    {
        return false;
    }
    if (!IsNumericPrimitive(sourceBase) && !ResolvesToEnum(sourceBase, table))
    {
        ctx.EmitAtNodeRange(anchor, "as-err-no-explicit-conversion", {sourceBase, targetType});
    }
    return true;
}

/**
 * @brief Checks if a target type declares a constructor accepting a convertible source type.
 * @param[in] sourceBase Base name of the source expression type.
 * @param[in] targetType Target type name.
 * @param[in] binding Template parameter substitutions for the target type.
 * @param[in] ctx Diagnostic context.
 * @return True if a matching constructor overload exists, false otherwise.
 */
static bool IsConstructibleFrom(std::string_view sourceBase, std::string_view targetType,
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

                           auto parameterType = SingleArgumentType(parameters);
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

/**
 * @brief Checks if a target type is ignored for explicit construction checking.
 * @param[in] targetType Target type name.
 * @param[in] table Symbol table.
 * @param[in] ctx Diagnostic context.
 * @return True if targetType is a built-in value type or unresolvable non-enum.
 */
static bool IsTargetTypeIgnoredForConstruction(std::string_view targetType, const SymbolTable& table,
                                               const DiagnosticContext& ctx)
{
    if (IsBuiltInValueType(targetType, ctx))
    {
        return true;
    }
    return !FindTypeDeclaration(targetType, table).found && !ResolvesToEnum(targetType, table);
}

/**
 * @brief Checks if the source type is compatible with target type via hierarchy or conversion operators.
 * @param[in] sourceBaseName Base name of the source expression type.
 * @param[in] targetType Target type name.
 * @param[in] table Symbol table.
 * @return True if hierarchy-related or declares conversion to target.
 */
static bool IsSourceCompatibleWithTarget(std::string_view sourceBaseName, std::string_view targetType,
                                         const SymbolTable& table)
{
    return AreHierarchyRelated(sourceBaseName, targetType, table) ||
           DeclaresConversionTo(sourceBaseName, targetType, table, false);
}

/**
 * @brief Rule for a one-argument construction: T(expr) or T v(expr);.
 * @param[in] argument Sole construction argument fact.
 * @param[in] targetType Destination type name.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckConstruction(const ConstructionArgumentFact& argument, std::string_view targetType, const Scope* scope,
                       DiagnosticContext& ctx)
{
    if (!argument.present)
    {
        return;
    }
    const auto source = ResolveValueType(argument.value, scope, ctx);
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

    if (CheckEnumConstruction(argument.operand.range, source.baseName, targetType, ctx))
    {
        return;
    }

    if (!IsConstructibleFrom(source.baseName, targetType, binding, ctx))
    {
        ctx.EmitAtNodeRange(argument.operand.range, "as-err-no-explicit-conversion", {source.baseName, targetType});
    }
}

/**
 * @brief Inspects constructor overloads to determine if a zero-argument or default constructor is available.
 * @param[in] constructors Collection of constructor symbols declared on a type.
 * @return ZeroArgConstructorStatus indicating whether a default constructor exists and if it is deleted.
 */
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

/**
 * @brief Checks that a non-handle variable declaration provides a valid default constructor.
 * @param[in] target Range the diagnostic is anchored to.
 * @param[in] typeName Target type name.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckDefaultConstructor(const SourceRange& target, std::string_view typeName, DiagnosticContext& ctx)
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

    if (isDeleted)
    {
        ctx.EmitAtNodeRange(target, "as-err-deleted-method-called", {typeName, typeName});
    }
    else if (!hasZeroArg && !ctx.request.AlwaysImplementsDefaultConstruct())
    {
        ctx.EmitAtNodeRange(target, "as-err-no-default-constructor", {typeName});
    }
}

/**
 * @brief Reports every statement of a constructor body that delegates to the class itself.
 * @param[in] site Delegating constructor site whose slice selects the offending statements.
 * @param[in] facts Declaration facts that own the slice.
 * @param[in,out] ctx Diagnostic context.
 */
void CheckConstructorDelegation(const ConstructorDelegationSiteFact& site, const DeclarationSiteFacts& facts,
                                DiagnosticContext& ctx)
{
    const auto end = std::min(static_cast<size_t>(site.callBegin) + site.callCount, facts.delegationCalls.size());
    for (size_t i = site.callBegin; i < end; ++i)
    {
        ctx.EmitAtNodeRange(facts.delegationCalls[i], "as-err-constructor-delegation-disallowed");
    }
}

void CheckDeclaratorConstruction(const VariableDeclaratorFact& declarator, const DeclaredType& declared,
                                 const Scope* scope, DiagnosticContext& ctx)
{
    if (declared.isTemplateOrArray)
    {
        return;
    }

    if (ClassifyNonInstantiable(declared.baseName, ctx.request.symbolTable) == NonInstantiableKind::Abstract)
    {
        ctx.EmitAtRange(declarator.range, "as-err-abstract-instantiated", {declared.baseName, declared.baseName});
        return;
    }

    if (!declarator.hasArguments && !declarator.hasValue)
    {
        CheckDefaultConstructor(declarator.nameRange, declared.baseName, ctx);
    }
    else if (declarator.hasArguments)
    {
        CheckConstruction(declarator.argument, declared.baseName, scope, ctx);
    }
}

} // namespace angel_lsp::analysis
