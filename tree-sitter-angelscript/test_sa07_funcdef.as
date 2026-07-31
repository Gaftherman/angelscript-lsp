// Test array of unknown type — should we flag the inner element type?
UnknownType[] arr;

// Test funcdef return type unresolved — SA-07 gap in ValidateFuncdef?
funcdef UnknownReturn BadCallback(int x);

// Test handle of unknown — Player@ p — TypeKind::Handle → NOT Unknown → NOT flagged
// This is intentional: handles to unknown types are valid for forward declarations
Player@ p_handle;
