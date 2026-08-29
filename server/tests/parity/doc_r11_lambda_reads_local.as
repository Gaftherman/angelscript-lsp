// LAMBDA-01, the half that is an error: "No matching symbol 'local'". Contrast doc_p02, where the
// same lambda reads a global and compiles.
funcdef void CB();
void Init() { int local = 1; CB@ cb = function() { local = 2; }; }
