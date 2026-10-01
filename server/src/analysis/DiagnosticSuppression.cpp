#include "analysis/DiagnosticSuppression.h"
#include "analysis/DiagnosticCodes.h"
#include "analysis/NodeIndex.h"
#include "parser/GrammarNames.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <unordered_map>

namespace angel_lsp::analysis
{
namespace
{
struct CodeAliasEntry
{
    std::string_view alias;
    std::string_view code;
};

inline constexpr CodeAliasEntry k_codeAliases[] = {
    // Warnings
    {"W101", diagnostics::codes::UninitializedVariableRead},
    {"W102", diagnostics::codes::UnreachableCode},
    {"W103", diagnostics::codes::UnusedVariable},
    {"W105", diagnostics::codes::SignedUnsignedMismatch},
    {"W106", diagnostics::codes::FloatTruncation},
    {"W107", diagnostics::codes::UnsupportedDirective},
    {"W108", diagnostics::codes::UndeclaredIdentifier},
    {"W109", diagnostics::codes::PossibleNullDereference},
    {"W110", diagnostics::codes::HandleComparisonEquality},
    {"W111", diagnostics::codes::GlobalFunctionAttribute},
    {"W112", diagnostics::codes::IncludeNotFound},
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
    {"E154", diagnostics::codes::BinaryOperatorArity},
    {"E155", diagnostics::codes::CannotInferNull},
    {"E156", diagnostics::codes::CannotInferVoid},
    {"E157", diagnostics::codes::CannotReturnLocalRef},
    {"E158", diagnostics::codes::CannotReturnParamRef},
    {"E159", diagnostics::codes::CharacterLiteralIsString},
    {"E160", diagnostics::codes::CircularInherit},
    {"E161", diagnostics::codes::ClassMemberConst},
    {"E162", diagnostics::codes::CompoundAssignOnIndexedProp},
    {"E163", diagnostics::codes::CompoundAssignOnValueProp},
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
};

bool EqualsCaseInsensitive(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

std::string_view StripCommentDelimiters(std::string_view text) noexcept
{
    if (text.starts_with("//"))
        text.remove_prefix(2);
    else if (text.starts_with("/*"))
        text.remove_prefix(2);

    if (text.ends_with("*/"))
        text.remove_suffix(2);

    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    return text;
}

std::vector<std::string_view> TokenizeDirective(std::string_view text)
{
    std::vector<std::string_view> tokens;
    size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ',' || text[i] == ';'))
            ++i;
        if (i >= text.size())
            break;
        size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])) && text[i] != ',' && text[i] != ';')
            ++i;
        tokens.push_back(text.substr(start, i - start));
    }
    return tokens;
}
} // namespace

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
        if (EqualsCaseInsensitive(entry.alias, aliasOrCode) || EqualsCaseInsensitive(entry.code, aliasOrCode))
            return entry.code;
    }
    return aliasOrCode;
}

std::string FormatDiagnosticDisplayCode(std::string_view canonicalCode)
{
    std::string_view alias = GetDiagnosticAlias(canonicalCode);
    return alias.empty() ? std::string(canonicalCode) : std::string(alias);
}

void DiagnosticSuppressionMap::AddSuppression(std::string_view code, uint32_t startLine, uint32_t endLine)
{
    m_suppressions.push_back({std::string(code), startLine, endLine});
}

bool DiagnosticSuppressionMap::IsSuppressed(std::string_view code, uint32_t line) const
{
    std::string_view canonical = GetCanonicalDiagnosticCode(code);
    for (const auto& s : m_suppressions)
    {
        if (line >= s.startLine && line <= s.endLine)
        {
            if (s.code == "all" || s.code == code || s.code == canonical)
                return true;
        }
    }
    return false;
}

namespace
{
void ProcessCommentDirective(std::string_view rawText, uint32_t line, DiagnosticSuppressionMap& map,
                             std::unordered_map<std::string, std::vector<uint32_t>>& openRanges)
{
    std::string_view content = StripCommentDelimiters(rawText);
    auto tokens = TokenizeDirective(content);
    if (tokens.empty())
        return;

    std::string_view action = tokens[0];
    const bool isDisableLine = EqualsCaseInsensitive(action, "disable-line");
    const bool isDisableNextLine = EqualsCaseInsensitive(action, "disable-next-line");
    const bool isDisable = EqualsCaseInsensitive(action, "disable");
    const bool isEnable = EqualsCaseInsensitive(action, "enable");

    if (!isDisableLine && !isDisableNextLine && !isDisable && !isEnable)
        return;

    for (size_t k = 1; k < tokens.size(); ++k)
    {
        std::string codeKey = EqualsCaseInsensitive(tokens[k], "all")
                                  ? "all"
                                  : std::string(GetCanonicalDiagnosticCode(tokens[k]));
        if (isDisableLine)
        {
            map.AddSuppression(codeKey, line, line);
        }
        else if (isDisableNextLine)
        {
            map.AddSuppression(codeKey, line + 1, line + 1);
        }
        else if (isDisable)
        {
            openRanges[codeKey].push_back(line);
        }
        else if (isEnable)
        {
            auto it = openRanges.find(codeKey);
            if (it != openRanges.end() && !it->second.empty())
            {
                uint32_t start = it->second.back();
                it->second.pop_back();
                map.AddSuppression(codeKey, start, line > 0 ? line - 1 : 0);
            }
        }
    }
}

void ScanFallbackComments(std::string_view sourceCode, DiagnosticSuppressionMap& map,
                          std::unordered_map<std::string, std::vector<uint32_t>>& openRanges)
{
    uint32_t line = 0;
    size_t pos = 0;
    while (pos < sourceCode.size())
    {
        size_t nextPos = sourceCode.find('\n', pos);
        if (nextPos == std::string_view::npos)
            nextPos = sourceCode.size();
        std::string_view lineStr = sourceCode.substr(pos, nextPos - pos);
        size_t commentIdx = lineStr.find("//");
        if (commentIdx != std::string_view::npos)
            ProcessCommentDirective(lineStr.substr(commentIdx), line, map, openRanges);
        pos = nextPos + 1;
        ++line;
    }
}
} // namespace

DiagnosticSuppressionMap ParseDiagnosticSuppressions(std::string_view sourceCode, const NodeIndex* nodeIndex)
{
    DiagnosticSuppressionMap map;
    std::unordered_map<std::string, std::vector<uint32_t>> openRanges;

    if (nodeIndex)
    {
        for (TSNode cNode : nodeIndex->Nodes(parser::nodes::Comment))
        {
            uint32_t startByte = ts_node_start_byte(cNode);
            uint32_t endByte = ts_node_end_byte(cNode);
            if (startByte < sourceCode.size() && endByte <= sourceCode.size() && startByte <= endByte)
            {
                std::string_view commentText = sourceCode.substr(startByte, endByte - startByte);
                ProcessCommentDirective(commentText, ts_node_start_point(cNode).row, map, openRanges);
            }
        }
    }
    else
    {
        ScanFallbackComments(sourceCode, map, openRanges);
    }

    for (const auto& [codeKey, starts] : openRanges)
    {
        for (uint32_t start : starts)
            map.AddSuppression(codeKey, start, UINT32_MAX);
    }

    return map;
}

} // namespace angel_lsp::analysis
