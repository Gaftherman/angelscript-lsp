#include "analysis/InitializerListChecker.h"
#include "analysis/ASTUtils.h"
#include "analysis/NodeIndex.h"
#include "analysis/SemanticHelpers.h"
#include "analysis/TypeConversionChecker.h"
#include "analysis/rules/RuleIndex.h"

#include "parser/GrammarNames.h"
#include <algorithm>
#include <ankerl/unordered_dense.h>
#include <cctype>
#include <string>
#include <unordered_set>
#include <vector>

namespace angel_lsp::analysis
{
namespace
{
std::string Trimmed(std::string text)
{
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    return text;
}

/**
 * @brief Strips the decorations that do not change which values a type accepts.
 *
 * `const`, `&` and `@` all leave the underlying type - and therefore its list pattern -
 * exactly as it was. The brackets and angle brackets are deliberately kept: those *are* the
 * shape this rule reads. That is also why `SemanticHelpers::CleanBaseType` cannot be used
 * here despite doing most of the same work - it strips `[]` and unwraps `array<...>` down
 * to the element type, which is precisely the information that has to survive.
 *
 * These three are language syntax rather than configuration: AngelScript spells them this
 * way and no host can change it.
 */
std::string StripDecorations(std::string_view raw)
{
    std::string text = Trimmed(std::string(raw));

    for (;;)
    {
        if (text.starts_with("const") && text.size() > 5 && std::isspace(static_cast<unsigned char>(text[5])))
        {
            text = Trimmed(text.substr(5));
            continue;
        }
        if (!text.empty() && (text.back() == '@' || text.back() == '&'))
        {
            text.pop_back();
            text = Trimmed(text);
            continue;
        }
        // A trailing `const` is only a qualifier when something separates it from the type
        // name; without the boundary check a class called `Wconst` would be truncated to
        // `W` and then match nothing.
        if (text.size() > 5 && text.ends_with("const"))
        {
            const char before = text[text.size() - 6];
            if (std::isspace(static_cast<unsigned char>(before)) || before == '&' || before == '@')
            {
                text = Trimmed(text.substr(0, text.size() - 5));
                continue;
            }
        }
        break;
    }

    // Whitespace inside the shape - `array < int >` - would defeat the tests below, and
    // none of it is meaningful.
    std::string compact;
    compact.reserve(text.size());
    for (char c : text)
    {
        if (!std::isspace(static_cast<unsigned char>(c)))
            compact += c;
    }
    return compact;
}

/** @brief `Name<A, B>` split into its name and its arguments; arguments empty if none. */
struct TemplateSpelling
{
    std::string name;
    std::vector<std::string> arguments;
};

TemplateSpelling ReadTemplateSpelling(const std::string& type)
{
    TemplateSpelling spelling;

    const size_t open = type.find('<');
    if (open == std::string::npos || !type.ends_with('>'))
    {
        spelling.name = type;
        return spelling;
    }

    spelling.name = type.substr(0, open);

    const std::string inner = type.substr(open + 1, type.size() - open - 2);
    int depth = 0;
    std::string current;
    for (char c : inner)
    {
        if (c == '<')
        {
            ++depth;
        }
        else if (c == '>')
        {
            --depth;
        }
        else if (c == ',' && depth == 0)
        {
            spelling.arguments.push_back(Trimmed(current));
            current.clear();
            continue;
        }
        current += c;
    }
    if (!current.empty())
    {
        spelling.arguments.push_back(Trimmed(current));
    }
    return spelling;
}

void EmitAtNode(TSNode node, DiagnosticContext& ctx, std::string_view code, std::string_view arg)
{
    const TSPoint start = ts_node_start_point(node);
    const TSPoint end = ts_node_end_point(node);
    ctx.EmitAtRange({start.row, start.column, end.row, end.column}, code, arg);
}

/**
 * @brief The declared return type of the function a `return` sits in, or "" when unknown.
 */
std::string EnclosingReturnType(TSNode returnNode, std::string_view sourceCode)
{
    for (TSNode parent = ts_node_parent(returnNode); !ts_node_is_null(parent); parent = ts_node_parent(parent))
    {
        const std::string_view type = NodeType(parent);
        if (type == "lambda_expression")
        {
            return "";
        }
        if (type == "func_declaration")
        {
            TSNode returnType = parser::GetChildByField(parent, parser::fields::ReturnType);
            if (ts_node_is_null(returnType))
            {
                returnType = parser::GetChildByField(parent, parser::fields::Type);
            }
            return ts_node_is_null(returnType) ? std::string() : GetNodeText(returnType, sourceCode);
        }
    }
    return "";
}

/**
 * @brief How many values a list writes, counting the ones that were left out.
 */
uint32_t ListValueCount(TSNode listNode)
{
    uint32_t separators = 0;
    uint32_t written = 0;
    const uint32_t count = ts_node_child_count(listNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_child(listNode, i);
        const std::string_view type = NodeType(child);
        if (type == ",")
        {
            ++separators;
        }
        else if (ts_node_is_named(child) && type != "comment")
        {
            ++written;
        }
    }

    if (separators > 0)
    {
        return separators + 1;
    }
    return written > 0 ? 1 : 0;
}

void CheckElementValue(TSNode element, const std::string& wanted, DiagnosticContext& ctx,
                       const ElementContext& elements)
{
    if (wanted.empty() || wanted == "?")
    {
        return;
    }

    const std::string actual = ResolveExpressionType(
        element, {elements.scopeRoot, ctx.request.symbolTable, elements.sourceCode, ctx.request.fileUri});
    if (actual.empty())
    {
        return;
    }

    if (!CanConvertImplicitly(actual, wanted, ctx))
    {
        const TSPoint start = ts_node_start_point(element);
        const TSPoint end = ts_node_end_point(element);
        ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-err-no-implicit-conversion",
                        StripDecorations(actual), StripDecorations(wanted));
    }
}

/**
 * @brief Container structure and dimensionality information.
 */
struct ContainerInfo
{
    bool isContainer = false;
    uint32_t dimensions = 1;
    std::string elementType;
};

/**
 * @brief Checks whether a constructor function matches AngelScript's list behavior signature.
 *
 * Conforming to `asBEHAVE_LIST_CONSTRUCT` / `asBEHAVE_LIST_FACTORY`:
 * - Single parameter: integer reference with in modifier (e.g. `const int &in` or `int &in`).
 * - Two parameters: both integer references with in modifier (e.g. `int &in, int &in`).
 */
bool IsListConstructorSignature(const FunctionSignature& fn)
{
    if (fn.parameters.size() == 1)
    {
        return fn.parameters[0].modifier == ParameterModifier::In &&
               (fn.parameters[0].typeName.find("int") != std::string::npos || fn.parameters[0].baseTypeName == "int" ||
                fn.parameters[0].baseTypeName == "uint");
    }
    if (fn.parameters.size() == 2)
    {
        return fn.parameters[0].modifier == ParameterModifier::In &&
               fn.parameters[1].modifier == ParameterModifier::In &&
               (fn.parameters[0].typeName.find("int") != std::string::npos || fn.parameters[0].baseTypeName == "int" ||
                fn.parameters[0].baseTypeName == "uint") &&
               (fn.parameters[1].typeName.find("int") != std::string::npos || fn.parameters[1].baseTypeName == "int" ||
                fn.parameters[1].baseTypeName == "uint");
    }
    return false;
}

/**
 * @brief Intermediate container member inspection state.
 */
struct ContainerMemberInspection
{
    uint32_t maxIndexParams = 0;
    std::string indexReturnType;
    bool hasListConstructor = false;
};

/**
 * @brief Inspects container member functions for multi-index operators and list constructors.
 *
 * @param[in]  containerKey Class key in rule index.
 * @param[in]  className    Short name of the container class.
 * @param[in]  ctx          Diagnostic context.
 * @param[out] out          Inspection state sink.
 */
void InspectContainerMembers(const std::string& containerKey, const std::string& className,
                             const DiagnosticContext& ctx, ContainerMemberInspection& out)
{
    const auto& members = ctx.request.GetRuleIndex().Members(containerKey);
    for (const auto& key : members.memberKeys)
    {
        const auto mSyms = ctx.request.symbolTable.FindSymbolsPtr(key);
        if (!mSyms)
        {
            continue;
        }
        for (const auto& m : *mSyms)
        {
            if (m.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(m.signature))
            {
                continue;
            }
            const auto& fn = m.GetFunction();
            if (m.name == "opIndex")
            {
                if (fn.parameters.size() > out.maxIndexParams)
                {
                    out.maxIndexParams = static_cast<uint32_t>(fn.parameters.size());
                    out.indexReturnType = fn.returnBaseTypeName.empty() ? fn.returnType : fn.returnBaseTypeName;
                }
            }
            else if (m.name == className && IsListConstructorSignature(fn))
            {
                out.hasListConstructor = true;
            }
        }
    }
}

/**
 * @brief Builds a list of candidate container keys for a symbol name lookup.
 *
 * @param[in] sym          Target symbol.
 * @param[in] fallbackName Fallback type name.
 * @return Ordered list of unique candidate container keys.
 */
std::vector<std::string> CandidateContainerKeys(const Symbol& sym, std::string_view fallbackName)
{
    std::vector<std::string> containers;
    if (!sym.qualifiedName.empty())
    {
        containers.push_back(sym.qualifiedName);
    }
    if (!sym.name.empty() && sym.name != sym.qualifiedName)
    {
        containers.push_back(sym.name);
    }
    if (!fallbackName.empty() && fallbackName != sym.name && fallbackName != sym.qualifiedName)
    {
        containers.emplace_back(fallbackName);
    }
    return containers;
}

/**
 * @brief Inspects a class symbol to determine whether it behaves as a container.
 *
 * @param[in] sym      Class symbol to inspect.
 * @param[in] spelling Template spelling of the container type.
 * @param[in] ctx      Diagnostic context.
 * @return Container info if class behaves as a container, std::nullopt otherwise.
 */
std::optional<ContainerInfo> InspectClassContainer(const Symbol& sym, const TemplateSpelling& spelling,
                                                   const DiagnosticContext& ctx)
{
    ContainerMemberInspection inspection;
    const auto containers = CandidateContainerKeys(sym, spelling.name);
    for (const auto& containerKey : containers)
    {
        InspectContainerMembers(containerKey, sym.name, ctx, inspection);
    }

    auto resolveElem = [&](const std::string& fallback)
    {
        std::string elem = !spelling.arguments.empty() ? spelling.arguments[0] : StripDecorations(fallback);
        return elem.empty() ? "auto" : elem;
    };

    if (inspection.maxIndexParams >= 2)
    {
        return ContainerInfo{true, inspection.maxIndexParams, resolveElem(inspection.indexReturnType)};
    }
    if (inspection.hasListConstructor && (!spelling.arguments.empty() || inspection.maxIndexParams == 1))
    {
        return ContainerInfo{true, 1, resolveElem(inspection.indexReturnType)};
    }
    return std::nullopt;
}

/**
 * @brief Inspects whether a type represents a sequence or multi-dimensional container.
 *
 * Identifies containers via:
 * 1. Syntax arrays: `T[]`.
 * 2. Engine-configured array type or `arrayLikeTemplates`.
 * 3. SymbolTable registration with multi-index `opIndex` (e.g. 2D grid/matrix) or list constructors.
 *
 * @param[in] type               Cleaned type string.
 * @param[in] spelling           Parsed template spelling.
 * @param[in] ctx                Diagnostic context.
 * @param[in] arrayLikeTemplates Custom array-like template names.
 * @return Resolved container dimensionality and element type.
 */
ContainerInfo InspectContainer(const std::string& type, const TemplateSpelling& spelling, const DiagnosticContext& ctx,
                               const std::unordered_set<std::string>& arrayLikeTemplates)
{
    if (type.ends_with("[]"))
    {
        return {true, 1, type.substr(0, type.size() - 2)};
    }

    const std::string_view configuredArray = ctx.request.GetArrayTypeName();
    if (spelling.name == "array" || (!configuredArray.empty() && spelling.name == configuredArray) ||
        arrayLikeTemplates.contains(spelling.name))
    {
        std::string elem = spelling.arguments.empty() ? "auto" : spelling.arguments[0];
        return {true, 1, std::move(elem)};
    }

    if (!spelling.name.empty())
    {
        const auto syms = ctx.request.symbolTable.FindSymbolsPtr(spelling.name);
        const auto typeSymbols = syms ? *syms : ctx.request.symbolTable.FindTypeSymbolsByShortName(spelling.name);
        for (const auto& sym : typeSymbols)
        {
            if (sym.type == SymbolType::Class)
            {
                if (auto info = InspectClassContainer(sym, spelling, ctx))
                {
                    return *info;
                }
            }
        }
    }

    return {false, 1, ""};
}

/**
 * @brief Dictionary container key and value type information.
 */
struct DictInfo
{
    bool isDict = false;
    std::string keyType;
    std::string valType;
};

/**
 * @brief State tracking for dictionary container inspection.
 */
struct DictTypeState
{
    std::string keyType;
    std::string valType;
    bool isAssociative = false;
};

/**
 * @brief Inspects a function symbol for associative dictionary signatures (`set`, `get`, `exists`).
 *
 * @param[in]     m     Candidate function symbol.
 * @param[in,out] state Dictionary type state sink.
 */
void InspectDictFunctionSignature(const Symbol& m, DictTypeState& state)
{
    if (m.type != SymbolType::Function || !std::holds_alternative<FunctionSignature>(m.signature))
    {
        return;
    }
    const auto& fn = m.GetFunction();
    const bool isSetOrGet = (m.name == "set" || m.name == "get") && fn.parameters.size() >= 2;
    const bool isExists = (m.name == "exists") && !fn.parameters.empty();
    if (!isSetOrGet && !isExists)
    {
        return;
    }
    state.isAssociative = true;
    if (state.keyType.empty())
    {
        state.keyType =
            fn.parameters[0].baseTypeName.empty() ? fn.parameters[0].typeName : fn.parameters[0].baseTypeName;
    }
    if (isSetOrGet && state.valType.empty())
    {
        state.valType =
            fn.parameters[1].baseTypeName.empty() ? fn.parameters[1].typeName : fn.parameters[1].baseTypeName;
    }
}

/**
 * @brief Inspects member methods and keys for associative container semantics.
 *
 * @param[in]     containerKey Container key in rule index.
 * @param[in]     argCount     Template argument count.
 * @param[in]     ctx          Diagnostic context.
 * @param[in,out] state        Dictionary type state sink.
 */
void InspectClassDictMembers(const std::string& containerKey, size_t argCount, const DiagnosticContext& ctx,
                             DictTypeState& state)
{
    const auto& members = ctx.request.GetRuleIndex().Members(containerKey);
    if (members.methodNames.contains("exists"))
    {
        state.isAssociative = true;
    }
    if (argCount >= 2 && (members.methodNames.contains("insert") || members.methodNames.contains("opIndex") ||
                          members.methodNames.contains("find") || members.methodNames.contains("set")))
    {
        state.isAssociative = true;
    }

    for (const auto& key : members.memberKeys)
    {
        if (const auto mSyms = ctx.request.symbolTable.FindSymbolsPtr(key))
        {
            for (const auto& m : *mSyms)
            {
                InspectDictFunctionSignature(m, state);
            }
        }
    }
}

/**
 * @brief Inspects a single class symbol for dictionary semantics.
 *
 * @param[in]     sym      Candidate class symbol.
 * @param[in]     spelling Parsed template spelling.
 * @param[in]     ctx      Diagnostic context.
 * @param[in,out] state    Dictionary type state sink.
 */
void InspectClassDict(const Symbol& sym, const TemplateSpelling& spelling, const DiagnosticContext& ctx,
                      DictTypeState& state)
{
    if (sym.type != SymbolType::Class)
    {
        return;
    }
    const auto containers = CandidateContainerKeys(sym, spelling.name);
    for (const auto& containerKey : containers)
    {
        InspectClassDictMembers(containerKey, spelling.arguments.size(), ctx, state);
    }
}

/**
 * @brief Inspects whether a type represents an associative container and resolves its key and value types.
 *
 * @param[in] spelling Parsed template spelling of the container.
 * @param[in] ctx      Diagnostic context.
 * @return Resolved dictionary info.
 */
DictInfo InspectDictionary(const TemplateSpelling& spelling, const DiagnosticContext& ctx)
{
    if (spelling.name.empty())
    {
        return {false, "", ""};
    }

    DictTypeState state;
    if (spelling.arguments.size() >= 2)
    {
        state.keyType = spelling.arguments[0];
        state.valType = spelling.arguments[1];
    }
    else if (spelling.arguments.size() == 1)
    {
        state.valType = spelling.arguments[0];
    }

    const auto syms = ctx.request.symbolTable.FindSymbolsPtr(spelling.name);
    const auto typeSymbols = syms ? *syms : ctx.request.symbolTable.FindTypeSymbolsByShortName(spelling.name);

    for (const auto& sym : typeSymbols)
    {
        InspectClassDict(sym, spelling, ctx, state);
    }

    if (!state.isAssociative)
    {
        return {false, "", ""};
    }

    if (state.keyType.empty())
    {
        state.keyType = "string";
    }
    if (state.valType.empty())
    {
        state.valType = "?";
    }

    return {true, StripDecorations(state.keyType), StripDecorations(state.valType)};
}

/**
 * @brief Field information for aggregate struct member properties.
 */
struct StructFieldInfo
{
    std::string name;
    std::string type;
    uint32_t line = 0;
    uint32_t col = 0;
};

/**
 * @brief Aggregate struct fields and list initialization capability.
 */
struct AggregateStructInfo
{
    bool hasListSupport = false;
    std::vector<StructFieldInfo> fields;
};

using StructLayoutCache = ankerl::unordered_dense::map<std::string, AggregateStructInfo>;

/**
 * @brief Inspects a member symbol and records it if it represents an aggregate field or list constructor.
 *
 * @param[in]     s        Member symbol.
 * @param[in]     classSym Containing class symbol.
 * @param[in,out] info     Aggregate struct info sink.
 */
void InspectStructMemberSymbol(const Symbol& s, const Symbol& classSym, AggregateStructInfo& info)
{
    if (s.type == SymbolType::Variable || s.type == SymbolType::Property)
    {
        if (std::holds_alternative<VariableSignature>(s.signature))
        {
            const auto& varSig = s.GetVariable();
            if (!varSig.isVirtualProperty)
            {
                std::string fType = varSig.baseTypeName.empty() ? varSig.typeName : varSig.baseTypeName;
                info.fields.push_back({s.name, StripDecorations(fType), s.startLine, s.startCharacter});
            }
        }
    }
    else if (s.type == SymbolType::Function && std::holds_alternative<FunctionSignature>(s.signature))
    {
        if (s.name == classSym.name && IsListConstructorSignature(s.GetFunction()))
        {
            info.hasListSupport = true;
        }
    }
}

/**
 * @brief Collects aggregate struct fields and list constructors from a container key.
 *
 * @param[in]     container Container name key in rule index.
 * @param[in]     classSym  Containing class symbol.
 * @param[in]     ctx       Diagnostic context.
 * @param[in,out] info      Aggregate struct info sink.
 */
void CollectStructContainerMembers(const std::string& container, const Symbol& classSym, const DiagnosticContext& ctx,
                                   AggregateStructInfo& info)
{
    const auto& members = ctx.request.GetRuleIndex().Members(container);
    for (const auto& key : members.memberKeys)
    {
        if (const auto mSyms = ctx.request.symbolTable.FindSymbolsPtr(key))
        {
            for (const auto& s : *mSyms)
            {
                InspectStructMemberSymbol(s, classSym, info);
            }
        }
    }
}

/**
 * @brief Sorts and deduplicates struct field definitions by declaration order.
 *
 * @param[in,out] fields Vector of struct fields to finalize.
 */
void FinalizeStructFields(std::vector<StructFieldInfo>& fields)
{
    std::sort(fields.begin(), fields.end(),
              [](const StructFieldInfo& a, const StructFieldInfo& b)
              {
                  if (a.line != b.line)
                  {
                      return a.line < b.line;
                  }
                  if (a.col != b.col)
                  {
                      return a.col < b.col;
                  }
                  return a.name < b.name;
              });

    fields.erase(std::unique(fields.begin(), fields.end(), [](const StructFieldInfo& a, const StructFieldInfo& b)
                             { return a.name == b.name && a.line == b.line && a.col == b.col; }),
                 fields.end());
}

/**
 * @brief Inspects a single class symbol for aggregate struct fields and list constructors.
 *
 * @param[in]     sym      Class symbol to inspect.
 * @param[in]     spelling Parsed template spelling.
 * @param[in]     ctx      Diagnostic context.
 * @param[in,out] info     Aggregate struct info sink.
 */
void InspectClassAggregate(const Symbol& sym, const TemplateSpelling& spelling, const DiagnosticContext& ctx,
                           AggregateStructInfo& info)
{
    if (sym.type != SymbolType::Class)
    {
        return;
    }

    if (ctx.request.IsRegisteredSymbol(sym.name) ||
        (!sym.qualifiedName.empty() && ctx.request.IsRegisteredSymbol(sym.qualifiedName)) ||
        IsFromPredefinedStub(sym, ctx))
    {
        info.hasListSupport = true;
    }

    const auto candidateContainers = CandidateContainerKeys(sym, spelling.name);
    for (const auto& container : candidateContainers)
    {
        CollectStructContainerMembers(container, sym, ctx, info);
    }
}

/**
 * @brief Dynamically extracts member properties in declaration order for an aggregate struct/class.
 *
 * @param[in]     spelling Parsed template spelling of the struct.
 * @param[in]     ctx      Diagnostic context.
 * @param[in,out] cache    Layout cache across inspections.
 * @return Aggregate struct field layout and list initialization support.
 */
AggregateStructInfo InspectAggregateStruct(const TemplateSpelling& spelling, const DiagnosticContext& ctx,
                                           StructLayoutCache& cache)
{
    if (spelling.name.empty())
    {
        return {};
    }

    const auto syms = ctx.request.symbolTable.FindSymbolsPtr(spelling.name);
    const auto typeSymbols = syms ? *syms : ctx.request.symbolTable.FindTypeSymbolsByShortName(spelling.name);

    const Symbol* classSym = nullptr;
    for (const auto& sym : typeSymbols)
    {
        if (sym.type == SymbolType::Class)
        {
            classSym = &sym;
            break;
        }
    }

    const std::string cacheKey =
        classSym ? (classSym->qualifiedName.empty() ? classSym->name : classSym->qualifiedName) : spelling.name;

    const auto it = cache.find(cacheKey);
    if (it != cache.end())
    {
        return it->second;
    }

    AggregateStructInfo info;
    if (ctx.request.IsRegisteredSymbol(spelling.name))
    {
        info.hasListSupport = true;
    }

    for (const auto& sym : typeSymbols)
    {
        InspectClassAggregate(sym, spelling, ctx, info);
    }

    FinalizeStructFields(info.fields);
    cache.emplace(cacheKey, info);
    return info;
}

/**
 * @brief Context for recursive initializer list validation.
 */
struct ListValidationContext
{
    DiagnosticContext& ctx;
    const ElementContext& elements;
    const std::unordered_set<std::string>& arrayLikeTemplates;
    StructLayoutCache& cache;
    int depth = 0;
};

void ValidateList(TSNode listNode, const std::string& targetType, const ListValidationContext& valCtx);

/**
 * @brief Validates elements of a sequence or multi-dimensional container recursively.
 *
 * @param[in] node      Initializer list node.
 * @param[in] dimension Remaining container dimensions.
 * @param[in] elemType  Expected element type.
 * @param[in] valCtx    Validation context.
 */
void ValidateMultiDimensionalContainer(TSNode node, uint32_t dimension, const std::string& elemType,
                                       const ListValidationContext& valCtx)
{
    if (valCtx.depth >= k_maxAstDepth || ts_node_is_null(node))
    {
        return;
    }

    const uint32_t count = ts_node_named_child_count(node);
    const ListValidationContext nextCtx{valCtx.ctx, valCtx.elements, valCtx.arrayLikeTemplates, valCtx.cache,
                                        valCtx.depth + 1};
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_named_child(node, i);
        if (dimension > 1)
        {
            if (NodeType(child) != "initializer_list")
            {
                EmitAtNode(child, valCtx.ctx, "as-err-initializer-list-expected", "");
            }
            else
            {
                ValidateMultiDimensionalContainer(child, dimension - 1, elemType, nextCtx);
            }
        }
        else
        {
            if (NodeType(child) == "initializer_list")
            {
                ValidateList(child, elemType, nextCtx);
            }
            else
            {
                CheckElementValue(child, elemType, valCtx.ctx, valCtx.elements);
            }
        }
    }
}

