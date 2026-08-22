#include <doctest/doctest.h>
#include "config/ServerConfig.h"
#include <vector>
#include <string>

using namespace angel_lsp::config;

namespace
{
    // Helper to create char* array from vector of strings
    struct ArgvHelper
    {
        std::vector<std::string> storage;
        std::vector<char*> argv;

        ArgvHelper(std::initializer_list<std::string> args)
            : storage(args)
        {
            argv.reserve(storage.size());
            for (auto &s : storage)
            {
                argv.push_back(s.data());
            }
        }

        int argc() const
        {
            return static_cast<int>(argv.size());
        }

        char** data()
        {
            return argv.data();
        }
    };
}

TEST_SUITE("ServerConfig - CLI Argument Parsing")
{
    TEST_CASE("Default values when no arguments provided")
    {
        SUBCASE("Null argv or zero argc")
        {
            ServerConfig config1 = FromArgs(0, nullptr);
            CHECK(config1.features.enableHover == true);
            CHECK(config1.features.enableDefinition == true);
            CHECK(config1.features.enableCompletion == true);
            CHECK(config1.features.enableSemanticTokens == true);
            CHECK(config1.features.enableSignatureHelp == true);
            CHECK(config1.features.enablePredefinedLoader == true);
            CHECK(config1.info.name == "AngelScript Language Server");
            CHECK(config1.info.version == "1.0.0");
            CHECK(config1.info.fileExtension == ".as");
            CHECK(config1.info.predefinedFileExtension == ".as.predefined");
            CHECK(config1.info.locale == "en");
            CHECK(config1.info.showHelp == false);
            CHECK(config1.info.showVersion == false);
            CHECK(config1.types.stringTypeName == "string");
            CHECK(config1.types.arrayTypeName == "array");
            CHECK(config1.types.registeredSymbols.empty());
        }

        SUBCASE("Only executable name in argv")
        {
            ArgvHelper args{"angel_lsp"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableHover == true);
            CHECK(config.features.enableDefinition == true);
            CHECK(config.features.enableCompletion == true);
            CHECK(config.features.enableSemanticTokens == true);
            CHECK(config.features.enableSignatureHelp == true);
            CHECK(config.features.enablePredefinedLoader == true);
            CHECK(config.info.showHelp == false);
            CHECK(config.info.showVersion == false);
            CHECK(config.info.locale == "en");
            CHECK(config.info.fileExtension == ".as");
            CHECK(config.info.predefinedFileExtension == ".as.predefined");
        }
    }

    TEST_CASE("Boolean feature flags with inline '=' syntax (--flag=value)")
    {
        SUBCASE("Hover flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-hover=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableHover == false);

            ArgvHelper args2{"angel_lsp", "--enable-hover=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableHover == true);

            ArgvHelper args3{"angel_lsp", "--enable-hover=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableHover == false);

            ArgvHelper args4{"angel_lsp", "--enable-hover=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableHover == true);
        }

        SUBCASE("Definition flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-definition=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableDefinition == false);

            ArgvHelper args2{"angel_lsp", "--enable-definition=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableDefinition == true);

            ArgvHelper args3{"angel_lsp", "--enable-definition=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableDefinition == false);

            ArgvHelper args4{"angel_lsp", "--enable-definition=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableDefinition == true);
        }

        SUBCASE("Completion flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-completion=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableCompletion == false);

            ArgvHelper args2{"angel_lsp", "--enable-completion=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableCompletion == true);

            ArgvHelper args3{"angel_lsp", "--enable-completion=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableCompletion == false);

            ArgvHelper args4{"angel_lsp", "--enable-completion=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableCompletion == true);
        }

        SUBCASE("Semantic tokens flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-semantic-tokens=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableSemanticTokens == false);

            ArgvHelper args2{"angel_lsp", "--enable-semantic-tokens=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableSemanticTokens == true);

            ArgvHelper args3{"angel_lsp", "--enable-semantic-tokens=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableSemanticTokens == false);

            ArgvHelper args4{"angel_lsp", "--enable-semantic-tokens=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableSemanticTokens == true);
        }

        SUBCASE("Signature help flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-signature-help=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableSignatureHelp == false);

            ArgvHelper args2{"angel_lsp", "--enable-signature-help=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableSignatureHelp == true);

            ArgvHelper args3{"angel_lsp", "--enable-signature-help=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableSignatureHelp == false);

            ArgvHelper args4{"angel_lsp", "--enable-signature-help=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableSignatureHelp == true);
        }

        SUBCASE("Predefined loader flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-predefined-loader=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enablePredefinedLoader == false);

            ArgvHelper args2{"angel_lsp", "--enable-predefined-loader=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enablePredefinedLoader == true);

            ArgvHelper args3{"angel_lsp", "--enable-predefined-loader=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enablePredefinedLoader == false);

            ArgvHelper args4{"angel_lsp", "--enable-predefined-loader=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enablePredefinedLoader == true);
        }
    }

    TEST_CASE("Boolean feature flags with space-separated syntax (--flag value)")
    {
        SUBCASE("Hover flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-hover", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableHover == false);

            ArgvHelper args2{"angel_lsp", "--enable-hover", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableHover == true);

            ArgvHelper args3{"angel_lsp", "--enable-hover", "0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableHover == false);

            ArgvHelper args4{"angel_lsp", "--enable-hover", "1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableHover == true);
        }

        SUBCASE("Definition flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-definition", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableDefinition == false);

            ArgvHelper args2{"angel_lsp", "--enable-definition", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableDefinition == true);
        }

        SUBCASE("Completion flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-completion", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableCompletion == false);

            ArgvHelper args2{"angel_lsp", "--enable-completion", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableCompletion == true);
        }

        SUBCASE("Semantic tokens flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-semantic-tokens", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableSemanticTokens == false);

            ArgvHelper args2{"angel_lsp", "--enable-semantic-tokens", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableSemanticTokens == true);
        }

        SUBCASE("Signature help flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-signature-help", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableSignatureHelp == false);

            ArgvHelper args2{"angel_lsp", "--enable-signature-help", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableSignatureHelp == true);
        }

        SUBCASE("Predefined loader flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-predefined-loader", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enablePredefinedLoader == false);

            ArgvHelper args2{"angel_lsp", "--enable-predefined-loader", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enablePredefinedLoader == true);
        }
    }

    TEST_CASE("Disable flags (--disable-*)")
    {
        SUBCASE("Plain disable flags")
        {
            ArgvHelper args{"angel_lsp",
                             "--disable-hover",
                             "--disable-definition",
                             "--disable-completion",
                             "--disable-semantic-tokens",
                             "--disable-signature-help",
                             "--disable-predefined-loader"};

            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableHover == false);
            CHECK(config.features.enableDefinition == false);
            CHECK(config.features.enableCompletion == false);
            CHECK(config.features.enableSemanticTokens == false);
            CHECK(config.features.enableSignatureHelp == false);
            CHECK(config.features.enablePredefinedLoader == false);
        }

        SUBCASE("Disable flags with inline values")
        {
            ArgvHelper args1{"angel_lsp", "--disable-hover=true"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableHover == false);

            ArgvHelper args2{"angel_lsp", "--disable-hover=false"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableHover == true);

            ArgvHelper args3{"angel_lsp", "--disable-completion=1"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableCompletion == false);

            ArgvHelper args4{"angel_lsp", "--disable-completion=0"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableCompletion == true);
        }
    }

    TEST_CASE("Standalone --enable-* flags (without explicit value)")
    {
        ArgvHelper args{"angel_lsp",
                         "--disable-hover",
                         "--enable-hover",
                         "--disable-completion",
                         "--enable-completion"};

        ServerConfig config = FromArgs(args.argc(), args.data());
        CHECK(config.features.enableHover == true);
        CHECK(config.features.enableCompletion == true);
    }

    TEST_CASE("String options (--locale, --file-ext, --predefined-ext)")
    {
        SUBCASE("Inline '=' syntax")
        {
            ArgvHelper args{"angel_lsp",
                             "--locale=es-ES",
                             "--file-ext=.angel",
                             "--predefined-ext=.angel.predefined"};

            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.info.locale == "es-ES");
            CHECK(config.info.fileExtension == ".angel");
            CHECK(config.info.predefinedFileExtension == ".angel.predefined");
        }

        SUBCASE("Space-separated syntax")
        {
            ArgvHelper args{"angel_lsp",
                             "--locale", "fr-FR",
                             "--file-ext", ".as_script",
                             "--predefined-ext", ".predef"};

            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.info.locale == "fr-FR");
            CHECK(config.info.fileExtension == ".as_script");
            CHECK(config.info.predefinedFileExtension == ".predef");
        }

        SUBCASE("Alternative alias flags (--file-extension, --predefined-extension)")
        {
            ArgvHelper args{"angel_lsp",
                             "--file-extension=.as3",
                             "--predefined-extension=.as3.predefined"};

            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.info.fileExtension == ".as3");
            CHECK(config.info.predefinedFileExtension == ".as3.predefined");
        }
    }

    TEST_CASE("Help and version flags")
    {
        SUBCASE("--help and -h")
        {
            ArgvHelper args1{"angel_lsp", "--help"};
            CHECK(FromArgs(args1.argc(), args1.data()).info.showHelp == true);

            ArgvHelper args2{"angel_lsp", "-h"};
            CHECK(FromArgs(args2.argc(), args2.data()).info.showHelp == true);
        }

        SUBCASE("--version and -v")
        {
            ArgvHelper args1{"angel_lsp", "--version"};
            CHECK(FromArgs(args1.argc(), args1.data()).info.showVersion == true);

            ArgvHelper args2{"angel_lsp", "-v"};
            CHECK(FromArgs(args2.argc(), args2.data()).info.showVersion == true);
        }
    }

    TEST_CASE("Case-insensitivity and alternative boolean spellings")
    {
        ArgvHelper args1{"angel_lsp", "--enable-hover=FALSE", "--enable-definition=False", "--enable-completion=OFF", "--enable-semantic-tokens=no"};
        ServerConfig config1 = FromArgs(args1.argc(), args1.data());
        CHECK(config1.features.enableHover == false);
        CHECK(config1.features.enableDefinition == false);
        CHECK(config1.features.enableCompletion == false);
        CHECK(config1.features.enableSemanticTokens == false);

        ArgvHelper args2{"angel_lsp", "--enable-hover=TRUE", "--enable-definition=True", "--enable-completion=ON", "--enable-semantic-tokens=yes"};
        ServerConfig config2 = FromArgs(args2.argc(), args2.data());
        CHECK(config2.features.enableHover == true);
        CHECK(config2.features.enableDefinition == true);
        CHECK(config2.features.enableCompletion == true);
        CHECK(config2.features.enableSemanticTokens == true);
    }

    TEST_CASE("Complex combinations of multiple flags")
    {
        ArgvHelper args{"angel_lsp",
                         "--disable-hover",
                         "--enable-definition=true",
                         "--enable-completion", "false",
                         "--enable-semantic-tokens=1",
                         "--enable-signature-help", "0",
                         "--locale=es-ES",
                         "--file-ext", ".angelscript",
                         "--predefined-ext=.custom.predef"};

        ServerConfig config = FromArgs(args.argc(), args.data());
        CHECK(config.features.enableHover == false);
        CHECK(config.features.enableDefinition == true);
        CHECK(config.features.enableCompletion == false);
        CHECK(config.features.enableSemanticTokens == true);
        CHECK(config.features.enableSignatureHelp == false);
        CHECK(config.features.enablePredefinedLoader == true);
        CHECK(config.info.locale == "es-ES");
        CHECK(config.info.fileExtension == ".angelscript");
        CHECK(config.info.predefinedFileExtension == ".custom.predef");
    }

    TEST_CASE("Robustness and edge cases")
    {
        SUBCASE("Unknown flags and positional arguments are safely ignored")
        {
            ArgvHelper args{"angel_lsp", "--unknown-flag", "--foo=bar", "positional_file.as", "--enable-hover=false"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableHover == false);
            CHECK(config.features.enableDefinition == true);
        }

        SUBCASE("Trailing flag missing value")
        {
            ArgvHelper args1{"angel_lsp", "--locale"};
            ServerConfig config1 = FromArgs(args1.argc(), args1.data());
            CHECK(config1.info.locale == "en");

            ArgvHelper args2{"angel_lsp", "--file-ext"};
            ServerConfig config2 = FromArgs(args2.argc(), args2.data());
            CHECK(config2.info.fileExtension == ".as");

            ArgvHelper args3{"angel_lsp", "--predefined-ext"};
            ServerConfig config3 = FromArgs(args3.argc(), args3.data());
            CHECK(config3.info.predefinedFileExtension == ".as.predefined");
        }

        SUBCASE("Null pointers inside argv")
        {
            char* rawArgs[] = { (char*)"angel_lsp", nullptr, (char*)"--disable-hover", nullptr, (char*)"--locale=es" };
            ServerConfig config = FromArgs(5, rawArgs);
            CHECK(config.features.enableHover == false);
            CHECK(config.info.locale == "es");
        }

        SUBCASE("Empty string arguments in argv")
        {
            ArgvHelper args{"angel_lsp", "", "--enable-hover=false", ""};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableHover == false);
        }

        SUBCASE("Negative argc or null argv variations")
        {
            ServerConfig config1 = FromArgs(-1, nullptr);
            CHECK(config1.features.enableHover == true);
            CHECK(config1.info.locale == "en");

            char* emptyArgv[] = { nullptr };
            ServerConfig config2 = FromArgs(1, emptyArgv);
            CHECK(config2.features.enableHover == true);
        }

        SUBCASE("Empty inline values and multiple equals signs")
        {
            ArgvHelper args1{"angel_lsp", "--enable-hover="};
            ServerConfig config1 = FromArgs(args1.argc(), args1.data());
            CHECK(config1.features.enableHover == true);

            ArgvHelper args2{"angel_lsp", "--file-ext=foo=bar=baz"};
            ServerConfig config2 = FromArgs(args2.argc(), args2.data());
            CHECK(config2.info.fileExtension == "foo=bar=baz");
        }

        SUBCASE("Non-boolean parameter following space-separated flag")
        {
            ArgvHelper args{"angel_lsp", "--enable-hover", "invalid_bool", "--locale", "de-DE"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            // "--enable-hover" with non-bool next arg shouldn't consume "invalid_bool", should default to true
            CHECK(config.features.enableHover == true);
            CHECK(config.info.locale == "de-DE");
        }

        SUBCASE("All alternative alias flags")
        {
            ArgvHelper args{"angel_lsp",
                             "--disable-semantictokens",
                             "--disable-signaturehelp",
                             "--disable-predefinedloader"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableSemanticTokens == false);
            CHECK(config.features.enableSignatureHelp == false);
            CHECK(config.features.enablePredefinedLoader == false);

            ArgvHelper args2{"angel_lsp",
                              "--enable-semantictokens=1",
                              "--enable-signaturehelp=1",
                              "--enable-predefinedloader=1"};
            ServerConfig config2 = FromArgs(args2.argc(), args2.data());
            CHECK(config2.features.enableSemanticTokens == true);
            CHECK(config2.features.enableSignatureHelp == true);
            CHECK(config2.features.enablePredefinedLoader == true);
        }

        SUBCASE("Flag override precedence (last argument wins)")
        {
            ArgvHelper args{"angel_lsp",
                             "--enable-hover=false",
                             "--enable-hover=true",
                             "--disable-hover",
                             "--enable-hover",
                             "--disable-hover=false"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            // --disable-hover=false enables hover
            CHECK(config.features.enableHover == true);

            ArgvHelper args2{"angel_lsp",
                              "--enable-definition=true",
                              "--disable-definition"};
            ServerConfig config2 = FromArgs(args2.argc(), args2.data());
            CHECK(config2.features.enableDefinition == false);
        }
    }
}

