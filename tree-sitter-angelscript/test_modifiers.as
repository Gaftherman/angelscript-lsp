// AST: &, out, inout, in in params — what does tree-sitter capture for param_type children?
void A(int &out x) {}
void B(int &inout x) {}
void C(int &in x) {}
void D(int & x) {}
void E(const int x) {}
void F(const int& x) {}
