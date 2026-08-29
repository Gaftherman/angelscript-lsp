// PARSER-09. CScriptBuilder strips a metadata block and hands the declaration to the compiler, so
// this compiles. The grammar has no rule for it, so each block turns its declaration into an ERROR
// node and the symbol disappears from the index.
[Property, Category="Weapons"]
int m_Health = 100;

[Category("Armas")]
enum WeaponType { Pistol, Rifle }
