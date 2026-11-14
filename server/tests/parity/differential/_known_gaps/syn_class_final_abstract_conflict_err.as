// Known gap: the compiler rejects this script; the server stays silent. Oracle: [(2, 7, 'A class cannot be both abstract and final')]
// @expect: reject
final abstract class Bad {}
