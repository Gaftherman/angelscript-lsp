// TYPE-06. "No matching signatures to 'SetMode(int)' ... Rejected due to type mismatch on
// parameter 'm'". OverloadResolver already refuses this; TypeConversionChecker allowed it.
enum Mode { A, B }
void SetMode(Mode m) {}
void main() { int id = 1; SetMode(id); }
