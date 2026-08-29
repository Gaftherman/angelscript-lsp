// PARSER-02 refuted for the plain-quote form: "Multiline strings are not allowed in this
// application" - an engine property that is off by default. The three hand-rolled scanners in the
// server that end a "..." at the newline match the default engine. Heredoc: doc_p12.
void main() { string s = "Linea 1
Linea 2"; }
