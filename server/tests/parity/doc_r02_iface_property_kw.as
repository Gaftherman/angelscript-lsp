// PROP-05 refuted. `property` on an interface method is a parse error:
//   ERROR (1, 44): Expected ';' / Instead found identifier 'property'
// with or without `const`. as-err-interface-method-attribute is correct as written.
interface IEntity { int get_Health() const property; }