/**
 * @brief Validates a single dictionary key-value pair in an initializer list.
 *
 * @param[in] child    Pair initializer list node.
 * @param[in] dictInfo Dictionary key and value type information.
 * @param[in] valCtx   Validation context.
 */
void ValidateDictionaryPair(TSNode child, const DictInfo& dictInfo, const ListValidationContext& valCtx)
{
    const uint32_t pairCount = ts_node_named_child_count(child);
    if (pairCount > 0)
    {
        TSNode keyNode = ts_node_named_child(child, 0);
        if (NodeType(keyNode) == "initializer_list")
        {
            EmitAtNode(keyNode, valCtx.ctx, "as-err-initializer-list-not-supported", dictInfo.keyType);
        }
        else
        {
            CheckElementValue(keyNode, dictInfo.keyType, valCtx.ctx, valCtx.elements);
        }
    }
    if (pairCount > 1)
    {
        TSNode valNode = ts_node_named_child(child, 1);
        if (NodeType(valNode) == "initializer_list")
        {
            EmitAtNode(valNode, valCtx.ctx, "as-err-initializer-list-not-supported", dictInfo.valType);
        }
        else
        {
            CheckElementValue(valNode, dictInfo.valType, valCtx.ctx, valCtx.elements);
        }
    }
}

