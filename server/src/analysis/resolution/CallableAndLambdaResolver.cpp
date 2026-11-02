#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/index/ScopeTree.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/OverloadResolver.h"
#include "analysis/resolution/OverloadTypeConversions.h"
#include "analysis/resolution/SemanticHelpers.h"
#include "analysis/resolution/TemplateTypeResolver.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/ASTUtils.h"
#include "parser/GrammarSymbols.h"
#include "parser/Primitives.h"
#include "utils/ScopedNameUtils.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{
/**
 * @brief Resolves argument_list node from either argument_list or call_expression.
 * @param[in] node AST node.
 * @return argument_list node or null node.
 */
TSNode ResolveArgumentListNode(TSNode node)
{
    if (ts_node_is_null(node))
    {
        return TSNode{};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symArgumentList)
    {
        return node;
    }
    if (sym == gs.symCallExpression || sym == gs.symConstructCallExpression || sym == gs.symVariableDeclarator)
    {
        TSNode args = parser::GetChildByField(node, gs.fieldArguments);
        if (!ts_node_is_null(args))
        {
            return args;
        }
        TSTreeCursor cursor = ts_tree_cursor_new(node);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                TSNode ch = ts_tree_cursor_current_node(&cursor);
                if (ts_node_symbol(ch) == gs.symArgumentList)
                {
                    ts_tree_cursor_delete(&cursor);
                    return ch;
                }
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
            ts_tree_cursor_delete(&cursor);
        }
    }
    return TSNode{};
}

void ProcessCallArgumentChild(TSNode child, TSFieldId fieldId, std::string_view sourceCode, CallArgumentInfo& current)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (fieldId == gs.fieldArgName)
    {
        current.nameNode = child;
        if (!sourceCode.empty())
        {
            current.name = parser::GetNodeText(child, sourceCode);
        }
        return;
    }

    current.exprNode = child;
}

/**
 * @brief Structurally parses arguments from an argument_list AST node into a list of CallArgumentInfo.
 * @param[in] argumentList AST node representing an argument list or call expression.
 * @param[in] sourceCode Document source text buffer.
 * @return Vector of extracted argument information including expressions and explicit names.
 */
std::vector<CallArgumentInfo> ExtractCallArguments(TSNode argumentList, std::string_view sourceCode)
{
    TSNode targetList = ResolveArgumentListNode(argumentList);
    if (ts_node_is_null(targetList))
    {
        return {};
    }

    std::vector<CallArgumentInfo> result;
    TSTreeCursor cursor = ts_tree_cursor_new(targetList);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return result;
    }

    CallArgumentInfo current;
    current.index = 0;
    bool inArgument = false;

    do
    {
        TSNode child = ts_tree_cursor_current_node(&cursor);
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol sym = ts_node_symbol(child);

        if (sym == gs.tokOpenParen || sym == gs.symComment || sym == gs.tokColon)
        {
            continue;
        }
        if (sym == gs.tokCloseParen)
        {
            break;
        }
        if (sym == gs.tokComma)
        {
            if (inArgument)
            {
                result.push_back(std::move(current));
                current = CallArgumentInfo{};
                current.index = static_cast<uint32_t>(result.size());
                inArgument = false;
            }
            continue;
        }

        if (inArgument && !ts_node_is_null(current.exprNode))
        {
            result.push_back(std::move(current));
            current = CallArgumentInfo{};
            current.index = static_cast<uint32_t>(result.size());
        }

        const TSFieldId fieldId = ts_tree_cursor_current_field_id(&cursor);
        ProcessCallArgumentChild(child, fieldId, sourceCode, current);
        inArgument = true;

    } while (ts_tree_cursor_goto_next_sibling(&cursor));

    ts_tree_cursor_delete(&cursor);

    if (inArgument)
    {
        result.push_back(std::move(current));
    }

    return result;
}

size_t CountCallArguments(TSNode argumentList)
{
    return ExtractCallArguments(argumentList, "").size();
}

