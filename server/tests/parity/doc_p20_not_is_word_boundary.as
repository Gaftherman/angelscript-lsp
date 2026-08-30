// `!is` is AngelScript's handle-inequality operator and it is spelled with letters, so it is the
// one operator that can swallow the front of an identifier. Both lines below compile, and they
// have to be told apart by the character after `!is`: a word boundary makes it the operator, a
// letter makes it the first three characters of a call.
//
// The formatter's tokenizer matched it greedily and rewrote `!isdigit(s)` into `!is digit(s)`,
// which is a different program and does not compile. Twenty-eight of the 1061 scripts in the
// corpus were being corrupted this way on every format - see tests/FormatterCorpusTest.cpp, which
// checks the token stream survives formatting for every script here and in that corpus.
class C {}

bool isdigit(const string &in s)
{
    return s.length() > 0;
}

void main()
{
    C@ a = C();
    C@ b = C();

    if (a !is b)
    {
        return;
    }

    if (!isdigit("5"))
    {
        return;
    }
}