/**
 * @brief Validates an initializer list against expected dictionary key-value semantics.
 *
 * @param[in] listNode Initializer list node.
 * @param[in] dictInfo Dictionary key and value type information.
 * @param[in] valCtx   Validation context.
 */
void ValidateDictionaryList(TSNode listNode, const DictInfo& dictInfo, const ListValidationContext& valCtx)
{
    const uint32_t count = ts_node_named_child_count(listNode);
    for (uint32_t i = 0; i < count; ++i)
    {
        TSNode child = ts_node_named_child(listNode, i);
        if (NodeType(child) != "initializer_list")
        {
            EmitAtNode(child, valCtx.ctx, "as-err-initializer-list-expected", "");
            continue;
        }

        const uint32_t valuesCount = ListValueCount(child);
        if (valuesCount < 2)
        {
            EmitAtNode(child, valCtx.ctx, "as-err-initializer-list-too-few", "");
        }
        else if (valuesCount > 2)
        {
            EmitAtNode(child, valCtx.ctx, "as-err-initializer-list-too-many", "");
        }

        ValidateDictionaryPair(child, dictInfo, valCtx);
    }
}

/**
 * @brief Validates an initializer list against an aggregate struct or class layout.
 *
 * @param[in] listNode   Initializer list node.
 * @param[in] type       Decorated or unadorned type name.
 * @param[in] structInfo Struct layout information.
 * @param[in] valCtx     Validation context.
 */
