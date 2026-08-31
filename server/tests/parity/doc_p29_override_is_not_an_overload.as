// A method redeclared in a subclass OVERRIDES the base's; it is not a second candidate competing
// with it. Every call below is ACCEPTED:
//
//     angelscript_oracle doc_p29_override_is_not_an_overload.as
//
// A member lookup walks the hierarchy and finds the base's declaration alongside the derived one.
// Treating both as candidates made them score identically and reported "Multiple matching
// signatures" - 75 findings over the corpus, all on legal code, from one JSON library that declares
// `class json : meta_api::json::v2::json` and restates its methods as an interface summary. Two
// details made it hard to see: the two declarations have genuinely different qualified names, so
// the duplicate-declaration guard did not apply; and the tie also handed the definite-assignment
// checker an arbitrary overload to read `&out` from, which reported the line that initialises a
// variable as reading it uninitialised.
//
// The boundary is doc_r28: two declarations across a hierarchy whose PARAMETER LISTS differ really
// are two candidates, and an argument matching both really is ambiguous.

class Base
{
    bool Get(int &out value) { value = 1; return true; }
    bool Get(const string &in key, int &out value) { value = 2; return true; }
    void Touch(int v) {}
}

class Derived : Base
{
    // Same parameter lists, so each of these replaces the base's rather than joining it.
    bool Get(int &out value) override { value = 3; return true; }
    bool Get(const string &in key, int &out value) override { value = 4; return true; }

    // A genuinely new overload, which does join them.
    bool Get(const string &in key) { return true; }
}

class Deeper : Derived
{
    bool Get(int &out value) override { value = 5; return true; }
}

void CallsThem()
{
    Derived d;
    Deeper deeper;

    int value;
    d.Get(value);
    d.Get("key", value);
    d.Get("key");

    // Three levels of the same override, still one method.
    int fromDeeper;
    deeper.Get(fromDeeper);

    // And the out-parameter counts as initialising its argument, whichever declaration in the
    // chain the resolver settles on.
    Touch(value);
    Touch(fromDeeper);
}

void Touch(int v) {}