static bool IsConstArgumentNode(TSNode exprNode, const ExpressionTypeContext& ctx)
{
    if (ts_node_is_null(exprNode))
    {
        return false;
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(exprNode);
    if (sym != gs.symIdentifier && sym != gs.symScopedIdentifier)
    {
        return false;
    }
    const std::string name = GetTrimmedNodeText(exprNode, ctx.sourceCode);
    if (ctx.scope)
    {
        if (const auto* def = ResolveInScopeAt(ctx.scope, name, ts_node_start_byte(exprNode)))
        {
            if (!def->typeName.empty() && HasConstModifier(def->typeName))
            {
                return true;
            }
        }
    }
    if (const auto syms = ctx.symbolTable.FindSymbolsPtr(name))
    {
        for (const auto& foundSym : *syms)
        {
            if ((foundSym.type == SymbolType::Variable || foundSym.type == SymbolType::Property) &&
                foundSym.GetVariable().modifiers.isConst)
            {
                return true;
            }
        }
    }
    return false;
}

std::vector<std::string> ExtractCallArgumentTypes(TSNode callNode, const ExpressionTypeContext& ctx)
{
    auto args = ExtractCallArguments(callNode, ctx.sourceCode);
    std::vector<std::string> types;
    types.reserve(args.size());
    for (const auto& arg : args)
    {
        if (ts_node_is_null(arg.exprNode))
        {
            types.push_back("");
        }
        else
        {
            std::string argType = ResolveExpressionType(arg.exprNode, ctx);
            if (!argType.empty() && !HasConstModifier(argType) && IsConstArgumentNode(arg.exprNode, ctx))
            {
                argType.insert(0, "const ");
            }
            types.push_back(std::move(argType));
        }
    }
    return types;
}

std::vector<Symbol> CollectConstructorCandidates(const std::string& baseName, TSNode contextNode,
                                                 std::string_view sourceCode, const SymbolTable& symbolTable)
{
    std::vector<Symbol> candidateSymbols;
    auto ctorSyms = symbolTable.FindMemberSymbols(baseName, LastScopeSegment(baseName));
    for (const auto& s : ctorSyms)
    {
        if (s.type == SymbolType::Function)
        {
            candidateSymbols.push_back(s);
        }
    }

    if (candidateSymbols.empty())
    {
        auto scopeSyms = FindSymbolsInScope(baseName, contextNode, sourceCode, symbolTable);
        for (const auto& s : scopeSyms)
        {
            if (s.type == SymbolType::Class)
            {
                std::string qName = s.qualifiedName.empty() ? s.name : s.qualifiedName;
                auto qCtors = symbolTable.FindMemberSymbols(qName, s.name);
                for (const auto& cs : qCtors)
                {
                    if (cs.type == SymbolType::Function)
                    {
                        candidateSymbols.push_back(cs);
                    }
                }
            }
        }
    }

    if (candidateSymbols.empty())
    {
        auto fnSyms = symbolTable.FindSymbols(baseName);
        for (const auto& s : fnSyms)
        {
            if (s.type == SymbolType::Function)
            {
                candidateSymbols.push_back(s);
            }
        }
    }

    return candidateSymbols;
}

bool StepCommentState(std::string_view code, size_t& i, size_t cursorByte, CallDelimiterState& state)
{
    if (state.inLineComment)
    {
        if (code[i] == '\n')
        {
            state.inLineComment = false;
        }
        return true;
    }

    if (state.inBlockComment)
    {
        if (code[i] == '*' && i + 1 < cursorByte && code[i + 1] == '/')
        {
            state.inBlockComment = false;
            ++i;
        }
        return true;
    }

    if (code[i] == '/' && i + 1 < cursorByte)
    {
        if (code[i + 1] == '/')
        {
            state.inLineComment = true;
            ++i;
            return true;
        }
        if (code[i + 1] == '*')
        {
            state.inBlockComment = true;
            ++i;
            return true;
        }
    }
    return false;
}

bool StepStringState(std::string_view code, size_t& i, size_t cursorByte, CallDelimiterState& state)
{
    if (state.inString)
    {
        if (code[i] == '\\' && i + 1 < cursorByte)
        {
            ++i;
        }
        else if (code[i] == state.stringChar)
        {
            state.inString = false;
        }
        return true;
    }

    if (code[i] == '"' || code[i] == '\'')
    {
        state.inString = true;
        state.stringChar = code[i];
        return true;
    }
    return false;
}

void StepDelimiterDepth(char c, CallDelimiterState& state, uint32_t& activeParam)
{
    switch (c)
    {
    case '(':
        ++state.parenDepth;
        break;
    case ')':
        if (state.parenDepth > 0)
            --state.parenDepth;
        break;
    case '[':
        ++state.bracketDepth;
        break;
    case ']':
        if (state.bracketDepth > 0)
            --state.bracketDepth;
        break;
    case '{':
        ++state.braceDepth;
        break;
    case '}':
        if (state.braceDepth > 0)
            --state.braceDepth;
        break;
    case '<':
        ++state.angleDepth;
        break;
    case '>':
        if (state.angleDepth > 0)
            --state.angleDepth;
        break;
    case ',':
        if (state.AtTopLevel())
        {
            ++activeParam;
        }
        break;
    default:
        break;
    }
}

uint32_t CalculateActiveCallParameter(TSNode argListNode, size_t cursorByte, std::string_view sourceCode)
{
    TSNode targetList = ResolveArgumentListNode(argListNode);
    if (ts_node_is_null(targetList) || sourceCode.empty())
    {
        return 0;
    }

    uint32_t startByte = ts_node_start_byte(targetList);
    uint32_t endByte = ts_node_end_byte(targetList);
    if (cursorByte <= startByte)
    {
        return 0;
    }

    if (startByte < sourceCode.size() && sourceCode[startByte] == '(')
    {
        ++startByte;
    }

    const size_t bound = std::min(cursorByte, static_cast<size_t>(endByte));
    uint32_t activeParam = 0;
    CallDelimiterState state;

    for (size_t i = startByte; i < bound; ++i)
    {
        if (StepCommentState(sourceCode, i, bound, state) || StepStringState(sourceCode, i, bound, state))
        {
            continue;
        }
        StepDelimiterDepth(sourceCode[i], state, activeParam);
    }

    return activeParam;
}

// --- A lambda against the funcdef it is being handed to -------------------------------

std::string LastSegmentOf(const std::string& name)
{
    return std::string(utils::LastScopeSegment(name));
}

/** @brief True for a name that denotes a typedef, whose alias no spelling can see through. */
bool NamesATypedef(const std::string& name, const SymbolTable& table)
{
    const auto bucket = table.FindSymbolsPtr(name);
    if (!bucket)
    {
        return false;
    }
    for (const auto& sym : *bucket)
    {
        if (sym.type == SymbolType::Typedef)
        {
            return true;
        }
    }
    return false;
}

bool IsLambdaExpression(TSNode node) noexcept
{
    if (ts_node_is_null(node))
    {
        return false;
    }
    return ts_node_symbol(node) == parser::GetGrammarSymbols().symLambdaExpression;
}

/**
 * @brief Resolves lambda_parameter_list node from either parameter list or lambda expression.
 * @param[in] node AST node.
 * @return lambda_parameter_list node or null node.
 */
static TSNode ResolveLambdaParameterListNode(TSNode node)
{
    if (ts_node_is_null(node))
    {
        return TSNode{};
    }
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symLambdaParameterList)
    {
        return node;
    }
    if (sym == gs.symLambdaExpression)
    {
        TSNode params = parser::GetChildByField(node, gs.fieldParameters);
        if (!ts_node_is_null(params))
        {
            return params;
        }
        TSTreeCursor cursor = ts_tree_cursor_new(node);
        if (ts_tree_cursor_goto_first_child(&cursor))
        {
            do
            {
                TSNode ch = ts_tree_cursor_current_node(&cursor);
                if (ts_node_symbol(ch) == gs.symLambdaParameterList)
                {
                    ts_tree_cursor_delete(&cursor);
                    return ch;
                }
            } while (ts_tree_cursor_goto_next_sibling(&cursor));
            ts_tree_cursor_delete(&cursor);
        }
    }
    return TSNode{};
}

