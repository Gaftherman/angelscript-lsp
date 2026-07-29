
#include "config/ServerConfig.h"
#include <cstring>

namespace angel_lsp::config
{
    ServerConfig FromArgs(int argc, char **argv)
    {
        FeatureFlags feature = {};
        Info info = {"AngelScript Language Server", "1.0.0", ".as", ".as.predefined"};

        return ServerConfig{feature, info};
    }
}