void ValidateStructAggregateList(TSNode listNode, const std::string& type, const AggregateStructInfo& structInfo,
                                 const ListValidationContext& valCtx)
{
    if (!structInfo.hasListSupport)
    {
        EmitAtNode(listNode, valCtx.ctx, "as-err-initializer-list-not-supported", type);
        return;
    }

    const uint32_t written = ListValueCount(listNode);
    const auto expected = static_cast<uint32_t>(structInfo.fields.size());
    if (written < expected)
    {
        EmitAtNode(listNode, valCtx.ctx, "as-err-initializer-list-too-few", "");
        return;
    }
    if (written > expected)
    {
        EmitAtNode(listNode, valCtx.ctx, "as-err-initializer-list-too-many", "");
        return;
    }

    const ListValidationContext nextCtx{valCtx.ctx, valCtx.elements, valCtx.arrayLikeTemplates, valCtx.cache,
                                        valCtx.depth + 1};
    const uint32_t childCount = ts_node_named_child_count(listNode);
    for (uint32_t i = 0; i < childCount && i < structInfo.fields.size(); ++i)
    {
        TSNode elem = ts_node_named_child(listNode, i);
        if (NodeType(elem) == "initializer_list")
        {
            ValidateList(elem, structInfo.fields[i].type, nextCtx);
        }
        else
        {
            CheckElementValue(elem, structInfo.fields[i].type, valCtx.ctx, valCtx.elements);
        }
    }
}

