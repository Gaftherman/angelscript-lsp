#include <doctest/doctest.h>

#include "utils/Utils.h"

using namespace angel_lsp::utils;

// =====================================================================================
// IsPredefinedFile - the real matching logic Server::ReadWorkspaceFiles uses to decide
// whether a workspace file gets loaded as an engine-registration stub (see
// SemanticAnalyzerTest.cpp's "predefined-style file" test for what happens once one is).
// =====================================================================================

TEST_CASE("IsPredefinedFile - file ending with the configured extension matches")
{
    CHECK(IsPredefinedFile("file:///project/stubs.as.predefined", ".as.predefined"));
}

TEST_CASE("IsPredefinedFile - an ordinary script file does not match")
{
    CHECK_FALSE(IsPredefinedFile("file:///project/script.as", ".as.predefined"));
}

TEST_CASE("IsPredefinedFile - the extension must be a true suffix, not just a substring")
{
    CHECK_FALSE(IsPredefinedFile("file:///project/foo.as.predefined.txt", ".as.predefined"));
}

TEST_CASE("IsPredefinedFile - an empty extension never matches")
{
    CHECK_FALSE(IsPredefinedFile("file:///project/stubs.as.predefined", ""));
}

// =====================================================================================
// IsPrimitiveType - validates AngelScript built-in primitive type names
// =====================================================================================

TEST_CASE("IsPrimitiveType - standard integer types match")
{
    CHECK(IsPrimitiveType("int"));
    CHECK(IsPrimitiveType("int8"));
    CHECK(IsPrimitiveType("int16"));
    CHECK(IsPrimitiveType("int32"));
    CHECK(IsPrimitiveType("int64"));
    CHECK(IsPrimitiveType("uint"));
    CHECK(IsPrimitiveType("uint8"));
    CHECK(IsPrimitiveType("uint16"));
    CHECK(IsPrimitiveType("uint32"));
    CHECK(IsPrimitiveType("uint64"));
}

TEST_CASE("IsPrimitiveType - floating point, boolean, and void types match")
{
    CHECK(IsPrimitiveType("float"));
    CHECK(IsPrimitiveType("double"));
    CHECK(IsPrimitiveType("bool"));
    CHECK(IsPrimitiveType("void"));
}

TEST_CASE("IsPrimitiveType - non-primitive types and user types do not match")
{
    CHECK_FALSE(IsPrimitiveType("string"));
    CHECK_FALSE(IsPrimitiveType("array"));
    CHECK_FALSE(IsPrimitiveType("dictionary"));
    CHECK_FALSE(IsPrimitiveType("Vector3"));
    CHECK_FALSE(IsPrimitiveType("int128"));
    CHECK_FALSE(IsPrimitiveType("uint128"));
    CHECK_FALSE(IsPrimitiveType("Int"));
    CHECK_FALSE(IsPrimitiveType("BOOL"));
    CHECK_FALSE(IsPrimitiveType(""));
    CHECK_FALSE(IsPrimitiveType("int32_t"));
}

// =====================================================================================
// Document - validates Layer 1 Document struct
// =====================================================================================

#include "document/Document.h"

TEST_CASE("Document - struct initialization and fields")
{
    angel_lsp::document::Document doc{"file:///test.as", "void main() {}", 1, nullptr};
    CHECK(doc.uri == "file:///test.as");
    CHECK(doc.text == "void main() {}");
    CHECK(doc.version == 1);
    CHECK(doc.tree == nullptr);
}


