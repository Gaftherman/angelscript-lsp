#include "analysis/checkers/TypeConversionCommon.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/resolution/OperandRangeResolution.h"
#include "analysis/types/ExpressionTypeCache.h"
#include "analysis/types/TypeSanitization.h"

#include <algorithm>
#include <string_view>

namespace angel_lsp::analysis
{

/**
 * @brief Checks whether a declared type denotes a usable non-template class, enum or built-in value type.
 * @param[in] baseName Cleaned type name to inspect.
 * @param[in] ctx Diagnostic context carrying symbol table and configuration.
 * @return True when the type can be instantiated or checked for conversions; false otherwise.
 */
static bool IsDeclaredTypeUsable(std::string_view baseName, const DiagnosticContext& ctx)
{
    if (IsBuiltInValueType(baseName, ctx) || ResolvesToEnum(baseName, ctx.request.symbolTable))
    {
        return true;
    }

    const TypeDeclarationInfo declaration = FindTypeDeclaration(baseName, ctx.request.symbolTable);
    if (!declaration.found || !declaration.isClass || declaration.isTemplate)
    {
        return false;
    }
    return !ResolvesToNonClassDeclaration(baseName, ctx.request.symbolTable);
}

/**
 * @brief Reads a written type spelling into the shape the conversion rules can act on.
 * @param[in] raw Type spelling as written, including qualifiers and decorations.
 * @param[in] ctx Diagnostic context.
 * @return DeclaredType structure.
 */
DeclaredType ReadDeclaredType(std::string_view raw, const DiagnosticContext& ctx)
{
    DeclaredType result;
    if (raw.empty())
    {
        return result;
    }

    result.qualifiers.isHandle = IsHandleTypeView(raw);
    result.qualifiers.isConst = IsConstTypeView(raw);
    result.baseName = CleanBaseType(raw);
    if (result.baseName.empty() || ctx.request.IsRegisteredSymbol(result.baseName))
    {
        return result;
    }

    if (raw.find('<') != std::string_view::npos || raw.find('[') != std::string_view::npos)
    {
        result.usable = true;
        result.isTemplateOrArray = true;
        return result;
    }

    result.usable = IsDeclaredTypeUsable(result.baseName, ctx);
    return result;
}

/**
 * @brief Reports diagnostics when the initializer expression resolves to an unknown symbol or type.
 * @param[in] declarator Declarator fact holding the initializer operand.
 * @param[in] declared Declared type details of the variable.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckUnknownInitializerSource(const VariableDeclaratorFact& declarator, const DeclaredType& declared,
                                          const Scope* scope, DiagnosticContext& ctx)
{
    const std::string_view identName = TrimString(declarator.valueOperand.text);
    if (identName.empty())
    {
        return;
    }

    bool isTypeOrTemplate = false;
    bool isNamespace = false;
    ForEachSymbolNamed(identName, ctx.request.symbolTable,
                       [&](const Symbol& s) -> bool
                       {
                           if (s.type == SymbolType::Class || s.type == SymbolType::Interface ||
                               s.type == SymbolType::Typedef || s.type == SymbolType::Enum)
                           {
                               isTypeOrTemplate = true;
                               return false;
                           }
                           if (s.type == SymbolType::Namespace)
                           {
                               isNamespace = true;
                               return false;
                           }
                           return true;
                       });
    if (isTypeOrTemplate)
    {
        EmitExpressionIsDataType(ToConversionExpression(declarator.valueOperand), ctx, identName);
        return;
    }
    if (isNamespace)
    {
        ctx.EmitAtRange(declarator.valueOperand.range, "as-err-undefined-identifier", identName);
        return;
    }

    if (!FindFuncdef(declared.baseName, ctx.request.symbolTable) &&
        IsFunctionReference(declarator.valueOperand.bytes, ExpressionTypeContext(scope, ctx)))
    {
        ctx.EmitAtRange(declarator.valueOperand.range, "as-err-no-implicit-conversion", {identName, declared.baseName});
    }
}

/**
 * @brief Reports diagnostics when initializing a handle variable from an incompatible source.
 * @param[in] declarator Declarator fact holding the initializer operand.
 * @param[in] source Resolved type of the initializer expression.
 * @param[in] declared Declared type details of the target variable.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckHandleInitializer(const VariableDeclaratorFact& declarator, const ExpressionType& source,
                                   const DeclaredType& declared, DiagnosticContext& ctx)
{
    if (source.baseName == "null")
    {
        return;
    }
    if (source.isLiteral)
    {
        ctx.EmitAtRange(declarator.valueOperand.range, "as-err-no-implicit-conversion",
                        {source.baseName, declared.baseName});
        return;
    }
    if (source.isConst && !declared.qualifiers.isConst)
    {
        ctx.EmitAtRange(declarator.valueOperand.range, "as-err-no-implicit-conversion",
                        {"const " + source.baseName + "@", declared.baseName + "@"});
        return;
    }
    if (IsSameType(source.baseName, declared.baseName))
    {
        return;
    }

    if (ctx.request.symbolTable.HasSymbolAnywhere(source.baseName) &&
        ctx.request.symbolTable.HasSymbolAnywhere(declared.baseName))
    {
        const auto hierarchy = GetInheritedTypeHierarchy(declared.baseName, ctx.request.symbolTable);
        if (std::find(hierarchy.begin(), hierarchy.end(), source.baseName) != hierarchy.end())
        {
            ctx.EmitAtRange(declarator.valueOperand.range, "as-err-no-implicit-conversion",
                            {source.baseName + "@", declared.baseName + "@"});
        }
    }
}

/**
 * @brief Validates variable initializer compatibility against the declared variable type.
 * @param[in] declarator Declarator fact holding the initializer value and operand.
 * @param[in] declared Declared type details of the variable.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
static void CheckInitializer(const VariableDeclaratorFact& declarator, const DeclaredType& declared, const Scope* scope,
                             DiagnosticContext& ctx)
{
    if (!declarator.hasValue || declarator.valueIsInitializerList)
    {
        return;
    }

    if (const auto dataTypeName = IsBareDataType(declarator.valueOperand.bytes, ExpressionTypeContext(scope, ctx)))
    {
        EmitExpressionIsDataType(ToConversionExpression(declarator.valueOperand), ctx, *dataTypeName);
        return;
    }

    const ExpressionType source = ResolveValueType(declarator.value, scope, ctx);
    if (ExpressionTypeCache::IsPoisonType(source.baseName))
    {
        return;
    }
    if (!source.known || source.baseName.empty())
    {
        CheckUnknownInitializerSource(declarator, declared, scope, ctx);
        return;
    }

    if (!declared.qualifiers.isHandle)
    {
        CheckFloatTruncation(declarator.valueOperand, {source.baseName, declared.baseName}, scope, ctx);
    }
    else
    {
        CheckHandleInitializer(declarator, source, declared, ctx);
        return;
    }

    if (!IsConvertible(source.baseName, declared.baseName, ctx))
    {
        ctx.EmitAtRange(declarator.valueOperand.range, "as-err-no-implicit-conversion",
                        {source.baseName, declared.baseName});
    }
}

/**
 * @brief Validates non-handle declarator construction semantics.
 * @param[in] declarator Variable declarator fact.
 * @param[in] declared Declared type details.
 * @param[in] scope Enclosing lexical scope.
 * @param[in,out] ctx Diagnostic context.
 */
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

/**
 * @brief Validates each declarator in a declared-type variable declaration statement.
 * @param[in] site Variable declaration site fact.
 * @param[in] request Analysis request carrying declaration facts and scope root.
 * @param[in,out] ctx Diagnostic context.
 */
static void ProcessDeclaredVariableDeclarators(const VariableDeclarationSiteFact& site,
                                               const TypeConversionCheckRequest& request, DiagnosticContext& ctx)
{
    if (!request.declarationSites)
    {
        return;
    }

    const auto funcdef = FindFuncdef(CleanBaseType(site.typeText), ctx.request.symbolTable);
    const auto& declarators = request.declarationSites->declarators;
    const uint32_t begin = site.declaratorBegin;
    const uint32_t end = site.declaratorBegin + site.declaratorCount;
    const uint32_t sliceEnd = std::min<uint32_t>(end, static_cast<uint32_t>(declarators.size()));

    if (funcdef)
    {
        for (uint32_t i = begin; i < sliceEnd; ++i)
        {
            const auto& declarator = declarators[i];
            CheckFuncdefAssignment(declarator.range, funcdef->GetFuncdef(), declarator.funcdefValue, ctx);
        }
    }

    const DeclaredType declared = ReadDeclaredType(site.typeText, ctx);
    if (!declared.usable || IsMixinClass(declared.baseName, ctx.request.symbolTable))
    {
        return;
    }

    const Scope* scope = ResolveNodeScope(site.range, request);
    for (uint32_t i = begin; i < sliceEnd; ++i)
    {
        const auto& declarator = declarators[i];
        CheckInitializer(declarator, declared, scope, ctx);
        if (!declared.qualifiers.isHandle)
        {
            CheckDeclaratorConstruction(declarator, declared, scope, ctx);
        }
    }
}

/**
 * @brief Validates a variable declaration across auto and explicit types.
 * @param[in] site Variable declaration fact.
 * @param[in] request Analysis request details.
 * @param[in,out] ctx Diagnostic context.
 */
void ProcessVariableDeclarationSite(const VariableDeclarationSiteFact& site, const TypeConversionCheckRequest& request,
                                    DiagnosticContext& ctx)
{
    if (CleanBaseType(site.typeText) == "auto")
    {
        if (request.conversionSites)
        {
            if (const auto* autoSite = request.conversionSites->FindAutoDeclaration(site.bytes))
            {
                ProcessAutoDeclarationSite(*autoSite, request, ctx);
            }
        }
        return;
    }
    ProcessDeclaredVariableDeclarators(site, request, ctx);
}

} // namespace angel_lsp::analysis
