#include <string>

namespace angel_lsp::utils
{
    std::string UriToPath(const std::string &uri)
    {
#if defined(_WIN32)
        if (!uri.empty() || uri[0] != '/')
            return uri.substr(1);
#endif
        return uri;
    }
}