/**
 * @brief Inspects a child node of a lambda parameter declaration and updates the parameter info.
 * @param[in] child Child AST node within the parameter declaration.
 * @param[in] fieldId Structural field ID associated with the child node.
 * @param[in] sourceCode Document source text buffer.
 * @param[in,out] current Lambda parameter metadata struct being populated.
 */
void ApplyLambdaParamChild(TSNode child, TSFieldId fieldId, std::string_view sourceCode, LambdaParamASTInfo& current)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol sym = ts_node_symbol(child);
    if (fieldId == gs.fieldParamType)
    {
        current.typeNode = child;
        current.hasWrittenType = true;
        std::string_view text = parser::GetNodeTextView(child, sourceCode);
        current.isConst = IsConstTypeView(text);
        current.isHandle = IsHandleTypeView(text);
        current.typeName = CleanBaseType(text);
    }
    else if (sym == gs.tokAmp)
    {
        current.isReference = true;
    }
    else if (sym == gs.tokIn || sym == gs.tokOut || sym == gs.tokInout)
    {
        current.modifier = (sym == gs.tokIn)    ? ParameterModifier::In
                           : (sym == gs.tokOut) ? ParameterModifier::Out
                                                : ParameterModifier::InOut;
    }
    else if (fieldId == gs.fieldName || sym == gs.symIdentifier)
    {
        current.nameNode = child;
        current.name = parser::GetNodeText(child, sourceCode);
    }
}

