#include "analysis/index/TypeExtraction.h"

#include "analysis/index/SyntaxOperands.h"
#include "parser/ASTUtils.h"

#include <algorithm>
#include <span>
#include <string_view>
#include <tree_sitter/api.h>
#include <utility>

extern "C" const TSLanguage* tree_sitter_angelscript();

namespace angel_lsp::analysis
{
TypeExtractionSymbols::TypeExtractionSymbols()
{
    const TSLanguage* lang = tree_sitter_angelscript();

    auto addPrimitive = [&](const char* name, uint32_t len, TypeKind kind)
    {
        TSSymbol sym = ts_language_symbol_for_name(lang, name, len, false);
        if (sym != 0)
        {
            primitiveKindMap.emplace(sym, kind);
        }
    };

    addPrimitive("void", 4, TypeKind::Void);
    addPrimitive("int", 3, TypeKind::Int32);
    addPrimitive("int32", 5, TypeKind::Int32);
    addPrimitive("int8", 4, TypeKind::Int8);
    addPrimitive("int16", 5, TypeKind::Int16);
    addPrimitive("int64", 5, TypeKind::Int64);
    addPrimitive("uint", 4, TypeKind::UInt32);
    addPrimitive("uint32", 6, TypeKind::UInt32);
    addPrimitive("uint8", 5, TypeKind::UInt8);
    addPrimitive("uint16", 6, TypeKind::UInt16);
    addPrimitive("uint64", 6, TypeKind::UInt64);
    addPrimitive("float", 5, TypeKind::Float);
    addPrimitive("double", 6, TypeKind::Double);
    addPrimitive("bool", 4, TypeKind::Bool);
    addPrimitive("string", 6, TypeKind::String);
    addPrimitive("auto", 4, TypeKind::Auto);
}

static const TypeExtractionSymbols& GetTypeExtractionSymbols()
{
    static const TypeExtractionSymbols symbols;
    return symbols;
}

static bool ReadTypeLeaf(TSNode node, std::string_view sourceCode, const NodeIndex* index, TypeExtractionResult& result)
{
    const auto& gs = parser::GetGrammarSymbols();
    const TSSymbol symbol = ts_node_symbol(node);
    if (symbol == gs.symPrimitiveType)
    {
        result.baseTypeName = parser::GetNodeText(node, sourceCode);
        SyntaxOperands(node, index)
            .ForEachChild(
                [&](TSNode token)
                {
                    result.kind = GetTypeExtractionSymbols().LookupPrimitiveKind(ts_node_symbol(token));
                    return false;
                });
        return true;
    }
    if (symbol == gs.symIdentifier)
    {
        result.baseTypeName = parser::GetNodeText(node, sourceCode);
        return true;
    }
    return false;
}

static void ReadTypeModifier(TSNode child, TSNode previous, const NodeIndex* index, TypeExtractionResult& result)
{
    const auto& gs = parser::GetGrammarSymbols();
    if (ts_node_is_missing(child))
    {
        return;
    }
    const TSSymbol token = ts_node_symbol(child);
    if (token == gs.tokOpenBracket)
    {
        result.isArray = true;
        result.arrayDepth++;
    }
    else if (token == gs.tokAt)
    {
        if (result.isHandle &&
            (ts_node_is_null(previous) || ts_node_is_missing(previous) || ts_node_symbol(previous) != gs.tokConst))
        {
            result.hasPrimitiveHandle = true;
        }
        result.isHandle = true;
        if (!result.isArray && !ts_node_is_null(previous) && ts_node_symbol(previous) == gs.symDatatype)
        {
            const TSNode inner = SyntaxOperands(previous, index).FirstNamed();
            if (!ts_node_is_null(inner) && ts_node_symbol(inner) == gs.symPrimitiveType)
            {
                result.hasPrimitiveHandle = true;
            }
        }
    }
    else if (token == gs.tokAmp)
    {
        result.isReference = true;
    }
    else if (token == gs.tokConst)
    {
        result.isConst = true;
    }
}

/** @brief Folds ordered type operands without recursive calls or recursively owned results. */
class TypeReadWorklist
{
  public:
    TypeReadWorklist(std::string_view sourceCode, const NodeIndex* index) : m_sourceCode(sourceCode), m_index(index)
    {
    }

    TypeExtractionResult Read(TSNode node)
    {
        Evaluate(node);
        while (!m_pending.empty())
        {
            const TypeReadStep step = m_pending.back();
            m_pending.pop_back();
            switch (step.action)
            {
            case TypeReadAction::Evaluate:
                Evaluate(step.node);
                break;
            case TypeReadAction::Child:
                ReadChild(step.node, step.previous);
                break;
            case TypeReadAction::Datatype:
                FinishDatatype(m_frames.back(), m_completed.baseTypeName, m_completed.kind);
                break;
            case TypeReadAction::TemplateArgument:
                FinishArgument(m_frames.back().result);
                break;
            case TypeReadAction::Finish:
                FinishType();
                break;
            }
        }
        return std::move(m_completed);
    }

