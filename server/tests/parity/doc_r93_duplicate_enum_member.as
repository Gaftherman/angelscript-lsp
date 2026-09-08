// Two enumerators with the same name in one enum.
//
// angelscript_oracle:
//   ERROR (4, 5): Name conflict. 'Member1' is already used.
//
// The value being identical is beside the point - it is the name that conflicts.

enum DuplicateEnumMemberProbe
{
    ParityDupMember = 0,
    ParityDupMember = 0
}
