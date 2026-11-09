#include "analysis/types/TypeConversionValues.h"

namespace angel_lsp::analysis
{

std::string MakeConversionEdgeKey(std::string_view from, std::string_view to)
{
    std::string key;
    key.reserve(from.size() + to.size() + 2);
    key.append(from).append("->").append(to);
    return key;
}

std::string SingleArgumentType(const std::vector<ParameterInformation>& parameters)
{
    return parameters.empty() ? std::string() : CanonicalizeType(parameters[0].typeName);
}

namespace TypeConversionChecker
{
bool evaluateUserConversions(const TypeInfo& source, const TypeInfo& target, size_t depth,
                             ankerl::unordered_dense::set<std::string>& visitedEdges)
{
    if (depth >= MAX_CONVERSION_DEPTH)
    {
        return false;
    }
    if (source == target)
    {
        return true;
    }
    const std::string edgeKey = MakeConversionEdgeKey(source.name, target.name);
    return visitedEdges.contains(edgeKey);
}

bool canConvertImplicitly(const TypeInfo& source, const TypeInfo& target, size_t depth,
                          ankerl::unordered_dense::set<std::string>& visitedEdges)
{
    if (depth >= MAX_CONVERSION_DEPTH)
    {
        return false;
    }

    if (source == target)
    {
        return true;
    }

    const std::string edgeKey = MakeConversionEdgeKey(source.name, target.name);
    if (!visitedEdges.insert(edgeKey).second)
    {
        return false;
    }

    bool convertible = evaluateUserConversions(source, target, depth + 1, visitedEdges);
    visitedEdges.erase(edgeKey);
    return convertible;
}
} // namespace TypeConversionChecker

} // namespace angel_lsp::analysis