  private:
    void Evaluate(TSNode node)
    {
        m_completed = {};
        if (ts_node_is_null(node) || ReadTypeLeaf(node, m_sourceCode, m_index, m_completed))
        {
            return;
        }

        m_frames.emplace_back();
        m_pending.push_back({TypeReadAction::Finish});
        const auto firstChild = m_pending.size();
        TSNode previous{};
        SyntaxOperands(node, m_index)
            .ForEachChild(
                [&](TSNode child)
                {
                    m_pending.push_back({TypeReadAction::Child, child, previous});
                    previous = child;
                    return true;
                });
        std::ranges::reverse(std::span(m_pending).subspan(firstChild));
    }

    void ReadChild(TSNode child, TSNode previous)
    {
        const auto& gs = parser::GetGrammarSymbols();
        const auto symbol = ts_node_symbol(child);
        auto& frame = m_frames.back();
        if (symbol == gs.symScope)
        {
            frame.scopePrefix = parser::GetNodeText(child, m_sourceCode);
        }
        else if (symbol == gs.symDatatype)
        {
            const TSNode inner = SyntaxOperands(child, m_index).FirstNamed();
            if (ts_node_is_null(inner))
            {
                FinishDatatype(frame, parser::GetNodeText(child, m_sourceCode), frame.result.kind);
            }
            else
            {
                m_pending.push_back({TypeReadAction::Datatype});
                m_pending.push_back({TypeReadAction::Evaluate, inner});
            }
        }
        else if (symbol == gs.symTemplateTypeList)
        {
            ReadTemplate(child, frame);
        }
        else if (!ts_node_is_named(child))
        {
            ReadTypeModifier(child, previous, m_index, frame.result);
        }
    }

    static void FinishDatatype(TypeReadFrame& frame, std::string_view name, TypeKind kind)
    {
        frame.datatypeText = frame.scopePrefix + std::string(name);
        frame.result.baseTypeName = frame.datatypeText;
        frame.result.kind = kind;
    }

    void ReadTemplate(TSNode list, TypeReadFrame& frame)
    {
        frame.result.isArray = true;
        frame.result.arrayDepth++;
        frame.result.templateName = frame.datatypeText;

        const auto firstArgument = m_pending.size();
        const auto typeSymbol = parser::GetGrammarSymbols().symType;
        SyntaxOperands(list, m_index)
            .ForEachNamed(
                [&](TSNode argument)
                {
                    if (ts_node_symbol(argument) == typeSymbol)
                    {
                        m_pending.push_back({TypeReadAction::Evaluate, argument});
                        m_pending.push_back({TypeReadAction::TemplateArgument});
                    }
                    return true;
                });
        std::ranges::reverse(std::span(m_pending).subspan(firstArgument));
    }

    void FinishArgument(TypeExtractionResult& result)
    {
        if (result.templateArgumentTypes.empty())
        {
            result.baseTypeName = m_completed.baseTypeName;
            result.hasPrimitiveHandle = m_completed.hasPrimitiveHandle;
            result.arrayDepth += m_completed.arrayDepth;
            if (m_completed.kind != TypeKind::Unknown)
            {
                result.kind = m_completed.kind;
            }
        }
        result.templateArgumentTypes.push_back(std::move(m_completed.baseTypeName));
    }

    void FinishType()
    {
        auto& result = m_frames.back().result;
        if (result.isArray)
        {
            result.kind = TypeKind::Array;
            if (result.templateName.empty())
            {
                result.templateName = "array";
            }
        }
        else if (result.isHandle && result.kind == TypeKind::Unknown)
        {
            result.kind = TypeKind::Handle;
        }
        m_completed = std::move(result);
        m_frames.pop_back();
    }

    std::string_view m_sourceCode;
    const NodeIndex* m_index;
    std::vector<TypeReadStep> m_pending;
    std::vector<TypeReadFrame> m_frames;
    TypeExtractionResult m_completed;
};

TypeExtractionResult ExtractTypeInfoFromAST(TSNode typeNode, std::string_view sourceCode, const NodeIndex* index)
{
    return TypeReadWorklist(sourceCode, index).Read(typeNode);
}

bool IsNullInitializer(TSNode valueNode, const NodeIndex* index)
{
    const TSNode node = UnwrapIndexedParentheses(valueNode, index);
    return !ts_node_is_null(node) && ts_node_symbol(node) == parser::GetGrammarSymbols().symNullLiteral;
}
} // namespace angel_lsp::analysis