/**
 * @brief Validates an initializer list against an expected target type.
 *
 * @param[in] listNode   The `initializer_list` node.
 * @param[in] targetType Target type being initialized.
 * @param[in] valCtx     Validation context.
 */
void ValidateList(TSNode listNode, const std::string& targetType, const ListValidationContext& valCtx)
{
    if (valCtx.depth >= k_maxAstDepth || ts_node_is_null(listNode))
    {
        return;
    }

    const std::string type = StripDecorations(targetType);
    if (type.empty() || type == "auto" || type == "void")
    {
        return;
    }

    if (IsCorePrimitive(type) || type == "?")
    {
        EmitAtNode(listNode, valCtx.ctx, "as-err-initializer-list-not-supported", type);
        return;
    }

    const TemplateSpelling spelling = ReadTemplateSpelling(type);

    // 1. Dictionary types
    const DictInfo dictInfo = InspectDictionary(spelling, valCtx.ctx);
    if (dictInfo.isDict)
    {
        ValidateDictionaryList(listNode, dictInfo, valCtx);
        return;
    }

    // 2. Sequence & multi-dimensional container types
    const ContainerInfo containerInfo = InspectContainer(type, spelling, valCtx.ctx, valCtx.arrayLikeTemplates);
    if (containerInfo.isContainer)
    {
        ValidateMultiDimensionalContainer(listNode, containerInfo.dimensions, containerInfo.elementType, valCtx);
        return;
    }

    // 3. Class / Struct Aggregates
    const AggregateStructInfo structInfo = InspectAggregateStruct(spelling, valCtx.ctx, valCtx.cache);
    if (!structInfo.fields.empty())
    {
        ValidateStructAggregateList(listNode, type, structInfo, valCtx);
        return;
    }

    // A class with no member fields or list pattern
    if (valCtx.depth == 0 && valCtx.ctx.request.symbolTable.HasSymbolAnywhere(type) &&
        !valCtx.ctx.request.IsRegisteredSymbol(type))
    {
        const TSPoint start = ts_node_start_point(listNode);
        const TSPoint end = ts_node_end_point(listNode);
        valCtx.ctx.EmitAtRange({start.row, start.column, end.row, end.column}, "as-hint-list-pattern-unknown", type,
                               DiagnosticSeverity::Hint);
    }
}

