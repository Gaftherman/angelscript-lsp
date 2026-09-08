// The two places an anonymous function is legal, both measured clean by angelscript_oracle
// (exit 0, no diagnostics): as an argument where a funcdef is expected, and as the initialiser of a
// funcdef handle. The rejected stand-alone form is pinned in doc_r92.

funcdef void AnonFnParityCallback(bool);

void AnonFnParityTake(AnonFnParityCallback@ cb) {}

void AnonFnParityInPosition()
{
    AnonFnParityTake(function(bool param) { });

    AnonFnParityCallback@ handle = function(bool param) { };
}
