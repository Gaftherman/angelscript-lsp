// PARSER-02, the half of it that is real. A triple-quoted string spans lines; a plain "..." does
// not - see doc_r03.
void main() { string q = """
   SELECT id
"""; print(q); }