/**
 * @brief Traversal context for walking documents and validating initializer lists.
 */
struct InitializerListContext
{
    const InitializerListCheckRequest& request;
    DiagnosticContext& ctx;
    const std::unordered_set<std::string>& arrayLikeTemplates;
    StructLayoutCache& structCache;
};

/**
 * @brief Computes the element context at the position of an initializer list node.
 *
 * @param[in] node    Target AST node.
 * @param[in] request Analysis request.
 * @return ElementContext with enclosing scope.
 */
ElementContext ElementsAt(TSNode node, const InitializerListCheckRequest& request)
{
    const TSPoint start = ts_node_start_point(node);
    return ElementContext{request.sourceCode,
                          request.scopeRoot ? FindEnclosingScope(request.scopeRoot, start.row, start.column) : nullptr};
}

/**
 * @brief Validates typed initializer lists (`Type = { ... }`).
 *
 * @param[in] node    Typed initializer list AST node.
 * @param[in] initCtx Initializer traversal context.
 */
void ProcessTypedInitializerList(TSNode node, const InitializerListContext& initCtx)
{
    TSNode typeNode = parser::GetChildByField(node, parser::fields::Type);
    TSNode valueNode = parser::GetChildByField(node, parser::fields::Value);
    if (!ts_node_is_null(typeNode) && !ts_node_is_null(valueNode))
    {
        const ElementContext elements = ElementsAt(valueNode, initCtx.request);
        const ListValidationContext valCtx{initCtx.ctx, elements, initCtx.arrayLikeTemplates, initCtx.structCache, 0};
        ValidateList(valueNode, GetNodeText(typeNode, initCtx.request.sourceCode), valCtx);
    }
}

