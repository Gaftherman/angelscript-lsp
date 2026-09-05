// The other direction: a dictionary whose value is an array.
//
//     angelscript_oracle doc_p42_dictionary_holding_an_array.as   accepted
//
// A dictionary value is untyped at the language level, so this must never be reported however the
// analyzer models the container.
void test()
{
    dictionary d;

    array<int> nums = {1, 2, 3};
    d.set("nums", nums);

    array<int> back;
    d.get("nums", back);
}
