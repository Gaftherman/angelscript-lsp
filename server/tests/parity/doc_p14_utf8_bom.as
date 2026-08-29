// PARSER-04, and it already works. A leading EF BB BF is tolerated by the compiler, and
// tree-sitter tolerates it too: the grammar's extras is /\s+/, and in JavaScript regex \s matches
// U+FEFF, so the BOM is consumed as whitespace before any rule sees it. The document lists this as
// a defect; it is not one here. The only residue is that line 0 starts three bytes in.
class MyEntity { int id; }
