#pragma once

#include "analysis/index/AssignmentSiteFacts.h"
#include "analysis/index/BinaryExpressionFacts.h"
#include "analysis/index/NodeIndex.h"
#include "analysis/index/NullFlowFacts.h"
#include "analysis/index/SpanFactSearch.h"

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace angel_lsp::analysis
{

/**
 * @brief Document-local identifier of a statement record in DefiniteAssignmentFacts.
 */
using DefiniteAssignmentRecordId = NullFlowNodeId;

/**
 * @brief Sentinel value indicating an absent statement record.
 */
inline constexpr DefiniteAssignmentRecordId NoDefiniteAssignmentRecord = NoNullFlowNode;

/**
 * @brief Syntactic classification of a statement or block record for definite assignment analysis.
 */
enum class DefiniteAssignmentStmtKind : uint8_t
{
    Other,
    Block,
    If,
    While,
    For,
    DoWhile,
    Switch,
    Case,
    Expression,
    VariableDeclaration,
    Return,
    Break,
    Continue
};

/**
 * @brief Local variable declarator within a variable declaration statement.
 */
struct DefiniteAssignmentLocalFact
{
    IndexedByteRange bytes;              ///< Byte span of the declarator node.
    SourceRange range;                   ///< Line and column span of the declarator.
    SourceRange nameRange;               ///< Line and column span of the declared variable name identifier.
    std::string_view name;               ///< Variable identifier spelling; views analysed source text.
    bool hasInitializer = false;         ///< True if an explicit initializer expression is present.
    bool isPrimitive = false;            ///< True if the declared type is a primitive value type.
    bool isTrackedUninitialized = false; ///< True if primitive without initializer; tracked by flow analysis.
};

/**
 * @brief An identifier occurrence evaluated in a read context.
 */
struct DefiniteAssignmentReadFact
{
    IndexedByteRange bytes; ///< Byte span of the identifier node.
    SourceRange range;      ///< Line and column span of the identifier node.
    std::string_view name;  ///< Identifier spelling; views analysed source text.
};

/**
 * @brief An argument expression passed into a function or method call.
 */
struct DefiniteAssignmentArgumentFact
{
    IndexedByteRange bytes;          ///< Byte span of the argument expression.
    SourceRange range;               ///< Line and column span of the argument expression.
    std::string_view name;           ///< Identifier spelling if the argument is a simple identifier.
    bool isSimpleIdentifier = false; ///< True if argument is an identifier or single-segment scoped identifier.
};

/**
 * @brief A call expression evaluated during statement flow.
 */
struct DefiniteAssignmentCallFact
{
    IndexedByteRange bytes;       ///< Byte span of the call expression.
    SourceRange range;            ///< Line and column span of the call expression.
    std::string_view calleeText;  ///< Full spelling of the callee expression.
    std::string_view memberName;  ///< Member function name if callee is a member access; empty otherwise.
    std::string_view objectText;  ///< Receiver object spelling if callee is a member access.
    IndexedByteRange objectBytes; ///< Receiver object byte span for pass-2 type resolution.
    SourceRange objectRange;      ///< Receiver object range.
    bool isMemberCall = false;    ///< True if callee is a member expression (object.member).
    uint32_t argumentBegin = 0;   ///< First slice index into DefiniteAssignmentFacts::callArguments.
    uint32_t argumentCount = 0;   ///< Number of call arguments.
};

/**
 * @brief Flat pre-order record for a statement, branch, loop, block, or leaf statement.
 */
struct DefiniteAssignmentStmtRecord
{
    DefiniteAssignmentStmtKind kind = DefiniteAssignmentStmtKind::Other;
    IndexedByteRange bytes;            ///< Byte span of this statement or block.
    SourceRange range;                 ///< Line and column span of this statement or block.
    uint32_t parentIndex = UINT32_MAX; ///< Enclosing statement record index in DefiniteAssignmentFacts::records.
    uint32_t childBegin = 0;           ///< First direct child index into DefiniteAssignmentFacts::childIndices.
    uint32_t childCount = 0;           ///< Number of direct child statement records.

    bool conditionAlwaysTrue = false; ///< True for while(true), for(;true;), or for(;;) where condition is true.
    bool isDefaultCase = false;       ///< True if this is a default case clause in a switch statement.

    uint32_t declaratorBegin = 0; ///< VariableDeclaration: first slice index into declarators.
    uint32_t declaratorCount = 0; ///< VariableDeclaration: number of declarators.

    uint32_t readBegin = 0; ///< First slice index into DefiniteAssignmentFacts::reads.
    uint32_t readCount = 0; ///< Number of identifier reads directly evaluated in this statement.

    uint32_t assignBegin = 0; ///< First slice index into DefiniteAssignmentFacts::assignments.
    uint32_t assignCount = 0; ///< Number of assignments evaluated in this statement.

    uint32_t callBegin = 0; ///< First slice index into DefiniteAssignmentFacts::calls.
    uint32_t callCount = 0; ///< Number of call expressions evaluated in this statement.
};

/**
 * @brief Function or lambda declaration body analyzed for definite assignment.
 */
struct DefiniteAssignmentFunctionFact
{
    IndexedByteRange bytes;                ///< Byte span of the function declaration or lambda expression node.
    SourceRange range;                     ///< Line and column span of the function or lambda node.
    IndexedByteRange bodyBytes;            ///< Byte span of the statement body block.
    SourceRange bodyRange;                 ///< Line and column span of the statement body block.
    bool isLambda = false;                 ///< True for lambda expressions; false for function declarations.
    uint32_t rootRecordIndex = UINT32_MAX; ///< Index of the body's statement record in records.
    uint32_t recordBegin = 0;              ///< First statement record index belonging to this callable.
    uint32_t recordCount = 0;              ///< Total number of statement records belonging to this callable.
};

/**
 * @brief AST-free facts for definite assignment analysis across all callable bodies in a document.
 * @note Text views point into the source passed to BuildDefiniteAssignmentFacts, which must outlive this object.
 */
struct DefiniteAssignmentFacts
{
    std::vector<DefiniteAssignmentStmtRecord> records;     ///< Pre-order statement and block records.
    std::vector<uint32_t> childIndices;                    ///< Direct child statement record indices.
    std::vector<DefiniteAssignmentFunctionFact> functions; ///< Analyzed functions and lambdas.
    std::vector<DefiniteAssignmentLocalFact> declarators;  ///< Declared local variables.
    std::vector<DefiniteAssignmentReadFact> reads;         ///< Identifier read occurrences.
    std::vector<AssignmentSiteFact> assignments;           ///< Assignment expressions (reusing AssignmentSiteFacts).
    std::vector<DefiniteAssignmentCallFact> calls;         ///< Call expressions evaluated in statements.
    std::vector<DefiniteAssignmentArgumentFact> callArguments; ///< Call arguments sliced by DefiniteAssignmentCallFact.
    AssignmentSiteFacts assignmentSites;                       ///< Complete document-level assignment facts.

    /**
     * @brief Finds the statement record that spans exactly the given bytes.
     * @param[in] bytes Byte span of the statement node.
     * @return Matching record pointer, or nullptr if not found.
     */
    [[nodiscard]] inline const DefiniteAssignmentStmtRecord* FindRecord(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(records, bytes);
    }

    /**
     * @brief Finds the function fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the function or lambda node.
     * @return Matching function fact pointer, or nullptr if not found.
     */
    [[nodiscard]] inline const DefiniteAssignmentFunctionFact* FindFunction(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(functions, bytes);
    }

    /**
     * @brief Finds the local variable declarator fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the declarator node.
     * @return Matching declarator fact pointer, or nullptr if not found.
     */
    [[nodiscard]] inline const DefiniteAssignmentLocalFact* FindDeclarator(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(declarators, bytes);
    }

    /**
     * @brief Finds the assignment fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the assignment expression node.
     * @return Matching assignment fact pointer, or nullptr if not found.
     */
    [[nodiscard]] inline const AssignmentSiteFact* FindAssignment(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(assignments, bytes);
    }

    /**
     * @brief Finds the call fact that spans exactly the given bytes.
     * @param[in] bytes Byte span of the call expression node.
     * @return Matching call fact pointer, or nullptr if not found.
     */
    [[nodiscard]] inline const DefiniteAssignmentCallFact* FindCall(IndexedByteRange bytes) const noexcept
    {
        return FindExactSpan(calls, bytes);
    }

    /**
     * @brief Returns a slice of direct child statement record indices for a parent record.
     * @param[in] record Parent statement record.
     * @return Contiguous span of record indices.
     */
    [[nodiscard]] inline std::span<const uint32_t> ChildrenOf(const DefiniteAssignmentStmtRecord& record) const noexcept
    {
        return record.childBegin < childIndices.size()
                   ? std::span<const uint32_t>(childIndices.data() + record.childBegin, record.childCount)
                   : std::span<const uint32_t>{};
    }

    /**
     * @brief Returns a slice of declarator facts for a variable declaration statement record.
     * @param[in] record Variable declaration statement record.
     * @return Contiguous span of declarator facts.
     */
    [[nodiscard]] inline std::span<const DefiniteAssignmentLocalFact>
    DeclaratorsOf(const DefiniteAssignmentStmtRecord& record) const noexcept
    {
        return record.declaratorBegin < declarators.size()
                   ? std::span<const DefiniteAssignmentLocalFact>(declarators.data() + record.declaratorBegin,
                                                                  record.declaratorCount)
                   : std::span<const DefiniteAssignmentLocalFact>{};
    }

    /**
     * @brief Returns a slice of identifier read facts evaluated in a statement record.
     * @param[in] record Statement record.
     * @return Contiguous span of read facts.
     */
    [[nodiscard]] inline std::span<const DefiniteAssignmentReadFact>
    ReadsOf(const DefiniteAssignmentStmtRecord& record) const noexcept
    {
        return record.readBegin < reads.size()
                   ? std::span<const DefiniteAssignmentReadFact>(reads.data() + record.readBegin, record.readCount)
                   : std::span<const DefiniteAssignmentReadFact>{};
    }

    /**
     * @brief Returns a slice of assignment facts evaluated in a statement record.
     * @param[in] record Statement record.
     * @return Contiguous span of assignment facts.
     */
    [[nodiscard]] inline std::span<const AssignmentSiteFact>
    AssignmentsOf(const DefiniteAssignmentStmtRecord& record) const noexcept
    {
        return record.assignBegin < assignments.size()
                   ? std::span<const AssignmentSiteFact>(assignments.data() + record.assignBegin, record.assignCount)
                   : std::span<const AssignmentSiteFact>{};
    }

    /**
     * @brief Returns a slice of call facts evaluated in a statement record.
     * @param[in] record Statement record.
     * @return Contiguous span of call facts.
     */
    [[nodiscard]] inline std::span<const DefiniteAssignmentCallFact>
    CallsOf(const DefiniteAssignmentStmtRecord& record) const noexcept
    {
        return record.callBegin < calls.size()
                   ? std::span<const DefiniteAssignmentCallFact>(calls.data() + record.callBegin, record.callCount)
                   : std::span<const DefiniteAssignmentCallFact>{};
    }

    /**
     * @brief Returns a slice of argument facts for a call fact.
     * @param[in] call Call fact.
     * @return Contiguous span of argument facts.
     */
    [[nodiscard]] inline std::span<const DefiniteAssignmentArgumentFact>
    ArgumentsOf(const DefiniteAssignmentCallFact& call) const noexcept
    {
        return call.argumentBegin < callArguments.size()
                   ? std::span<const DefiniteAssignmentArgumentFact>(callArguments.data() + call.argumentBegin,
                                                                     call.argumentCount)
                   : std::span<const DefiniteAssignmentArgumentFact>{};
    }

    /**
     * @brief Returns a slice of statement records belonging to a function.
     * @param[in] fn Function fact.
     * @return Contiguous span of statement records.
     */
    [[nodiscard]] inline std::span<const DefiniteAssignmentStmtRecord>
    RecordsOf(const DefiniteAssignmentFunctionFact& fn) const noexcept
    {
        return fn.recordBegin < records.size()
                   ? std::span<const DefiniteAssignmentStmtRecord>(records.data() + fn.recordBegin, fn.recordCount)
                   : std::span<const DefiniteAssignmentStmtRecord>{};
    }
};

/**
 * @brief Extracts all AST-free definite assignment facts across callable bodies in the document.
 * @param[in] index Precomputed node index of the document.
 * @param[in] source Document source text buffer; must outlive returned facts.
 * @return Populated definite assignment facts structure.
 */
[[nodiscard]] DefiniteAssignmentFacts BuildDefiniteAssignmentFacts(const NodeIndex& index, std::string_view source);

} // namespace angel_lsp::analysis
