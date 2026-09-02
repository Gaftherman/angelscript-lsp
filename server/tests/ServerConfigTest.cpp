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
            CHECK(config1.features.enableDocumentSymbols == true);
            CHECK(config1.features.enableWorkspaceSymbols == true);
            CHECK(config1.features.enableReferences == true);
            CHECK(config1.features.enableRename == true);
            CHECK(config1.features.enableDocumentHighlight == true);
            CHECK(config1.features.enableFoldingRange == true);
            CHECK(config1.features.enableInlayHints == true);
            CHECK(config1.features.enableCodeAction == true);
            CHECK(config1.features.enableFormatting == true);
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
            CHECK(config.features.enableDocumentSymbols == true);
            CHECK(config.features.enableWorkspaceSymbols == true);
            CHECK(config.features.enableReferences == true);
            CHECK(config.features.enableRename == true);
            CHECK(config.features.enableDocumentHighlight == true);
            CHECK(config.features.enableFoldingRange == true);
            CHECK(config.features.enableInlayHints == true);
            CHECK(config.features.enableCodeAction == true);
            CHECK(config.features.enableFormatting == true);
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

        SUBCASE("Document symbols flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-document-symbols=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableDocumentSymbols == false);

            ArgvHelper args2{"angel_lsp", "--enable-document-symbols=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableDocumentSymbols == true);

            ArgvHelper args3{"angel_lsp", "--enable-document-symbols=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableDocumentSymbols == false);

            ArgvHelper args4{"angel_lsp", "--enable-document-symbols=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableDocumentSymbols == true);
        }

        SUBCASE("Workspace symbols flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-workspace-symbols=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableWorkspaceSymbols == false);

            ArgvHelper args2{"angel_lsp", "--enable-workspace-symbols=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableWorkspaceSymbols == true);

            ArgvHelper args3{"angel_lsp", "--enable-workspace-symbols=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableWorkspaceSymbols == false);

            ArgvHelper args4{"angel_lsp", "--enable-workspace-symbols=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableWorkspaceSymbols == true);
        }

        SUBCASE("References flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-references=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableReferences == false);

            ArgvHelper args2{"angel_lsp", "--enable-references=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableReferences == true);

            ArgvHelper args3{"angel_lsp", "--enable-references=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableReferences == false);

            ArgvHelper args4{"angel_lsp", "--enable-references=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableReferences == true);
        }

        SUBCASE("Rename flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-rename=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableRename == false);

            ArgvHelper args2{"angel_lsp", "--enable-rename=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableRename == true);

            ArgvHelper args3{"angel_lsp", "--enable-rename=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableRename == false);

            ArgvHelper args4{"angel_lsp", "--enable-rename=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableRename == true);
        }

        SUBCASE("Document highlight flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-document-highlight=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableDocumentHighlight == false);

            ArgvHelper args2{"angel_lsp", "--enable-document-highlight=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableDocumentHighlight == true);

            ArgvHelper args3{"angel_lsp", "--enable-document-highlight=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableDocumentHighlight == false);

            ArgvHelper args4{"angel_lsp", "--enable-document-highlight=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableDocumentHighlight == true);
        }

        SUBCASE("Folding range flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-folding-range=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableFoldingRange == false);

            ArgvHelper args2{"angel_lsp", "--enable-folding-range=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableFoldingRange == true);

            ArgvHelper args3{"angel_lsp", "--enable-folding-range=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableFoldingRange == false);

            ArgvHelper args4{"angel_lsp", "--enable-folding-range=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableFoldingRange == true);
        }

        SUBCASE("Inlay hints flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-inlay-hints=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableInlayHints == false);

            ArgvHelper args2{"angel_lsp", "--enable-inlay-hints=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableInlayHints == true);

            ArgvHelper args3{"angel_lsp", "--enable-inlay-hints=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableInlayHints == false);

            ArgvHelper args4{"angel_lsp", "--enable-inlay-hints=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableInlayHints == true);
        }

        SUBCASE("Code action flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-code-action=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableCodeAction == false);

            ArgvHelper args2{"angel_lsp", "--enable-code-action=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableCodeAction == true);

            ArgvHelper args3{"angel_lsp", "--enable-code-action=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableCodeAction == false);

            ArgvHelper args4{"angel_lsp", "--enable-code-action=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableCodeAction == true);
        }

        SUBCASE("Formatting flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-formatting=false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableFormatting == false);

            ArgvHelper args2{"angel_lsp", "--enable-formatting=true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableFormatting == true);

            ArgvHelper args3{"angel_lsp", "--enable-formatting=0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableFormatting == false);

            ArgvHelper args4{"angel_lsp", "--enable-formatting=1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableFormatting == true);
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

        SUBCASE("Document symbols flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-document-symbols", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableDocumentSymbols == false);

            ArgvHelper args2{"angel_lsp", "--enable-document-symbols", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableDocumentSymbols == true);

            ArgvHelper args3{"angel_lsp", "--enable-document-symbols", "0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableDocumentSymbols == false);

            ArgvHelper args4{"angel_lsp", "--enable-document-symbols", "1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableDocumentSymbols == true);
        }

        SUBCASE("Workspace symbols flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-workspace-symbols", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableWorkspaceSymbols == false);

            ArgvHelper args2{"angel_lsp", "--enable-workspace-symbols", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableWorkspaceSymbols == true);

            ArgvHelper args3{"angel_lsp", "--enable-workspace-symbols", "0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableWorkspaceSymbols == false);

            ArgvHelper args4{"angel_lsp", "--enable-workspace-symbols", "1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableWorkspaceSymbols == true);
        }

        SUBCASE("References flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-references", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableReferences == false);

            ArgvHelper args2{"angel_lsp", "--enable-references", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableReferences == true);

            ArgvHelper args3{"angel_lsp", "--enable-references", "0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableReferences == false);

            ArgvHelper args4{"angel_lsp", "--enable-references", "1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableReferences == true);
        }

        SUBCASE("Rename flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-rename", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableRename == false);

            ArgvHelper args2{"angel_lsp", "--enable-rename", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableRename == true);

            ArgvHelper args3{"angel_lsp", "--enable-rename", "0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableRename == false);

            ArgvHelper args4{"angel_lsp", "--enable-rename", "1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableRename == true);
        }

        SUBCASE("Document highlight flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-document-highlight", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableDocumentHighlight == false);

            ArgvHelper args2{"angel_lsp", "--enable-document-highlight", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableDocumentHighlight == true);

            ArgvHelper args3{"angel_lsp", "--enable-document-highlight", "0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableDocumentHighlight == false);

            ArgvHelper args4{"angel_lsp", "--enable-document-highlight", "1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableDocumentHighlight == true);
        }

        SUBCASE("Folding range flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-folding-range", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableFoldingRange == false);

            ArgvHelper args2{"angel_lsp", "--enable-folding-range", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableFoldingRange == true);

            ArgvHelper args3{"angel_lsp", "--enable-folding-range", "0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableFoldingRange == false);

            ArgvHelper args4{"angel_lsp", "--enable-folding-range", "1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableFoldingRange == true);
        }

        SUBCASE("Inlay hints flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-inlay-hints", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableInlayHints == false);

            ArgvHelper args2{"angel_lsp", "--enable-inlay-hints", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableInlayHints == true);

            ArgvHelper args3{"angel_lsp", "--enable-inlay-hints", "0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableInlayHints == false);

            ArgvHelper args4{"angel_lsp", "--enable-inlay-hints", "1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableInlayHints == true);
        }

        SUBCASE("Code action flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-code-action", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableCodeAction == false);

            ArgvHelper args2{"angel_lsp", "--enable-code-action", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableCodeAction == true);

            ArgvHelper args3{"angel_lsp", "--enable-code-action", "0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableCodeAction == false);

            ArgvHelper args4{"angel_lsp", "--enable-code-action", "1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableCodeAction == true);
        }

        SUBCASE("Formatting flag")
        {
            ArgvHelper args1{"angel_lsp", "--enable-formatting", "false"};
            CHECK(FromArgs(args1.argc(), args1.data()).features.enableFormatting == false);

            ArgvHelper args2{"angel_lsp", "--enable-formatting", "true"};
            CHECK(FromArgs(args2.argc(), args2.data()).features.enableFormatting == true);

            ArgvHelper args3{"angel_lsp", "--enable-formatting", "0"};
            CHECK(FromArgs(args3.argc(), args3.data()).features.enableFormatting == false);

            ArgvHelper args4{"angel_lsp", "--enable-formatting", "1"};
            CHECK(FromArgs(args4.argc(), args4.data()).features.enableFormatting == true);
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
                             "--disable-predefined-loader",
                             "--disable-document-symbols",
                             "--disable-workspace-symbols",
                             "--disable-references",
                             "--disable-rename",
                             "--disable-document-highlight",
                             "--disable-folding-range",
                             "--disable-inlay-hints",
                             "--disable-code-action",
                             "--disable-formatting"};

            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableHover == false);
            CHECK(config.features.enableDefinition == false);
            CHECK(config.features.enableCompletion == false);
            CHECK(config.features.enableSemanticTokens == false);
            CHECK(config.features.enableSignatureHelp == false);
            CHECK(config.features.enablePredefinedLoader == false);
            CHECK(config.features.enableDocumentSymbols == false);
            CHECK(config.features.enableWorkspaceSymbols == false);
            CHECK(config.features.enableReferences == false);
            CHECK(config.features.enableRename == false);
            CHECK(config.features.enableDocumentHighlight == false);
            CHECK(config.features.enableFoldingRange == false);
            CHECK(config.features.enableInlayHints == false);
            CHECK(config.features.enableCodeAction == false);
            CHECK(config.features.enableFormatting == false);
        }

        SUBCASE("Disable flags with inline values")
        {
            ArgvHelper args1{"angel_lsp", "--disable-hover=true", "--disable-document-symbols=true", "--disable-workspace-symbols=true", "--disable-inlay-hints=true", "--disable-formatting=true"};
            ServerConfig config1 = FromArgs(args1.argc(), args1.data());
            CHECK(config1.features.enableHover == false);
            CHECK(config1.features.enableDocumentSymbols == false);
            CHECK(config1.features.enableWorkspaceSymbols == false);
            CHECK(config1.features.enableInlayHints == false);
            CHECK(config1.features.enableFormatting == false);

            ArgvHelper args2{"angel_lsp", "--disable-hover=false", "--disable-document-symbols=false", "--disable-workspace-symbols=false", "--disable-inlay-hints=false", "--disable-formatting=false"};
            ServerConfig config2 = FromArgs(args2.argc(), args2.data());
            CHECK(config2.features.enableHover == true);
            CHECK(config2.features.enableDocumentSymbols == true);
            CHECK(config2.features.enableWorkspaceSymbols == true);
            CHECK(config2.features.enableInlayHints == true);
            CHECK(config2.features.enableFormatting == true);

            ArgvHelper args3{"angel_lsp", "--disable-completion=1", "--disable-references=1", "--disable-rename=1", "--disable-document-highlight=1", "--disable-folding-range=1", "--disable-code-action=1"};
            ServerConfig config3 = FromArgs(args3.argc(), args3.data());
            CHECK(config3.features.enableCompletion == false);
            CHECK(config3.features.enableReferences == false);
            CHECK(config3.features.enableRename == false);
            CHECK(config3.features.enableDocumentHighlight == false);
            CHECK(config3.features.enableFoldingRange == false);
            CHECK(config3.features.enableCodeAction == false);

            ArgvHelper args4{"angel_lsp", "--disable-completion=0", "--disable-references=0", "--disable-rename=0", "--disable-document-highlight=0", "--disable-folding-range=0", "--disable-code-action=0"};
            ServerConfig config4 = FromArgs(args4.argc(), args4.data());
            CHECK(config4.features.enableCompletion == true);
            CHECK(config4.features.enableReferences == true);
            CHECK(config4.features.enableRename == true);
            CHECK(config4.features.enableDocumentHighlight == true);
            CHECK(config4.features.enableFoldingRange == true);
            CHECK(config4.features.enableCodeAction == true);
        }
    }

    TEST_CASE("Standalone --enable-* flags (without explicit value)")
    {
        ArgvHelper args{"angel_lsp",
                         "--disable-hover",
                         "--enable-hover",
                         "--disable-completion",
                         "--enable-completion",
                         "--disable-document-symbols",
                         "--enable-document-symbols",
                         "--disable-workspace-symbols",
                         "--enable-workspace-symbols",
                         "--disable-references",
                         "--enable-references",
                         "--disable-rename",
                         "--enable-rename",
                         "--disable-document-highlight",
                         "--enable-document-highlight",
                         "--disable-folding-range",
                         "--enable-folding-range",
                         "--disable-inlay-hints",
                         "--enable-inlay-hints",
                         "--disable-code-action",
                         "--enable-code-action",
                         "--disable-formatting",
                         "--enable-formatting"};

        ServerConfig config = FromArgs(args.argc(), args.data());
        CHECK(config.features.enableHover == true);
        CHECK(config.features.enableCompletion == true);
        CHECK(config.features.enableDocumentSymbols == true);
        CHECK(config.features.enableWorkspaceSymbols == true);
        CHECK(config.features.enableReferences == true);
        CHECK(config.features.enableRename == true);
        CHECK(config.features.enableDocumentHighlight == true);
        CHECK(config.features.enableFoldingRange == true);
        CHECK(config.features.enableInlayHints == true);
        CHECK(config.features.enableCodeAction == true);
        CHECK(config.features.enableFormatting == true);
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
        ArgvHelper args1{"angel_lsp",
                         "--enable-hover=FALSE",
                         "--enable-definition=False",
                         "--enable-completion=OFF",
                         "--enable-semantic-tokens=no",
                         "--enable-document-symbols=FALSE",
                         "--enable-workspace-symbols=Off",
                         "--enable-references=no",
                         "--enable-rename=0",
                         "--enable-document-highlight=FALSE",
                         "--enable-folding-range=False",
                         "--enable-inlay-hints=OFF",
                         "--enable-code-action=no",
                         "--enable-formatting=0"};
        ServerConfig config1 = FromArgs(args1.argc(), args1.data());
        CHECK(config1.features.enableHover == false);
        CHECK(config1.features.enableDefinition == false);
        CHECK(config1.features.enableCompletion == false);
        CHECK(config1.features.enableSemanticTokens == false);
        CHECK(config1.features.enableDocumentSymbols == false);
        CHECK(config1.features.enableWorkspaceSymbols == false);
        CHECK(config1.features.enableReferences == false);
        CHECK(config1.features.enableRename == false);
        CHECK(config1.features.enableDocumentHighlight == false);
        CHECK(config1.features.enableFoldingRange == false);
        CHECK(config1.features.enableInlayHints == false);
        CHECK(config1.features.enableCodeAction == false);
        CHECK(config1.features.enableFormatting == false);

        ArgvHelper args2{"angel_lsp",
                         "--enable-hover=TRUE",
                         "--enable-definition=True",
                         "--enable-completion=ON",
                         "--enable-semantic-tokens=yes",
                         "--enable-document-symbols=TRUE",
                         "--enable-workspace-symbols=On",
                         "--enable-references=yes",
                         "--enable-rename=1",
                         "--enable-document-highlight=TRUE",
                         "--enable-folding-range=True",
                         "--enable-inlay-hints=ON",
                         "--enable-code-action=yes",
                         "--enable-formatting=1"};
        ServerConfig config2 = FromArgs(args2.argc(), args2.data());
        CHECK(config2.features.enableHover == true);
        CHECK(config2.features.enableDefinition == true);
        CHECK(config2.features.enableCompletion == true);
        CHECK(config2.features.enableSemanticTokens == true);
        CHECK(config2.features.enableDocumentSymbols == true);
        CHECK(config2.features.enableWorkspaceSymbols == true);
        CHECK(config2.features.enableReferences == true);
        CHECK(config2.features.enableRename == true);
        CHECK(config2.features.enableDocumentHighlight == true);
        CHECK(config2.features.enableFoldingRange == true);
        CHECK(config2.features.enableInlayHints == true);
        CHECK(config2.features.enableCodeAction == true);
        CHECK(config2.features.enableFormatting == true);
    }

    TEST_CASE("Complex combinations of multiple flags")
    {
        ArgvHelper args{"angel_lsp",
                         "--disable-hover",
                         "--enable-definition=true",
                         "--enable-completion", "false",
                         "--enable-semantic-tokens=1",
                         "--enable-signature-help", "0",
                         "--disable-document-symbols",
                         "--enable-workspace-symbols=false",
                         "--enable-references=true",
                         "--disable-rename=false",
                         "--enable-document-highlight=false",
                         "--disable-folding-range",
                         "--enable-inlay-hints=1",
                         "--disable-code-action",
                         "--enable-formatting=true",
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
        CHECK(config.features.enableDocumentSymbols == false);
        CHECK(config.features.enableWorkspaceSymbols == false);
        CHECK(config.features.enableReferences == true);
        CHECK(config.features.enableRename == true);
        CHECK(config.features.enableDocumentHighlight == false);
        CHECK(config.features.enableFoldingRange == false);
        CHECK(config.features.enableInlayHints == true);
        CHECK(config.features.enableCodeAction == false);
        CHECK(config.features.enableFormatting == true);
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
                             "--disable-predefinedloader",
                             "--disable-documentsymbols",
                             "--disable-workspacesymbols",
                             "--disable-documenthighlight",
                             "--disable-foldingrange",
                             "--disable-inlayhints",
                             "--disable-codeaction"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableSemanticTokens == false);
            CHECK(config.features.enableSignatureHelp == false);
            CHECK(config.features.enablePredefinedLoader == false);
            CHECK(config.features.enableDocumentSymbols == false);
            CHECK(config.features.enableWorkspaceSymbols == false);
            CHECK(config.features.enableDocumentHighlight == false);
            CHECK(config.features.enableFoldingRange == false);
            CHECK(config.features.enableInlayHints == false);
            CHECK(config.features.enableCodeAction == false);

            ArgvHelper args2{"angel_lsp",
                              "--enable-semantictokens=1",
                              "--enable-signaturehelp=1",
                              "--enable-predefinedloader=1",
                              "--enable-documentsymbols=1",
                              "--enable-workspacesymbols=1",
                              "--enable-documenthighlight=1",
                              "--enable-foldingrange=1",
                              "--enable-inlayhints=1",
                              "--enable-codeaction=1"};
            ServerConfig config2 = FromArgs(args2.argc(), args2.data());
            CHECK(config2.features.enableSemanticTokens == true);
            CHECK(config2.features.enableSignatureHelp == true);
            CHECK(config2.features.enablePredefinedLoader == true);
            CHECK(config2.features.enableDocumentSymbols == true);
            CHECK(config2.features.enableWorkspaceSymbols == true);
            CHECK(config2.features.enableDocumentHighlight == true);
            CHECK(config2.features.enableFoldingRange == true);
            CHECK(config2.features.enableInlayHints == true);
            CHECK(config2.features.enableCodeAction == true);
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

            ArgvHelper args3{"angel_lsp",
                              "--enable-references=false",
                              "--enable-references=true",
                              "--disable-references=true"};
            ServerConfig config3 = FromArgs(args3.argc(), args3.data());
            CHECK(config3.features.enableReferences == false);

            ArgvHelper args4{"angel_lsp",
                              "--disable-rename",
                              "--enable-rename=true"};
            ServerConfig config4 = FromArgs(args4.argc(), args4.data());
            CHECK(config4.features.enableRename == true);

            ArgvHelper args5{"angel_lsp",
                              "--disable-formatting",
                              "--enable-formatting=true",
                              "--disable-formatting"};
            ServerConfig config5 = FromArgs(args5.argc(), args5.data());
            CHECK(config5.features.enableFormatting == false);

            ArgvHelper args6{"angel_lsp",
                              "--disable-inlay-hints",
                              "--enable-inlay-hints"};
            ServerConfig config6 = FromArgs(args6.argc(), args6.data());
            CHECK(config6.features.enableInlayHints == true);
        }

        SUBCASE("Search directories arguments")
        {
            ArgvHelper args{"angel_lsp",
                            "--search-dir=E:/Include/Path1",
                            "--search-directory=E:/Include/Path2",
                            "--search-path=E:/Include/Path3"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            REQUIRE(config.searchDirectories.size() == 3);
            CHECK(config.searchDirectories[0] == "E:/Include/Path1");
            CHECK(config.searchDirectories[1] == "E:/Include/Path2");
            CHECK(config.searchDirectories[2] == "E:/Include/Path3");
        }

        SUBCASE("Array-like template arguments")
        {
            // A host that registered its own element-wise list factory. This cannot be read from a
            // predefined stub - the stub format has no notation for a list factory, so `array<T>`
            // and `optional<T>` are declared identically - which is why it is asked for here.
            ArgvHelper args{"angel_lsp",
                            "--array-like-type=vector",
                            "--array-like-template=ring"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.types.arrayLikeTemplates.contains("vector"));
            CHECK(config.types.arrayLikeTemplates.contains("ring"));

            // The engine's default array type is always one of them, listed or not.
            const auto names = config.types.ArrayLikeTemplateNames();
            CHECK(names.contains("array"));
            CHECK(names.size() == 3);
        }

        SUBCASE("No array-like templates configured leaves only the default array type")
        {
            ArgvHelper args{"angel_lsp"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.types.arrayLikeTemplates.empty());

            const auto names = config.types.ArrayLikeTemplateNames();
            REQUIRE(names.size() == 1);
            CHECK(names.contains("array"));
        }
    }

    TEST_CASE("Type conversion checks flag")
    {
        SUBCASE("Enabled by default")
        {
            ArgvHelper args{"angel_lsp"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableTypeConversionChecks == true);
        }

        SUBCASE("Disabled via --disable-type-conversion-checks")
        {
            ArgvHelper args{"angel_lsp", "--disable-type-conversion-checks"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableTypeConversionChecks == false);
        }

        SUBCASE("Disabled via --enable-type-conversion-checks=false")
        {
            ArgvHelper args{"angel_lsp", "--enable-type-conversion-checks=false"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableTypeConversionChecks == false);
        }

        SUBCASE("Re-enabled explicitly")
        {
            ArgvHelper args{"angel_lsp", "--enable-type-conversion-checks=true"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableTypeConversionChecks == true);
        }
    }

    TEST_CASE("Implementation and selection range flags")
    {
        SUBCASE("Both enabled by default")
        {
            ServerConfig config = FromArgs(0, nullptr);
            CHECK(config.features.enableImplementation == true);
            CHECK(config.features.enableSelectionRange == true);
        }

        SUBCASE("Each disables independently")
        {
            ArgvHelper args{"angel_lsp", "--disable-implementation"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableImplementation == false);
            CHECK(config.features.enableSelectionRange == true);
        }

        SUBCASE("Disabled via --enable-selection-range=false")
        {
            ArgvHelper args{"angel_lsp", "--enable-selection-range=false"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableSelectionRange == false);
        }
    }

    TEST_CASE("Hierarchy flags")
    {
        SUBCASE("Both enabled by default")
        {
            ServerConfig config = FromArgs(0, nullptr);
            CHECK(config.features.enableCallHierarchy == true);
            CHECK(config.features.enableTypeHierarchy == true);
        }

        SUBCASE("Each disables independently")
        {
            ArgvHelper args{"angel_lsp", "--disable-call-hierarchy"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableCallHierarchy == false);
            CHECK(config.features.enableTypeHierarchy == true);
        }

        SUBCASE("Disabled via --enable-type-hierarchy=false")
        {
            ArgvHelper args{"angel_lsp", "--enable-type-hierarchy=false"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableTypeHierarchy == false);
        }
    }

    TEST_CASE("Linked editing flag")
    {
        SUBCASE("Enabled by default")
        {
            ServerConfig config = FromArgs(0, nullptr);
            CHECK(config.features.enableLinkedEditing == true);
        }

        SUBCASE("Disabled via --disable-linked-editing")
        {
            ArgvHelper args{"angel_lsp", "--disable-linked-editing"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.features.enableLinkedEditing == false);
        }
    }

    TEST_CASE("Predefined file paths")
    {
        SUBCASE("Empty by default")
        {
            ArgvHelper args{"angel_lsp"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.predefinedFiles.empty());
        }

        SUBCASE("Repeatable and order-preserving")
        {
            ArgvHelper args{"angel_lsp",
                            "--predefined-file=C:/Games/svencoop/as.predefined",
                            "--predefined-path=./stubs/engine.as.predefined"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            REQUIRE(config.predefinedFiles.size() == 2);
            CHECK(config.predefinedFiles[0] == "C:/Games/svencoop/as.predefined");
            CHECK(config.predefinedFiles[1] == "./stubs/engine.as.predefined");
        }

        SUBCASE("Independent of the predefined extension")
        {
            // These two used to be conflated: a configured path was passed as the suffix, which
            // matched no file at all and silently disabled stub loading.
            ArgvHelper args{"angel_lsp",
                            "--predefined-file=C:/Games/as.predefined",
                            "--predefined-ext=.stub"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            REQUIRE(config.predefinedFiles.size() == 1);
            CHECK(config.predefinedFiles[0] == "C:/Games/as.predefined");
            CHECK(config.info.predefinedFileExtension == ".stub");
        }

        SUBCASE("Empty value is ignored")
        {
            ArgvHelper args{"angel_lsp", "--predefined-file="};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.predefinedFiles.empty());
        }
    }

    TEST_CASE("Diagnostic severity overrides")
    {
        SUBCASE("Empty by default")
        {
            ArgvHelper args{"angel_lsp"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.diagnosticSeverities.empty());
        }

        SUBCASE("Repeatable, one entry per code")
        {
            ArgvHelper args{"angel_lsp",
                            "--diagnostic-severity=as-warn-unused-variable=hint",
                            "--diagnostic-severity=as-err-no-implicit-conversion=warning"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            REQUIRE(config.diagnosticSeverities.size() == 2);
            CHECK(config.diagnosticSeverities.at("as-warn-unused-variable") == "hint");
            CHECK(config.diagnosticSeverities.at("as-err-no-implicit-conversion") == "warning");
        }

        SUBCASE("Severity name is case-insensitive")
        {
            ArgvHelper args{"angel_lsp", "--diagnostic-severity=as-warn-unused-variable=HINT"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            REQUIRE(config.diagnosticSeverities.size() == 1);
            CHECK(config.diagnosticSeverities.at("as-warn-unused-variable") == "hint");
        }

        SUBCASE("Unknown severity is dropped rather than guessed at")
        {
            ArgvHelper args{"angel_lsp", "--diagnostic-severity=as-warn-unused-variable=loud"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.diagnosticSeverities.empty());
        }

        SUBCASE("Malformed pairs are ignored without disturbing valid ones")
        {
            ArgvHelper args{"angel_lsp",
                            "--diagnostic-severity=no-separator",
                            "--diagnostic-severity==hint",
                            "--diagnostic-severity=as-warn-unused-variable=",
                            "--diagnostic-severity=as-warn-unused-variable=error"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            REQUIRE(config.diagnosticSeverities.size() == 1);
            CHECK(config.diagnosticSeverities.at("as-warn-unused-variable") == "error");
        }
    }

    TEST_CASE("Engine properties")
    {
        SUBCASE("Default to the engine's own defaults")
        {
            ServerConfig config = FromArgs(0, nullptr);
            CHECK(config.engine.allowUnsafeReferences == false);
            CHECK(config.engine.privatePropAsProtected == false);
            CHECK(config.engine.disallowGlobalVars == false);
        }

        SUBCASE("Each known property is settable")
        {
            ArgvHelper args{"angel_lsp",
                            "--engine-property=allowUnsafeReferences=true",
                            "--engine-property=privatePropAsProtected=true",
                            "--engine-property=disallowGlobalVars=true"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.engine.allowUnsafeReferences == true);
            CHECK(config.engine.privatePropAsProtected == true);
            CHECK(config.engine.disallowGlobalVars == true);
        }

        SUBCASE("A bare name means on, the way --enable-x does")
        {
            ArgvHelper args{"angel_lsp", "--engine-property=allowUnsafeReferences"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.engine.allowUnsafeReferences == true);
        }

        SUBCASE("false is honoured, and so is the --engine-prop spelling")
        {
            ArgvHelper args{"angel_lsp",
                            "--engine-property=allowUnsafeReferences=true",
                            "--engine-prop=allowUnsafeReferences=false"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.engine.allowUnsafeReferences == false);
        }

        SUBCASE("An unknown name changes nothing")
        {
            // AngelScript has forty engine properties and this server reads three. Naming one of
            // the other thirty-seven has to be inert rather than land on a neighbouring field.
            ArgvHelper args{"angel_lsp",
                            "--engine-property=allowMultilineStrings=true",
                            "--engine-property=allowUnsafeReferences=true"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.engine.allowUnsafeReferences == true);
            CHECK(config.engine.privatePropAsProtected == false);
            CHECK(config.engine.disallowGlobalVars == false);
        }

        SUBCASE("A non-boolean value is dropped rather than guessed at")
        {
            ArgvHelper args{"angel_lsp", "--engine-property=allowUnsafeReferences=maybe"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.engine.allowUnsafeReferences == false);
        }

        SUBCASE("reportUnknownTypes is on, and can be turned off")
        {
            // On because an unreported unknown type surfaces as silence at every call site, not as
            // one missing diagnostic. Measured on the 1061-file corpus with the Sven Co-op profile
            // loaded: 1581 findings off, 3850 on. See DiagnosticsConfig::reportUnknownTypes.
            ArgvHelper plain{"angel_lsp"};
            CHECK(FromArgs(plain.argc(), plain.data()).diagnostics.reportUnknownTypes == true);

            ArgvHelper off{"angel_lsp", "--no-report-unknown-types"};
            CHECK(FromArgs(off.argc(), off.data()).diagnostics.reportUnknownTypes == false);

            ArgvHelper backOn{"angel_lsp", "--no-report-unknown-types", "--report-unknown-types"};
            CHECK(FromArgs(backOn.argc(), backOn.data()).diagnostics.reportUnknownTypes == true);
        }

        SUBCASE("propertyAccessorMode is an integer, not a bool")
        {
            // asEP_PROPERTY_ACCESSOR_MODE is the one property here whose value is not true/false,
            // which is why ApplyEngineProperty takes the raw text rather than a parsed bool.
            ArgvHelper args{"angel_lsp", "--engine-property=propertyAccessorMode=3"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.engine.propertyAccessorMode == 3);
        }

        SUBCASE("propertyAccessorMode defaults to 2, not to the engine's 3")
        {
            // Deliberate: mode 3 reports `c.V` when the accessor lacks the `property` keyword, and
            // defaulting to it would hand that error to every workspace whose host sets 2 - for
            // code that compiles for them. See EngineProperties::propertyAccessorMode.
            ArgvHelper args{"angel_lsp"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.engine.propertyAccessorMode == 2);
        }

        SUBCASE("The app-registered-only mode is a mode like any other")
        {
            // This subcase used to assert the opposite, on a comment saying 0 (disabled) and 1
            // (app-registered accessors only) do not change what a script declaration means. They
            // do. Measured: under both, `c.X` backed by a script `get_X`/`set_X` is rejected, with
            // the `property` keyword and without it, because the compiler skips script functions
            // outright - as_compiler.cpp:14003 and :14077, the second commented "Ignore script
            // functions, if the application has disabled script defined property accessors".
            //
            // While they were dropped here, a host running either had no way to state its
            // configuration at all, so the last value it set was silently replaced by another.
            ArgvHelper args{"angel_lsp",
                            "--engine-property=propertyAccessorMode=3",
                            "--engine-property=propertyAccessorMode=1"};
            ServerConfig config = FromArgs(args.argc(), args.data());
            CHECK(config.engine.propertyAccessorMode == 1);
        }
    }
}

TEST_CASE("ServerConfig - Every property accessor mode the engine has can be stated")
{
    // 0 and 1 used to be rejected, on a comment claiming neither changed what a script declaration
    // means. Measured false: under both, `c.X` backed by a script `get_X` is rejected with the
    // `property` keyword and without it, because the compiler skips script functions entirely. A
    // host running either had no way to say so.
    for (const char *mode : { "0", "1", "2", "3" })
    {
        const std::string flag = std::string("--engine-property=propertyAccessorMode=") + mode;
        ArgvHelper args{ "angel_lsp", flag.c_str() };
        const ServerConfig config = FromArgs(args.argc(), args.data());

        INFO("mode " << mode);
        CHECK(config.engine.propertyAccessorMode == mode[0] - '0');
    }
}

TEST_CASE("ServerConfig - A property accessor mode the engine does not have is dropped")
{
    // Left at the default rather than guessed at: a typo that silently picked a dialect would be
    // far harder to notice than one that changes nothing.
    ArgvHelper args{ "angel_lsp", "--engine-property=propertyAccessorMode=7" };
    const ServerConfig config = FromArgs(args.argc(), args.data());

    CHECK(config.engine.propertyAccessorMode == 2);
}

TEST_CASE("ServerConfig - Preprocessor features are off unless asked for")
{
    {
        ArgvHelper args{ "angel_lsp" };
        const ServerConfig config = FromArgs(args.argc(), args.data());

        CHECK_FALSE(config.preprocessor.elseSupport);
        CHECK_FALSE(config.preprocessor.elifSupport);
        CHECK_FALSE(config.preprocessor.ifdefSupport);
        CHECK_FALSE(config.preprocessor.defineInScripts);
        CHECK(config.pragmaMode == ServerConfig::PragmaMode::Accept);
    }

    {
        ArgvHelper args{ "angel_lsp",
                         "--preprocessor-feature=elseSupport=true",
                         "--preprocessor-feature=pragmaMode=error" };
        const ServerConfig config = FromArgs(args.argc(), args.data());

        CHECK(config.preprocessor.elseSupport);
        CHECK_FALSE(config.preprocessor.elifSupport);
        CHECK(config.pragmaMode == ServerConfig::PragmaMode::Error);
    }
}

TEST_CASE("ServerConfig - Defined words arrive from --define")
{
    ArgvHelper args{ "angel_lsp", "--define=SERVER", "--define=DEBUG" };
    const ServerConfig config = FromArgs(args.argc(), args.data());

    REQUIRE(config.definedWords.size() == 2);
    CHECK(config.definedWords[0] == "SERVER");
    CHECK(config.definedWords[1] == "DEBUG");
}

// Directing the server to a specific active predefined stub isolates the target host environment,
// ensuring engine symbols resolve against this exact file rather than an arbitrary stub.
TEST_CASE("ServerConfig - Active predefined file path is set via --predefined-active")
{
    ArgvHelper args{ "angel_lsp", "--predefined-active=C:/hosts/engine.as.predefined" };
    const ServerConfig config = FromArgs(args.argc(), args.data());

    CHECK(config.activePredefined == "C:/hosts/engine.as.predefined");
}

// Backward compatibility depends on this remaining empty by default; any non-empty default
// would restrict stub loading and break existing workspaces that rely on scanning and merging all stubs.
TEST_CASE("ServerConfig - Active predefined file is empty by default")
{
    ArgvHelper args{ "angel_lsp" };
    const ServerConfig config = FromArgs(args.argc(), args.data());

    CHECK(config.activePredefined.empty());
}

// Unlike cumulative stub lists (--predefined-file), the active stub designates a single chosen host profile;
// multiple occurrences must overwrite the previous value without accumulating into predefinedFiles.
TEST_CASE("ServerConfig - Last --predefined-active wins and does not accumulate into predefinedFiles")
{
    ArgvHelper args{ "angel_lsp",
                     "--predefined-active=C:/hosts/first.as.predefined",
                     "--predefined-active=C:/hosts/second.as.predefined" };
    const ServerConfig config = FromArgs(args.argc(), args.data());

    CHECK(config.activePredefined == "C:/hosts/second.as.predefined");
    CHECK(config.predefinedFiles.empty());
}

// Build tools and wrapper scripts may pass an empty flag value when unconfigured; treating it as a no-op
// preserves default stub scanning behavior instead of registering an invalid path.
TEST_CASE("ServerConfig - Empty --predefined-active value is ignored")
{
    ArgvHelper args{ "angel_lsp", "--predefined-active=" };
    const ServerConfig config = FromArgs(args.argc(), args.data());

    CHECK(config.activePredefined.empty());
}
