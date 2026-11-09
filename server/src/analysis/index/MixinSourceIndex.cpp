#include "analysis/index/MixinSourceIndex.h"

#include "analysis/index/NodeIndex.h"
#include "parser/AngelScriptParser.h"
#include "parser/SyntaxTree.h"
#include "utils/Utils.h"

#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <tree_sitter/api.h>
#include <utility>

namespace angel_lsp::analysis
{

[[nodiscard]] std::unique_ptr<NodeIndex> BuildNodeIndexForTree(const TSTree* tree)
{
    if (!tree)
    {
        return nullptr;
    }
    TSNode root = ts_tree_root_node(tree);
    if (ts_node_is_null(root))
    {
        return nullptr;
    }
    return std::make_unique<NodeIndex>(root);
}

[[nodiscard]] MixinFileCacheEntry BuildMixinFileCacheEntry(std::string source)
{
    if (source.empty())
    {
        return MixinFileCacheEntry{std::string{}, nullptr, parser::MakeTreePtr(), nullptr};
    }
    auto parser = std::make_unique<parser::AngelScriptParser>();
    auto tree = parser::MakeTreePtr(parser->Parse(source));
    std::unique_ptr<NodeIndex> nodeIndex = BuildNodeIndexForTree(tree.get());
    return MixinFileCacheEntry{
        std::move(source),
        std::move(parser),
        std::move(tree),
        std::move(nodeIndex),
    };
}

MixinFileCacheEntry BuildMixinFileCacheEntryFromText(std::string_view text)
{
    return BuildMixinFileCacheEntry(std::string(text));
}

MixinFileCacheEntry LoadMixinFileCacheEntry(std::string_view fileUri)
{
    std::ifstream file(utils::UriToPath(fileUri), std::ios::binary);
    if (!file.is_open())
    {
        return BuildMixinFileCacheEntry(std::string{});
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return BuildMixinFileCacheEntry(buffer.str());
}

} // namespace angel_lsp::analysis