/**
 * @brief Validates assignment expressions with initializer list right-hand sides (`a = { ... }`).
 *
 * @param[in] node    Assignment expression AST node.
 * @param[in] initCtx Initializer traversal context.
 */
void ProcessAssignmentExpression(TSNode node, const InitializerListContext& initCtx)
{
    TSNode value = parser::GetChildByField(node, parser::fields::Right);
    if (ts_node_is_null(value) || NodeType(value) != "initializer_list")
    {
        return;
    }

    TSNode opNode = parser::GetChildByField(node, parser::fields::Operator);
    TSNode target = parser::GetChildByField(node, parser::fields::Left);
    if (!ts_node_is_null(opNode) && !ts_node_is_null(target) && GetNodeText(opNode, initCtx.request.sourceCode) == "=")
    {
        const ElementContext elements = ElementsAt(value, initCtx.request);
        const std::string targetType =
            ResolveExpressionType(target, {elements.scopeRoot, initCtx.ctx.request.symbolTable,
                                           initCtx.request.sourceCode, initCtx.ctx.request.fileUri});
        if (!targetType.empty())
        {
            const ListValidationContext valCtx{initCtx.ctx, elements, initCtx.arrayLikeTemplates, initCtx.structCache,
                                               0};
            ValidateList(value, targetType, valCtx);
        }
    }
}

/**
 * @brief Validates return statements returning initializer lists (`return { ... };`).
 *
 * @param[in] node    Return statement AST node.
 * @param[in] initCtx Initializer traversal context.
 */
void ProcessReturnStatement(TSNode node, const InitializerListContext& initCtx)
{
    if (ts_node_named_child_count(node) == 0)
    {
        return;
    }

    TSNode value = ts_node_named_child(node, 0);
    if (NodeType(value) != "initializer_list")
    {
        return;
    }

    const std::string returnType = EnclosingReturnType(node, initCtx.request.sourceCode);
    if (!returnType.empty())
    {
        const ElementContext elements = ElementsAt(value, initCtx.request);
        const ListValidationContext valCtx{initCtx.ctx, elements, initCtx.arrayLikeTemplates, initCtx.structCache, 0};
        ValidateList(value, returnType, valCtx);
    }
}

/**
 * @brief Validates variable declarations with initializer list initializers (`Type a = { ... };`).
 *
 * @param[in] node    Variable declaration AST node.
 * @param[in] initCtx Initializer traversal context.
 */
