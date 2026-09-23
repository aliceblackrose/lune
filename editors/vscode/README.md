# Lune editor support

The `editors/vscode` directory is a dependency-free VS Code language extension for Lune 0.1. It provides:

- `.lune` file recognition;
- TextMate syntax highlighting for comments, strings, numbers, keywords, operators, and call sites;
- line-comment support;
- bracket matching and automatic closing;
- brace-based indentation.

## Install locally

From the repository root, copy or symlink `editors/vscode` into your VS Code extensions directory, then restart VS Code.

Typical locations:

- Linux: `~/.vscode/extensions/lune-language-0.1.0`
- macOS: `~/.vscode/extensions/lune-language-0.1.0`
- Windows: `%USERPROFILE%\.vscode\extensions\lune-language-0.1.0`

No Node.js build step is required because the extension contains only declarative language assets.

The grammar is intentionally aligned with the Lune 0.1 language contract. When syntax changes, update both `docs/language.md` and `syntaxes/lune.tmLanguage.json` in the same change.