/**
 * @brief Structurally parses lambda parameter declarations from AST into LambdaParamASTInfo structs.
 * @param[in] lambdaParamList AST node representing a lambda parameter list.
 * @param[in] sourceCode Document source text buffer.
 * @return Ordered vector of extracted lambda parameter AST definitions.
 */
std::vector<LambdaParamASTInfo> ExtractLambdaParameters(TSNode lambdaParamList, std::string_view sourceCode)
{
    TSNode targetList = ResolveLambdaParameterListNode(lambdaParamList);
    if (ts_node_is_null(targetList))
    {
        return {};
    }

    std::vector<LambdaParamASTInfo> result;
    TSTreeCursor cursor = ts_tree_cursor_new(targetList);
    if (!ts_tree_cursor_goto_first_child(&cursor))
    {
        ts_tree_cursor_delete(&cursor);
        return result;
    }

    LambdaParamASTInfo current;
    current.index = 0;
    bool inParam = false;

    do
    {
        TSNode child = ts_tree_cursor_current_node(&cursor);
        const auto& gs = parser::GetGrammarSymbols();
        const TSSymbol sym = ts_node_symbol(child);

        if (sym == gs.tokOpenParen || sym == gs.symComment)
        {
            continue;
        }
        if (sym == gs.tokCloseParen)
        {
            break;
        }
        if (sym == gs.tokComma)
        {
            if (inParam)
            {
                result.push_back(std::move(current));
                current = LambdaParamASTInfo{};
                current.index = static_cast<uint32_t>(result.size());
                inParam = false;
            }
            continue;
        }

        if (ts_node_is_null(current.startNode))
        {
            current.startNode = child;
        }
        inParam = true;

        const TSFieldId fieldId = ts_tree_cursor_current_field_id(&cursor);
        ApplyLambdaParamChild(child, fieldId, sourceCode, current);

    } while (ts_tree_cursor_goto_next_sibling(&cursor));

    ts_tree_cursor_delete(&cursor);

    if (inParam)
    {
        result.push_back(std::move(current));
    }

    return result;
}

std::vector<LambdaParameter> ReadLambdaParameters(TSNode listNode, std::string_view sourceCode)
{
    std::vector<LambdaParameter> parameters;
    auto astParams = ExtractLambdaParameters(listNode, sourceCode);
    parameters.reserve(astParams.size());
    for (const auto& p : astParams)
    {
        LambdaParameter lp;
        lp.typeName = p.typeName;
        lp.hasWrittenType = p.hasWrittenType;
        lp.isConst = p.isConst;
        lp.isHandle = p.isHandle;
        lp.isReference = p.isReference;
        lp.modifier = p.modifier;
        parameters.push_back(std::move(lp));
    }
    return parameters;
}

bool LambdaContradictsFuncdef(const std::vector<LambdaParameter>& lambdaParameters, const FuncdefSignature& funcdefSig,
                              const SymbolTable& table)
{
    if (lambdaParameters.size() != funcdefSig.parameters.size())
    {
        return true;
    }

    for (size_t i = 0; i < lambdaParameters.size(); ++i)
    {
        const LambdaParameter& written = lambdaParameters[i];
        if (!written.hasWrittenType)
        {
            continue;
        }

        const ParameterInformation& expected = funcdefSig.parameters[i];
        if (written.isHandle != expected.isHandle || written.isReference != expected.isReference ||
            written.modifier != expected.modifier)
        {
            return true;
        }

        // Compared by last `::` segment, so a name the funcdef writes bare and the lambda
        // writes qualified - or the other way round - is the same name. That costs the
        // `A::Foo` against `B::Foo` case, which is a rejection this misses rather than a legal
        // program it reports.
        const std::string writtenBase(CanonicalizeType(LastSegmentOf(written.typeName)));
        const std::string expectedBase(CanonicalizeType(LastSegmentOf(CleanBaseType(expected.typeName))));

        if (writtenBase.empty() || expectedBase.empty() || writtenBase == expectedBase)
        {
            continue;
        }
        if (NamesATypedef(writtenBase, table) || NamesATypedef(expectedBase, table))
        {
            continue;
        }
        return true;
    }

    return false;
}

bool LambdaContradictsFuncdef(TSNode lambdaNode, const FuncdefSignature& funcdefSig, const SymbolTable& table,
                              std::string_view sourceCode)
{
    TSNode listNode = parser::GetChildByField(lambdaNode, parser::GetGrammarSymbols().fieldParameters);
    if (ts_node_is_null(listNode))
    {
        // No parameter list to read is not the same as an empty one, and guessing which it is
        // would be guessing about the whole signature.
        return false;
    }
    return LambdaContradictsFuncdef(ReadLambdaParameters(listNode, sourceCode), funcdefSig, table);
}

