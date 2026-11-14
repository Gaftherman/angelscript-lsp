// @expect: accept
funcdef void Act();
void main() { Act@ a = function() {}; a(); }
