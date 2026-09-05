# Where these came from

Forty small scripts probing the shapes a condition can take: dangling and malformed `else`,
`else if` chains, `try`/`catch`, every clause of a `for`, `while` and `do`/`while`, and `switch`.
Each file states what it probes and records the real compiler's verdict.

They are exploratory rather than wired into a test. Two of them paid for the whole set by
contradicting the prediction written above them, and both became rules:

- `ce_01` - `if (c);` is an **error**, "If with empty statement". AngelScript is stricter here than
  C++ and stricter than its own loops: `while (c);`, `for (;;);` and `do; while (c);` are all
  accepted, and so is `if (c) {}`. Only the bare `;` after `if` or `else` is refused.
- `ce_39` - a `default` label in the middle of a `switch` is an **error**, "The default case must be
  the last one".

Two more were already gaps and are now closed: a `string` as a `for` condition, and a class with no
conversion operator as a condition. The parity corpus carries the regression guards for all four.