std::optional<Symbol> FindFuncdefSymbol(std::string_view typeName, const SymbolTable& table)
{
    if (typeName.empty() || parser::primitives::IsPrimitive(typeName))
    {
        return std::nullopt;
    }

    if (const auto bucket = table.FindSymbolsPtr(typeName))
    {
        for (const auto& sym : *bucket)
        {
            if (sym.type == SymbolType::Funcdef)
            {
                return sym;
            }
        }
    }

    const std::string_view bare = LastScopeSegment(typeName);
    const auto matches = table.FindTypeSymbolsByShortName(bare);
    for (const auto& sym : matches)
    {
        if (sym.type == SymbolType::Funcdef)
        {
            return sym;
        }
    }
    if (const auto bucket = table.FindSymbolsPtr(bare))
    {
        for (const auto& sym : *bucket)
        {
            if (sym.type == SymbolType::Funcdef)
            {
                return sym;
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Is handle type.
 * @param[in] typeName Type name to inspect or resolve.
 * @param[in] symbolTable Symbol or grammar symbol.
 * @return True if the condition is satisfied; false otherwise.
 */
bool IsHandleType(std::string_view typeName, const SymbolTable& symbolTable) noexcept
{
    if (typeName.empty())
    {
        return false;
    }
    if (IsHandleTypeView(typeName))
    {
        return true;
    }
    const std::string clean = CleanBaseType(typeName);
    return !clean.empty() && FindFuncdefSymbol(clean, symbolTable).has_value();
}

/**
 * @brief Finds the target funcdef symbol for a variable declarator.
 */
static std::optional<Symbol> FindDeclaratorFuncdefTarget(TSNode parent, const SymbolTable& table,
                                                         std::string_view sourceCode)
{
    TSNode declaration = parser::GetParent(parent);
    if (!ts_node_is_null(declaration))
    {
        TSNode typeNode = parser::GetChildByField(declaration, parser::GetGrammarSymbols().fieldVarType);
        if (ts_node_is_null(typeNode))
        {
            typeNode = parser::GetChildByField(declaration, parser::GetGrammarSymbols().fieldType);
        }
        if (!ts_node_is_null(typeNode))
        {
            return FindFuncdefSymbol(CleanBaseType(parser::GetNodeText(typeNode, sourceCode)), table);
        }
    }
    return std::nullopt;
}

/**
 * @brief Locates the index of targetChild within parent's named children.
 */
static std::optional<uint32_t> FindChildPosition(TSNode parent, TSNode targetChild)
{
    for (uint32_t i = 0; i < ts_node_named_child_count(parent); ++i)
    {
        if (ts_node_eq(ts_node_named_child(parent, i), targetChild))
        {
            return i;
        }
    }
    return std::nullopt;
}

/**
 * @brief Extracts the callee syntax node from a call expression node.
 */
static TSNode ExtractCallCalleeNode(TSNode call)
{
    return parser::GetCallCallee(call);
}

/**
 * @brief Matches candidate function parameters at position against funcdef symbols.
 */
static bool MatchCandidateFuncdef(const SymbolTable& table, std::string_view name, uint32_t position,
                                  FuncdefAgreement& agreement)
{
    if (agreement.conflict)
    {
        return false;
    }
    const auto bucket = table.FindSymbolsPtr(name);
    if (!bucket)
    {
        return false;
    }
    for (const auto& sym : *bucket)
    {
        if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
        {
            continue;
        }
        const auto& parameters = sym.GetFunction().parameters;
        if (position >= parameters.size())
        {
            continue;
        }
        auto funcdef = FindFuncdefSymbol(CleanBaseType(parameters[position].typeName), table);
        if (!funcdef)
        {
            continue;
        }
        if (agreement.sawCandidate && agreement.agreed && agreement.agreed->name != funcdef->name)
        {
            agreement.agreed = std::nullopt;
            agreement.conflict = true;
            return false;
        }
        agreement.agreed = std::move(funcdef);
        agreement.sawCandidate = true;
    }
    return agreement.sawCandidate && !agreement.conflict;
}

/**
 * @brief Finds the target funcdef symbol for an assignment expression.
 */
static std::optional<Symbol> FindAssignmentFuncdefTarget(TSNode parent, const SymbolTable& table,
                                                         std::string_view sourceCode)
{
    TSNode leftNode = parser::GetChildByField(parent, parser::GetGrammarSymbols().fieldLeft);
    if (ts_node_is_null(leftNode))
    {
        return std::nullopt;
    }
    std::string varName = CleanBaseType(parser::GetNodeText(leftNode, sourceCode));
    const size_t dot = varName.rfind('.');
    if (dot != std::string::npos)
    {
        varName = varName.substr(dot + 1);
    }
    const auto syms = table.FindSymbolsPtr(varName);
    if (!syms)
    {
        return std::nullopt;
    }
    for (const auto& s : *syms)
    {
        if (s.type == SymbolType::Variable && std::holds_alternative<VariableSignature>(s.signature))
        {
            if (auto funcdef = FindFuncdefSymbol(CleanBaseType(s.GetVariable().typeName), table))
            {
                return funcdef;
            }
        }
    }
    return std::nullopt;
}

/**
 * @brief Resolves target funcdef symbol across possible callee member/function names.
 */
static std::optional<Symbol> FindCallArgumentFuncdef(const SymbolTable& table, std::string_view calleeName,
                                                     uint32_t position)
{
    FuncdefAgreement agreement;
    auto lookUp = [&](std::string_view name) -> bool
    { return MatchCandidateFuncdef(table, name, position, agreement); };

    if (lookUp(calleeName))
    {
        return agreement.agreed;
    }
    if (agreement.conflict)
    {
        return std::nullopt;
    }

    const size_t dot = calleeName.rfind('.');
    if (dot != std::string_view::npos && lookUp(calleeName.substr(dot + 1)))
    {
        return agreement.agreed;
    }
    if (agreement.conflict)
    {
        return std::nullopt;
    }

    if (!utils::ContainsScopeSeparator(calleeName) && lookUp(utils::JoinScopedName(calleeName, calleeName)))
    {
        return agreement.agreed;
    }
    if (agreement.conflict)
    {
        return std::nullopt;
    }

    const std::string_view leaf = utils::LastScopeSegment(calleeName);
    if (leaf != calleeName && lookUp(leaf))
    {
        return agreement.agreed;
    }

    return std::nullopt;
}

std::optional<Symbol> FuncdefTargetOfLambda(TSNode lambdaNode, const SymbolTable& table, std::string_view sourceCode)
{
    if (!IsLambdaExpression(lambdaNode))
    {
        return std::nullopt;
    }

    TSNode parent = parser::GetParent(lambdaNode);
    if (ts_node_is_null(parent))
    {
        return std::nullopt;
    }

    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol parentSym = ts_node_symbol(parent);
    if (parentSym == gs.symVariableDeclarator)
    {
        return FindDeclaratorFuncdefTarget(parent, table, sourceCode);
    }
    if (parentSym == gs.symAssignmentExpression)
    {
        return FindAssignmentFuncdefTarget(parent, table, sourceCode);
    }
    if (parentSym != gs.symArgumentList)
    {
        return std::nullopt;
    }

    auto position = FindChildPosition(parent, lambdaNode);
    if (!position)
    {
        return std::nullopt;
    }

    TSNode call = parser::GetParent(parent);
    if (ts_node_is_null(call))
    {
        return std::nullopt;
    }

    TSNode callee = ExtractCallCalleeNode(call);
    if (ts_node_is_null(callee))
    {
        return std::nullopt;
    }

    const std::string calleeName = CleanBaseType(parser::GetNodeText(callee, sourceCode));
    if (*position == 0 && ts_node_named_child_count(parent) == 1)
    {
        if (auto asConversion = FindFuncdefSymbol(calleeName, table))
        {
            return asConversion;
        }
    }

    return FindCallArgumentFuncdef(table, calleeName, *position);
}

static uint32_t FindLambdaParamIndex(TSNode paramsNode, std::string_view paramName, std::string_view sourceCode)
{
    auto astParams = ExtractLambdaParameters(paramsNode, sourceCode);
    for (const auto& p : astParams)
    {
        if (p.name == paramName)
        {
            return p.index;
        }
    }
    return UINT32_MAX;
}

std::string InferLambdaParamType(TSNode nodeInLambda, std::string_view paramName, const SymbolTable& symbolTable,
                                 std::string_view sourceCode)
{
    if (ts_node_is_null(nodeInLambda) || paramName.empty())
    {
        return "";
    }

    TSNode lambdaNode = parser::FindAncestorWithSymbol(nodeInLambda, parser::GetGrammarSymbols().symLambdaExpression);
    if (ts_node_is_null(lambdaNode))
    {
        return "";
    }

    TSNode paramsNode = parser::GetChildByField(lambdaNode, parser::GetGrammarSymbols().fieldParameters);
    if (ts_node_is_null(paramsNode))
    {
        return "";
    }

    uint32_t paramIdx = FindLambdaParamIndex(paramsNode, paramName, sourceCode);
    if (paramIdx == UINT32_MAX)
    {
        return "";
    }

    auto targetFuncdef = FuncdefTargetOfLambda(lambdaNode, symbolTable, sourceCode);
    if (!targetFuncdef)
    {
        return "";
    }

    if (std::holds_alternative<FuncdefSignature>(targetFuncdef->signature))
    {
        const auto& fd = targetFuncdef->GetFuncdef();
        if (paramIdx < fd.parameters.size())
        {
            return MemberOwnerType(fd.parameters[paramIdx].typeName, symbolTable.GetDefaultArrayType());
        }
    }
    else if (std::holds_alternative<FunctionSignature>(targetFuncdef->signature))
    {
        const auto& fn = targetFuncdef->GetFunction();
        if (paramIdx < fn.parameters.size())
        {
            return MemberOwnerType(fn.parameters[paramIdx].typeName, symbolTable.GetDefaultArrayType());
        }
    }
    return "";
}

Symbol FuncdefToFunctionSymbol(const Symbol& funcdefSym, std::string_view callName)
{
    Symbol sym;
    sym.type = SymbolType::Function;
    sym.name = std::string(callName);
    sym.containerName = funcdefSym.containerName;
    sym.containerKind = funcdefSym.containerKind;
    sym.qualifiedName = funcdefSym.qualifiedName;
    sym.fileUri = funcdefSym.fileUri;
    sym.startLine = funcdefSym.startLine;
    sym.startCharacter = funcdefSym.startCharacter;
    sym.endLine = funcdefSym.endLine;
    sym.endCharacter = funcdefSym.endCharacter;

    if (std::holds_alternative<FuncdefSignature>(funcdefSym.signature))
    {
        const auto& fd = funcdefSym.GetFuncdef();
        FunctionSignature fnSig;
        fnSig.returnType = fd.returnType;
        fnSig.returnBaseTypeName = fd.returnBaseTypeName;
        fnSig.returnTemplateName = fd.returnTemplateName;
        fnSig.returnTypeKind = fd.returnTypeKind;
        fnSig.returnIsArray = fd.returnIsArray;
        fnSig.returnIsConst = fd.returnIsConst;
        fnSig.returnHasPrimitiveHandle = fd.returnHasPrimitiveHandle;
        fnSig.returnArrayDepth = fd.returnArrayDepth;
        fnSig.modifiers = fd.modifiers;
        fnSig.parameters = fd.parameters;
        sym.signature = std::move(fnSig);
    }
    return sym;
}

std::optional<Symbol> TryResolveCallableFuncdef(const std::string& name, const Scope* scope, const SymbolTable& table,
                                                uint32_t refByte)
{
    if (!scope || name.empty())
    {
        return std::nullopt;
    }
    const LocalDefinition* def = ResolveInScopeAt(scope, name, refByte);
    if (!def || (def->kind != LocalDefinitionKind::Variable && def->kind != LocalDefinitionKind::Parameter))
    {
        return std::nullopt;
    }
    const std::string clean = CleanBaseType(def->typeName);
    if (clean.empty())
    {
        return std::nullopt;
    }
    auto funcdefSym = FindFuncdefSymbol(clean, table);
    if (!funcdefSym)
    {
        return std::nullopt;
    }
    return FuncdefToFunctionSymbol(*funcdefSym, name);
}

static bool HasListConstructorParam(const ParameterInformation& p)
{
    const std::string norm = StripTypeDecorations(p.baseTypeName.empty() ? p.typeName : p.baseTypeName);
    const bool isInt = (norm == "int" || norm == "uint" || norm == "int64" || norm == "uint64");
    return isInt && (p.isReference || p.modifier == ParameterModifier::In || p.modifier == ParameterModifier::InOut ||
                     p.isInOutRef || IsReferenceTypeView(p.typeName));
}

bool IsListConstructorSignature(const FunctionSignature& fn)
{
    if (fn.parameters.size() == 1)
    {
        return HasListConstructorParam(fn.parameters[0]);
    }
    if (fn.parameters.size() == 2)
    {
        return HasListConstructorParam(fn.parameters[0]) && HasListConstructorParam(fn.parameters[1]);
    }
    return false;
}

bool IsListConstructor(const Symbol& sym)
{
    if (sym.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(sym.signature))
    {
        return false;
    }
    return IsListConstructorSignature(sym.GetFunction());
}

static bool IsMemberMethodSymbol(TSNode node, const Scope* scope, const SymbolTable& symbolTable,
                                 std::string_view sourceCode)
{
    TSNode objNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldObject);
    TSNode memNode = parser::GetChildByField(node, parser::GetGrammarSymbols().fieldMember);
    if (ts_node_is_null(objNode) || ts_node_is_null(memNode))
    {
        return false;
    }
    const std::string memName = parser::NodeText(memNode, sourceCode);
    const std::string objType = ResolveExpressionType(objNode, ExpressionTypeContext(scope, symbolTable, sourceCode));
    const std::string cleanObj = CleanBaseType(objType);
    if (cleanObj.empty() || memName.empty())
    {
        return false;
    }
    for (const auto& cls : GetInheritedTypeHierarchy(cleanObj, symbolTable))
    {
        if (const auto syms = symbolTable.FindMemberSymbolPtr(cls, memName))
        {
            bool hasFunc = false;
            bool hasVarOrProp = false;
            for (const auto& s : *syms)
            {
                if (s.type == SymbolType::Function)
                {
                    hasFunc = true;
                }
                else if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
                {
                    hasVarOrProp = true;
                }
            }
            if (hasFunc && !hasVarOrProp)
            {
                return true;
            }
        }
    }
    return false;
}

static bool HasFunctionSymbolOnly(const std::vector<Symbol>& syms)
{
    bool hasFunc = false;
    bool hasVarOrProp = false;
    for (const auto& s : syms)
    {
        if (s.type == SymbolType::Function)
        {
            hasFunc = true;
        }
        else if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
        {
            hasVarOrProp = true;
        }
    }
    return hasFunc && !hasVarOrProp;
}

static bool IsLocalVariableOrFieldInScope(const Scope* scope, TSSymbol sym, const std::string& name,
                                          uint32_t refByte = UINT32_MAX)
{
    const auto& gs = parser::GetGrammarSymbols();
    const bool isUnqualified =
        (sym == gs.symIdentifier || (sym == gs.symScopedIdentifier && !utils::ContainsScopeSeparator(name)));
    if (isUnqualified && scope)
    {
        if (const auto def = ResolveInScopeAt(scope, name, refByte))
        {
            return def->kind == LocalDefinitionKind::Variable || def->kind == LocalDefinitionKind::Parameter ||
                   def->kind == LocalDefinitionKind::Field;
        }
    }
    return false;
}

static bool HasGlobalFunctionSymbol(std::string_view name, const SymbolTable& symbolTable)
{
    if (const auto found = symbolTable.FindSymbolsPtr(name))
    {
        return HasFunctionSymbolOnly(*found);
    }
    const std::string_view lastSeg = LastScopeSegment(name);
    if (lastSeg != name)
    {
        if (const auto found = symbolTable.FindSymbolsPtr(lastSeg))
        {
            return HasFunctionSymbolOnly(*found);
        }
    }
    return false;
}

static bool IsIdentifierFunctionSymbol(TSNode node, const Scope* scope, const SymbolTable& symbolTable,
                                       std::string_view sourceCode)
{
    std::string name = parser::GetNodeText(node, sourceCode);
    if (name.empty())
    {
        return false;
    }
    if (IsLocalVariableOrFieldInScope(scope, ts_node_symbol(node), name, ts_node_start_byte(node)))
    {
        return false;
    }
    return HasGlobalFunctionSymbol(name, symbolTable);
}

bool IsFunctionReference(TSNode node, const Scope* scope, const SymbolTable& symbolTable, std::string_view sourceCode)
{
    const auto& gs = parser::GetGrammarSymbols();
    while (!ts_node_is_null(node) && ts_node_symbol(node) == gs.symParenthesizedExpression &&
           ts_node_named_child_count(node) > 0)
    {
        node = ts_node_named_child(node, 0);
    }
    if (ts_node_is_null(node))
    {
        return false;
    }
    const TSSymbol sym = ts_node_symbol(node);
    if (sym == gs.symCallExpression || sym == gs.symConstructCallExpression)
    {
        return false;
    }
    if (sym == gs.symMemberExpression)
    {
        return IsMemberMethodSymbol(node, scope, symbolTable, sourceCode);
    }
    if (sym == gs.symIdentifier || sym == gs.symScopedIdentifier)
    {
        return IsIdentifierFunctionSymbol(node, scope, symbolTable, sourceCode);
    }
    return false;
}

} // namespace angel_lsp::analysis
