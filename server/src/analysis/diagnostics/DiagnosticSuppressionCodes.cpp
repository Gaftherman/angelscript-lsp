#include "analysis/diagnostics/DiagnosticCodes.h"
#include "analysis/diagnostics/DiagnosticSuppression.h"
#include "parser/Keywords.h"
#include "utils/Utils.h"

#include <utility>

namespace angel_lsp::analysis
{

inline constexpr CodeAliasEntry k_codeAliases[] = {
    // Warnings
    {"W101", diagnostics::codes::UninitializedVariableRead},
    {"W102", diagnostics::codes::UnreachableCode},
    {"W103", diagnostics::codes::UnusedVariable},
    {"W104", diagnostics::codes::ShadowGlobal},
    {"W105", diagnostics::codes::SignedUnsignedMismatch},
    {"W106", diagnostics::codes::FloatTruncation},
    {"W107", diagnostics::codes::UnsupportedDirective},
    {"W108", diagnostics::codes::UndeclaredIdentifier},
    {"W109", diagnostics::codes::PossibleNullDereference},
    {"W110", diagnostics::codes::HandleComparisonEquality},
    {"W112", diagnostics::codes::IncludeNotFound},
    {"W113", diagnostics::codes::EngineStorageRule},
    {"W114", diagnostics::codes::EngineSchedulerSafety},
    {"W115", diagnostics::codes::EngineScheduledEnumDiscriminant},
    {"W116", diagnostics::codes::UnusedField},
    {"W117", diagnostics::codes::UnusedGlobalVariable},
    {"W118", diagnostics::codes::UnusedFunction},
    {"W119", diagnostics::codes::UnusedMethod},
    {"W120", diagnostics::codes::UnusedClass},
    {"W121", diagnostics::codes::EngineCallSafety},
    {"W122", diagnostics::codes::BinaryOperatorArity},
    {"W123", diagnostics::codes::ImplicitMemberAccess},
    // Hints
    {"W151", diagnostics::codes::AccessorDisabled},
    {"W152", diagnostics::codes::AccessorPortability},
    {"W153", diagnostics::codes::BoolConversion},
    {"W154", diagnostics::codes::FuncdefMissing},
    {"W155", diagnostics::codes::IntegerDivision},
    {"W156", diagnostics::codes::ListPatternUnknown},
    {"W157", diagnostics::codes::RepeatedConversion},
    {"W158", diagnostics::codes::ImportUnknownModule},
    {"W159", diagnostics::codes::FileInSeveralModules},
    {"W160", diagnostics::codes::EngineTypeSuggestion},
    // Syntax errors
    {"E100", diagnostics::codes::SyntaxError},
    {"E101", diagnostics::codes::SyntaxErrorGeneric},
    {"E102", diagnostics::codes::SyntaxErrorMissing},
    // Errors
    {"E103", diagnostics::codes::UnknownType},
    {"E104", diagnostics::codes::DuplicateDeclaration},
    {"E105", diagnostics::codes::VoidVariable},
    {"E106", diagnostics::codes::HandleOnPrimitive},
    {"E107", diagnostics::codes::ReservedKeywordName},
    {"E108", diagnostics::codes::MixinNotAType},
    {"E109", diagnostics::codes::MixinFinal},
    {"E110", diagnostics::codes::UndefinedNamespace},
    {"E111", diagnostics::codes::UndefinedIdentifier},
    {"E112", diagnostics::codes::TypedefNonPrimitive},
    {"E113", diagnostics::codes::PrivateMemberAccess},
    {"E114", diagnostics::codes::ProtectedMemberAccess},
    {"E115", diagnostics::codes::MemberNotFound},
    {"E116", diagnostics::codes::LambdaClosureDisallowed},
    {"E117", diagnostics::codes::CallArgumentCount},
    {"E118", diagnostics::codes::CallAmbiguous},
    {"E119", diagnostics::codes::CallNoMatchingSignature},
    {"E120", diagnostics::codes::NoMatchingConstructor},
    {"E121", diagnostics::codes::NoImplicitConversion},
    {"E122", diagnostics::codes::PositionalAfterNamedArg},
    {"E123", diagnostics::codes::LValueRequiredForOutParam},
    {"E124", diagnostics::codes::ConstAssignment},
    {"E125", diagnostics::codes::ConstMethodRequired},
    {"E126", diagnostics::codes::NotLValue},
    {"E127", diagnostics::codes::AssignVoid},
    {"E128", diagnostics::codes::AssignNonRefCall},
    {"E129", diagnostics::codes::BreakOutsideLoop},
    {"E130", diagnostics::codes::ContinueOutsideLoop},
    {"E131", diagnostics::codes::NotAllPathsReturn},
    {"E132", diagnostics::codes::ReturnValueRequired},
    {"E133", diagnostics::codes::ConditionNotBoolean},
    {"E134", diagnostics::codes::InvalidCaseType},
    {"E135", diagnostics::codes::CaseNotConstant},
    {"E136", diagnostics::codes::DuplicateCaseValue},
    {"E137", diagnostics::codes::DefaultMustBeLast},
    {"E138", diagnostics::codes::IfEmptyStatement},
    {"E139", diagnostics::codes::ElseEmptyStatement},
    {"E140", diagnostics::codes::SharedNotAllowedOnEntity},
    {"E141", diagnostics::codes::SharedCannotAccessNonShared},
    {"E142", diagnostics::codes::ExternalNotFound},
    {"E143", diagnostics::codes::ExternalNotShared},
    {"E144", diagnostics::codes::PrimitiveInoutRefDisallowed},
    {"E145", diagnostics::codes::GlobalVarsDisallowed},
    {"E146", diagnostics::codes::EnumScopeRequired},
    {"E147", diagnostics::codes::UnknownDirective},
    {"E148", diagnostics::codes::DirectiveSpaceAfterHash},
    {"E149", diagnostics::codes::IncludeNotQuoted},
    {"E150", diagnostics::codes::AbstractInstantiated},
    {"E151", diagnostics::codes::ArrayInvalidTemplate},
    {"E152", diagnostics::codes::AttributeRepeated},
    {"E153", diagnostics::codes::AutoRequiresInitializer},
    {"E155", diagnostics::codes::CannotInferNull},
    {"E156", diagnostics::codes::CannotInferVoid},
    {"E157", diagnostics::codes::CannotReturnLocalRef},
    {"E158", diagnostics::codes::CannotReturnParamRef},
    {"E159", diagnostics::codes::CharacterLiteralIsString},
    {"E160", diagnostics::codes::CircularInherit},
    {"E161", diagnostics::codes::ClassMemberConst},
    {"E162", diagnostics::codes::CompoundAssignOnIndexedProp},
    {"E164", diagnostics::codes::ConstOutParam},
    {"E165", diagnostics::codes::ConstVoidReturn},
    {"E166", diagnostics::codes::ConstructorDelegationDisallowed},
    {"E167", diagnostics::codes::ConstructorNotCallable},
    {"E168", diagnostics::codes::CyclicAutoDependency},
    {"E169", diagnostics::codes::DeclarationMissingBody},
    {"E170", diagnostics::codes::DefaultParamOrder},
    {"E171", diagnostics::codes::DeleteNotAutoGenerated},
    {"E172", diagnostics::codes::DeleteWithBody},
    {"E173", diagnostics::codes::DeleteWithOtherQualifier},
    {"E174", diagnostics::codes::DeletedMethodCalled},
    {"E175", diagnostics::codes::DestructorDelete},
    {"E176", diagnostics::codes::DestructorParam},
    {"E177", diagnostics::codes::DestructorReturnType},
    {"E178", diagnostics::codes::DoubleReference},
    {"E179", diagnostics::codes::DuplicateEnumMember},
    {"E180", diagnostics::codes::DuplicateParam},
    {"E181", diagnostics::codes::EmptyListElement},
    {"E182", diagnostics::codes::EnumInvalidInitializer},
    {"E183", diagnostics::codes::ExplicitNotMember},
    {"E184", diagnostics::codes::ExpressionIsDataType},
    {"E185", diagnostics::codes::ForeachUnsupported},
    {"E186", diagnostics::codes::FuncdefAttribute},
    {"E187", diagnostics::codes::FuncdefNotHandle},
    {"E188", diagnostics::codes::GlobalFunctionQualifiers},
    {"E189", diagnostics::codes::GlobalVariableAccessModifier},
    {"E190", diagnostics::codes::ImportHasBody},
    {"E191", diagnostics::codes::IncDecOnVirtualProp},
    {"E192", diagnostics::codes::InheritFinal},
    {"E193", diagnostics::codes::InitializerListExpected},
    {"E194", diagnostics::codes::InitializerListNotSupported},
    {"E195", diagnostics::codes::InitializerListTooFew},
    {"E196", diagnostics::codes::InitializerListTooMany},
    {"E197", diagnostics::codes::InterfaceConstructor},
    {"E198", diagnostics::codes::InterfaceImplMissing},
    {"E199", diagnostics::codes::InterfaceInstantiated},
    {"E200", diagnostics::codes::InterfaceMethodAttribute},
    {"E201", diagnostics::codes::InvalidCast},
    {"E202", diagnostics::codes::InvalidForeachContainer},
    {"E203", diagnostics::codes::MissingBody},
    {"E204", diagnostics::codes::MixinAbstract},
    {"E205", diagnostics::codes::MixinChildType},
    {"E206", diagnostics::codes::MixinConstructor},
    {"E207", diagnostics::codes::MixinDestructor},
    {"E208", diagnostics::codes::MixinInheritClass},
    {"E209", diagnostics::codes::MixinVirtualProperty},
    {"E210", diagnostics::codes::MixinInstantiationMemberNotFound},
    {"E211", diagnostics::codes::MultiClassInherit},
    {"E212", diagnostics::codes::MultilineString},
    {"E213", diagnostics::codes::NameConflict},
    {"E214", diagnostics::codes::NamedArgumentSyntax},
    {"E215", diagnostics::codes::NoDefaultConstructor},
    {"E216", diagnostics::codes::NoExplicitConversion},
    {"E217", diagnostics::codes::NoMatchingOperator},
    {"E218", diagnostics::codes::NullNonHandle},
    {"E219", diagnostics::codes::OpOverloadGlobal},
    {"E220", diagnostics::codes::OpcmpReturnInt},
    {"E221", diagnostics::codes::OpequalsReturnBool},
    {"E222", diagnostics::codes::OpindexNoParams},
    {"E223", diagnostics::codes::OutParamDefault},
    {"E224", diagnostics::codes::OverrideFinalMethod},
    {"E225", diagnostics::codes::OverrideNoBase},
    {"E226", diagnostics::codes::ParameterNotInstantiable},
    {"E227", diagnostics::codes::PropertyAccessorMissingBody},
    {"E228", diagnostics::codes::PropertyDuplicateAccessor},
    {"E229", diagnostics::codes::PropertyTypeMismatch},
    {"E230", diagnostics::codes::ReadOnlyProperty},
    {"E231", diagnostics::codes::RefTypeBoolConvDisallowed},
    {"E232", diagnostics::codes::ReservedWordAsParameterName},
    {"E233", diagnostics::codes::ReturnNotInstantiable},
    {"E234", diagnostics::codes::SignatureMismatchFuncHandle},
    {"E235", diagnostics::codes::StandaloneAnonymousFunction},
    {"E236", diagnostics::codes::TemplateClassNotSupported},
    {"E237", diagnostics::codes::ValueAssignForRef},
    {"E238", diagnostics::codes::VirtualPropertySignature},
    {"E239", diagnostics::codes::VoidParameter},
    {"E240", diagnostics::codes::VoidReference},
    {"E241", diagnostics::codes::VoidReturnValue},
    {"E242", diagnostics::codes::WriteOnlyProperty},
    {"E243", diagnostics::codes::IllegalOperation},
    {"E244", diagnostics::codes::BaseNotFound},
    {"E245", diagnostics::codes::InvalidReferenceReturn},
    {"E246", diagnostics::codes::ReadonlyHandle},
    {"E247", diagnostics::codes::StandaloneReference},
    {"E248", diagnostics::codes::ReadonlyReference},
    {"E249", diagnostics::codes::GlobalFunctionAttribute},
    {"E250", diagnostics::codes::VariableInSwitchCase},
    {"E251", diagnostics::codes::EmptySwitchStatement},
    {"E252", diagnostics::codes::SwitchConditionNotIntegral},
    {"E253", diagnostics::codes::TypeNotIndexable},
    {"E254", diagnostics::codes::NoMathConversion},
    {"E255", diagnostics::codes::IdentityRequiresHandles},
    {"E256", diagnostics::codes::ClassFinalAbstractConflict},
    {"E257", diagnostics::codes::AutoNotAllowedHere},
    {"E258", diagnostics::codes::HandleToHandle},
    {"E259", diagnostics::codes::CannotInferLambda},
    {"E260", diagnostics::codes::TemplateArgumentCountMismatch},
    {"E261", diagnostics::codes::HandleOnNoHandleType},
    {"E262", diagnostics::codes::DefaultArgUnresolved},
    {"E263", diagnostics::codes::IncludeUnterminated},
};

std::string_view GetDiagnosticAlias(std::string_view canonicalCode) noexcept
{
    for (const auto& entry : k_codeAliases)
    {
        if (entry.code == canonicalCode)
            return entry.alias;
    }
    return {};
}

std::string_view GetCanonicalDiagnosticCode(std::string_view aliasOrCode) noexcept
{
    for (const auto& entry : k_codeAliases)
    {
        if (utils::CaseInsensitiveEquals(entry.alias, aliasOrCode) ||
            utils::CaseInsensitiveEquals(entry.code, aliasOrCode))
            return entry.code;
    }
    return aliasOrCode;
}

std::string_view NormalizeDiagnosticSelector(std::string_view selector) noexcept
{
    constexpr std::pair<std::string_view, std::string_view> groups[] = {
        {"unused", "as-warn-unused"},
        {"engine", "as-warn-engine"},
        {"conversion", "as-warn-conversion"},
        {parser::keywords::Null, "as-warn-null"},
        {"shadow", "as-warn-shadow"},
        {"hints", "as-hint"},
        {"warnings", "as-warn"},
        {"all", "all"},
    };
    for (const auto& [name, alias] : groups)
    {
        if (utils::CaseInsensitiveEquals(name, selector) || utils::CaseInsensitiveEquals(alias, selector))
        {
            return name;
        }
    }
    return GetCanonicalDiagnosticCode(selector);
}

std::string_view GetDiagnosticGroup(std::string_view code) noexcept
{
    code = GetCanonicalDiagnosticCode(code);
    if (code.starts_with("as-warn-unused-"))
        return "unused";
    if (code.starts_with("as-warn-engine-") || code == diagnostics::codes::EngineTypeSuggestion)
        return "engine";
    if (code == diagnostics::codes::SignedUnsignedMismatch || code == diagnostics::codes::FloatTruncation ||
        code == diagnostics::codes::BoolConversion || code == diagnostics::codes::RepeatedConversion)
        return "conversion";
    if (code == diagnostics::codes::PossibleNullDereference)
        return parser::keywords::Null;
    if (code == diagnostics::codes::ShadowGlobal)
        return "shadow";
    return {};
}

std::string_view GetDiagnosticCategory(std::string_view code) noexcept
{
    code = GetCanonicalDiagnosticCode(code);
    if (code.starts_with("as-warn-"))
        return "warnings";
    if (code.starts_with("as-hint-"))
        return "hints";
    return {};
}

std::string_view FormatDiagnosticDisplayCode(std::string_view canonicalCode) noexcept
{
    const std::string_view alias = GetDiagnosticAlias(canonicalCode);
    return alias.empty() ? canonicalCode : alias;
}

} // namespace angel_lsp::analysis
