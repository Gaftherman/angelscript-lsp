#include <iostream>
#include <string>
#include <tree_sitter/api.h>
extern "C" const TSLanguage *tree_sitter_angelscript();
extern void printTree(TSNode node, int depth, const char* field);

int main() {
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_angelscript());
    
    std::string source1 = "void Main() { Engine::Math::Lerp(0.0f, 10.0f, 0.5f); }";
    TSTree *tree1 = ts_parser_parse_string(parser, NULL, source1.c_str(), source1.length());
    
    std::string source2 = "void Main() { Engine::Math::Vector3 pos2; }";
    TSTree *tree2 = ts_parser_parse_string(parser, NULL, source2.c_str(), source2.length());
    
    // I can't easily link printTree, I'll just use ts_node_string
    char *s1 = ts_node_string(ts_tree_root_node(tree1));
    std::cout << "Lerp:\n" << s1 << "\n\n";
    free(s1);
    
    char *s2 = ts_node_string(ts_tree_root_node(tree2));
    std::cout << "Vector3:\n" << s2 << "\n\n";
    free(s2);
    
    ts_tree_delete(tree1);
    ts_tree_delete(tree2);
    ts_parser_delete(parser);
    return 0;
}
