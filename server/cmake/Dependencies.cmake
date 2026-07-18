include(FetchContent)

# 1.1 JSON Parser (nlohmann/json)
FetchContent_Declare(json GIT_REPOSITORY https://github.com/nlohmann/json.git GIT_TAG v3.11.3)
FetchContent_MakeAvailable(json)

# 1.2 Logger (spdlog)
FetchContent_Declare(spdlog GIT_REPOSITORY https://github.com/gabime/spdlog.git GIT_TAG v1.13.0)
FetchContent_MakeAvailable(spdlog)

# 1.3 Unit Testing (doctest)
FetchContent_Declare(doctest GIT_REPOSITORY https://github.com/doctest/doctest.git GIT_TAG v2.4.11)
FetchContent_MakeAvailable(doctest)

# 1.4 LSP Framework
set(LSP_BUILD_EXAMPLES OFF CACHE BOOL "Disable Examples")
set(LSP_INSTALL OFF CACHE BOOL "Disable Install")

FetchContent_Declare(
    lsp_framework
    GIT_REPOSITORY https://github.com/leon-bckl/lsp-framework.git
    GIT_TAG master
)
FetchContent_MakeAvailable(lsp_framework)

# 1.5 Fast Hash Map (unordered_dense)
FetchContent_Declare(
    unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG v4.4.0
)
FetchContent_MakeAvailable(unordered_dense)
