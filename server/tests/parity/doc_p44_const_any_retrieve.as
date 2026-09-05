// `retrieve` on a const `any`.
//
//     angelscript_oracle doc_p44_const_any_retrieve.as   accepted
//
// Written because `angelscript_oracle --dump-registry` and tests/fixtures/sdk-addons.as.predefined
// disagree about it: the engine registers `bool retrieve(?&out) const` and the hand-written stub
// declares it without the `const`, for all three overloads. A stub that forgets `const` makes the
// analyzer refuse a call the engine accepts, which is the one failure this project treats as fatal
// - so the disagreement is worth a guard whichever way it resolves.
void test(const any@ box)
{
    int value = 0;
    box.retrieve(value);

    double asDouble = 0;
    box.retrieve(asDouble);
}
