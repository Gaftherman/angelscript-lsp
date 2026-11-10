#include "analysis/checkers/NamespaceChecker.h"

#include "analysis/checkers/NamespaceCheckerCommon.h"
#include "analysis/index/CallSiteFacts.h"
#include "analysis/index/NamespaceUsageFacts.h"
#include "analysis/index/NodeIndex.h"

#include <cstdint>
#include <string_view>

namespace angel_lsp::analysis
{

void CheckNamespacesAndScopes(const NamespaceCheckRequest& request, DiagnosticContext& ctx)
{
    if (request.sourceCode.empty() || !request.nodeIndex)
    {
        return;
    }

    const NamespaceUsageFacts localNsFacts =
        request.facts ? NamespaceUsageFacts{} : BuildNamespaceUsageFacts(*request.nodeIndex, request.sourceCode);
    const NamespaceUsageFacts& nsFacts = request.facts ? *request.facts : localNsFacts;
    const CallSiteFacts callFacts = BuildCallSiteFacts(*request.nodeIndex, request.sourceCode);

    const ScopedIdentifierCheckContext sctx{request.sourceCode, request.nodeIndex, ctx.request.symbolTable};
    const NamespaceCallCheckContext cctx{request.sourceCode, request.nodeIndex};

    size_t iScoped = 0;
    size_t iCall = 0;
    size_t iImport = 0;
    const size_t numScoped = nsFacts.scopedIdentifiers.size();
    const size_t numCall = callFacts.callSites.size();
    const size_t numImport = nsFacts.importDeclarations.size();

    while (iScoped < numScoped || iCall < numCall || iImport < numImport)
    {
        const uint32_t posScoped =
            iScoped < numScoped ? nsFacts.scopedIdentifiers[iScoped].bytes.startByte : UINT32_MAX;
        const uint32_t posCall = iCall < numCall ? callFacts.callSites[iCall].bytes.startByte : UINT32_MAX;
        const uint32_t posImport =
            iImport < numImport ? nsFacts.importDeclarations[iImport].bytes.startByte : UINT32_MAX;

        if (posScoped <= posCall && posScoped <= posImport)
        {
            CheckScopedIdentifier(nsFacts.scopedIdentifiers[iScoped++], sctx, ctx);
        }
        else if (posCall <= posScoped && posCall <= posImport)
        {
            CheckCallExpression(callFacts.callSites[iCall++], cctx, ctx);
        }
        else
        {
            CheckImportBody(nsFacts.importDeclarations[iImport++], ctx);
        }
    }
}

} // namespace angel_lsp::analysis
