/**
 * @file CodeActionExtractMethodStatements.cpp
 * @brief Analysis for statement range selection and input variable identification.
 */

#include "analysis/code_action/CodeActionExtractMethodAnalysis.h"
#include "utils/Utils.h"

namespace angel_lsp::analysis::code_action
{

/**
 * @brief Checks if a local definition is declared inside the statement range.
 * @param[in] def Local definition pointer.
 * @param[in] stmts Selected statement information.
 * @return True if declared inside the selection.
 */
static bool IsDeclaredInsideRange(const analysis::LocalDefinition* def, const ExtractMethodStatements& stmts)
{
    bool afterStart = def->startLine > stmts.selection.statementsRange.startLine ||
                      (def->startLine == stmts.selection.statementsRange.startLine &&
                       def->startCharacter >= stmts.selection.statementsRange.startCharacter);
    bool beforeEnd = def->endLine < stmts.selection.statementsRange.endLine ||
                     (def->endLine == stmts.selection.statementsRange.endLine &&
                      def->endCharacter <= stmts.selection.statementsRange.endCharacter);
    return afterStart && beforeEnd;
}

/**
 * @brief Validates if a referenced symbol is an outer input parameter or local variable.
 * @param[in] ref Scope reference.
 * @param[in] sc Enclosing scope.
 * @param[in] stmts Selected statement information.
 * @param[out] outInfo Discovered VarInfo.
 * @return True if reference is a valid extracted method input.
 */
static bool IsValidMethodInputRef(const analysis::LocalReference& ref, const analysis::Scope* sc,
                                  const ExtractMethodStatements& stmts, VarInfo& outInfo)
{
    if (ref.isMemberAccess || ref.startLine < stmts.selection.statementsRange.startLine ||
        ref.endLine > stmts.selection.statementsRange.endLine)
    {
        return false;
    }
    const analysis::LocalDefinition* def = analysis::ResolveInScope(sc, ref.name);
    if (!def)
    {
        return false;
    }
    if (stmts.selection.inClass && def->kind == analysis::LocalDefinitionKind::Field)
    {
        return false;
    }
    if (def->kind != analysis::LocalDefinitionKind::Variable && def->kind != analysis::LocalDefinitionKind::Parameter)
    {
        return false;
    }
    if (IsDeclaredInsideRange(def, stmts))
    {
        return false;
    }

    std::string tName = def->typeName.empty() ? "auto" : def->typeName;
    outInfo = {def->name, std::move(tName), false};
    return true;
}

/**
 * @brief Identifies candidate statements within the selected query range.
 * @param[in] query Incoming code action query context.
 * @return Extracted statement metadata if selection is valid, std::nullopt otherwise.
 */
std::optional<ExtractMethodStatements> FindSelectedStatements(const CodeActionQuery& query)
{
    if (!query.nodeIndex)
    {
        return std::nullopt;
    }
    auto selection = analysis::FindExtractMethodSelection(*query.nodeIndex, query.sourceCode, query.range);
    if (!selection)
    {
        return std::nullopt;
    }
    ExtractMethodStatements result;
    result.selection = std::move(*selection);
    result.selectedCode = std::string(utils::SliceSource(query.sourceCode, result.selection.statements.startByte,
                                                         result.selection.statements.endByte));
    return result;
}

std::vector<VarInfo> CollectMethodInputs(const ExtractMethodStatements& stmts, const analysis::Scope* fnScope)
{
    std::vector<VarInfo> inputParams;
    ankerl::unordered_dense::set<std::string> seenInputs;

    if (!fnScope)
    {
        return inputParams;
    }
    std::vector<const analysis::Scope*> worklist;
    worklist.push_back(fnScope);
    while (!worklist.empty())
    {
        const analysis::Scope* sc = worklist.back();
        worklist.pop_back();
        if (!sc)
        {
            continue;
        }

        for (const auto& ref : sc->references)
        {
            VarInfo info;
            if (IsValidMethodInputRef(ref, sc, stmts, info) && !seenInputs.contains(info.name))
            {
                seenInputs.insert(info.name);
                inputParams.push_back(std::move(info));
            }
        }

        for (const auto& child : sc->children)
        {
            worklist.push_back(child.get());
        }
    }
    return inputParams;
}

} // namespace angel_lsp::analysis::code_action