void ProcessVariableDeclaration(TSNode node, const InitializerListContext& initCtx)
{
    TSNode typeNode = parser::GetChildByField(node, parser::fields::VarType);
    if (ts_node_is_null(typeNode))
    {
        typeNode = parser::GetChildByField(node, parser::fields::Type);
    }
    if (ts_node_is_null(typeNode))
    {
        return;
    }

    const std::string declaredType = GetNodeText(typeNode, initCtx.request.sourceCode);
    const uint32_t childCount = ts_node_named_child_count(node);
    for (uint32_t i = 0; i < childCount; ++i)
    {
        TSNode declarator = ts_node_named_child(node, i);
        if (NodeType(declarator) != "variable_declarator")
        {
            continue;
        }

        TSNode valueNode = parser::GetChildByField(declarator, parser::fields::Value);
        if (!ts_node_is_null(valueNode) && NodeType(valueNode) == "initializer_list")
        {
            const ElementContext elements = ElementsAt(valueNode, initCtx.request);
            const ListValidationContext valCtx{initCtx.ctx, elements, initCtx.arrayLikeTemplates, initCtx.structCache,
                                               0};
            ValidateList(valueNode, declaredType, valCtx);
        }
    }
}

/**
 * @brief Dispatches an AST node to the corresponding initializer list processor.
 *
 * @param[in] node    AST node to process.
 * @param[in] initCtx Initializer traversal context.
 */
void DispatchInitListNode(TSNode node, const InitializerListContext& initCtx)
{
    const std::string_view nodeType = NodeType(node);
    if (nodeType == "typed_initializer_list")
    {
        ProcessTypedInitializerList(node, initCtx);
    }
    else if (nodeType == "assignment_expression")
    {
        ProcessAssignmentExpression(node, initCtx);
    }
    else if (nodeType == "return_statement")
    {
        ProcessReturnStatement(node, initCtx);
    }
    else if (nodeType == "variable_declaration")
    {
        ProcessVariableDeclaration(node, initCtx);
    }
}

/**
 * @brief Walks initializer list nodes using the fast NodeIndex cursor merge.
 *
 * @param[in] initCtx Initializer traversal context.
 */
void DispatchIndexedInitializerLists(const InitializerListContext& initCtx)
{
    struct Cursor
    {
        std::span<const TSNode> nodes;
        size_t index = 0;
        uint32_t currentByte() const
        {
            return (index < nodes.size()) ? ts_node_start_byte(nodes[index]) : UINT32_MAX;
        }
    };

    std::array<Cursor, 4> cursors = {{{initCtx.request.nodeIndex->Nodes(parser::nodes::TypedInitializerList), 0},
                                      {initCtx.request.nodeIndex->Nodes(parser::nodes::AssignmentExpression), 0},
                                      {initCtx.request.nodeIndex->Nodes(parser::nodes::ReturnStatement), 0},
                                      {initCtx.request.nodeIndex->Nodes(parser::nodes::VariableDeclaration), 0}}};

    while (true)
    {
        size_t best = 0;
        uint32_t minByte = cursors[0].currentByte();
        for (size_t c = 1; c < cursors.size(); ++c)
        {
            uint32_t b = cursors[c].currentByte();
            if (b < minByte)
            {
                minByte = b;
                best = c;
            }
        }
        if (minByte == UINT32_MAX)
        {
            break;
        }

        TSNode node = cursors[best].nodes[cursors[best].index++];
        DispatchInitListNode(node, initCtx);
    }
}

/**
 * @brief Falls back to flat worklist traversal when NodeIndex is unavailable.
 *
 * @param[in] initCtx Initializer traversal context.
 */
void DispatchTreeWalkInitializerLists(const InitializerListContext& initCtx)
{
    std::vector<TSNode> stack = {initCtx.request.root};
    while (!stack.empty())
    {
        TSNode node = stack.back();
        stack.pop_back();

        DispatchInitListNode(node, initCtx);

        const uint32_t count = ts_node_child_count(node);
        for (uint32_t i = 0; i < count; ++i)
        {
            stack.push_back(ts_node_child(node, i));
        }
    }
}
} // namespace

void CheckInitializerListAgainstType(TSNode listNode, const std::string& targetType, const ElementContext& elements,
                                     DiagnosticContext& ctx)
{
    StructLayoutCache cache;
    const std::unordered_set<std::string> arrayTemplates = ctx.request.GetArrayLikeTemplateNames();
    const ListValidationContext valCtx{ctx, elements, arrayTemplates, cache, 0};
    ValidateList(listNode, targetType, valCtx);
}

void ValidateInitializerList(TSNode listNode, const std::string& expectedType, DiagnosticContext& ctx)
{
    ElementContext elements{ctx.request.sourceCode, ctx.request.scopeRoot.get()};
    StructLayoutCache cache;
    const std::unordered_set<std::string> arrayTemplates = ctx.request.GetArrayLikeTemplateNames();
    const ListValidationContext valCtx{ctx, elements, arrayTemplates, cache, 0};
    ValidateList(listNode, expectedType, valCtx);
}

void CheckInitializerLists(const InitializerListCheckRequest& request, DiagnosticContext& ctx)
{
    const std::unordered_set<std::string> arrayLikeTemplates = ctx.request.GetArrayLikeTemplateNames();
    StructLayoutCache structCache;
    const InitializerListContext initCtx{request, ctx, arrayLikeTemplates, structCache};

    if (request.nodeIndex)
    {
        DispatchIndexedInitializerLists(initCtx);
    }
    else
    {
        DispatchTreeWalkInitializerLists(initCtx);
    }
}
} // namespace angel_lsp::analysis
