# Syntax fixtures

These files define parser-facing examples for the Lune v0 language contract.

- `valid/` files must parse successfully.
- `invalid/` files must fail during lexing or parsing.

The fixtures avoid semantic-only errors such as assigning an unknown name or redeclaring a binding in the same scope. Milestone 1 should wire every fixture into the bootstrap parser test runner and assert deterministic diagnostics for invalid cases.
