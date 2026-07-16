#include <iostream>
#include <string>
#include <tree_sitter/api.h>
extern "C" const TSLanguage *tree_sitter_angelscript();
int main() {
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_angelscript());
    std::string source = "void Main() { Collider::Collider(); }";
    TSTree *tree = ts_parser_parse_string(parser, NULL, source.c_str(), source.length());
    TSNode root_node = ts_tree_root_node(tree);
    char *string = ts_node_string(root_node);
    std::cout << string << std::endl;
    return 0;
}
