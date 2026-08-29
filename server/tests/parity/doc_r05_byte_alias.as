// TYPE-05 refuted at its root: "Identifier 'byte' is not a data type". `byte` is host-registered,
// not built in. The real defect it points at is narrower - a typedef is unwrapped only at the outer
// name, never inside template arguments. See docs/PARITY-BACKLOG.md.
void P(array<uint8>@ b) {}
void main() { array<byte> b; P(b); }
