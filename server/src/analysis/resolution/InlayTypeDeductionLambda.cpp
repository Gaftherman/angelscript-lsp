#include "analysis/index/ReturnSiteFacts.h"
#include "analysis/resolution/CallableAndLambdaResolver.h"
#include "analysis/resolution/ExpressionTypeResolver.h"
#include "analysis/resolution/InlayTypeDeduction.h"
#include "analysis/types/TypeSanitization.h"
#include "parser/Keywords.h"

#include <string>

namespace angel_lsp::analysis
{

std::string DeduceLambdaReturnTypeAt(const IndexedLambdaSite& site, const ExpressionTypeContext& ctx)
{
    if (site.returnExprStartByte != 0 && site.returnExprEndByte != 0)
    {
        std::string retType =
            ResolveExpressionTypeAt(IndexedByteRange{site.returnExprStartByte, site.returnExprEndByte}, ctx);
        if (!retType.empty())
        {
            return retType;
        }
    }

    if (site.hasReturnStatement && site.returnExprStartByte == 0)
    {
        return std::string(parser::keywords::Void);
    }

    if (ctx.nodeIndex)
    {
        const auto returnFacts = BuildReturnSiteFacts(*ctx.nodeIndex, ctx.sourceCode);
        for (const auto& ret : returnFacts.returns)
        {
            if (ret.owner == ReturnOwnerKind::Lambda && ret.bytes.startByte >= site.startByte &&
                ret.bytes.endByte <= site.endByte)
            {
                if (auto target = FuncdefTargetOfLambda(ret.lambdaTarget, ctx.symbolTable))
                {
                    return target->GetFuncdef().returnType;
                }
            }
        }
    }

    if (site.hasReturnStatement)
    {
        return std::string(parser::keywords::Void);
    }

    return {};
}

} // namespace angel_lsp::analysis
