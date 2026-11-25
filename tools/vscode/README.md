# Tachbit for VS Code

Support for the [Tachbit](https://github.com/superbro456/Tachbit) language (`.tb` files).

- syntax colors and two themes: **Tachbit Dark** and **Tachbit Light**
- run the current file with **▶** above the code, **F5**, **Cmd/Ctrl+Enter** or the green Run button (also on the Touch Bar)
- errors are underlined right in the code when you save
- language helper: completion (variables, functions, obj fields after `.`, module functions after `ui.`, built-in words),
  hover with signature and description, go to definition (**F12**), parameter hints, file outline,
  all references (**Shift+F12**), rename (**F2**)
- snippets for `func`, `obj`, `each`, `if` and more
- commands: *Tachbit: run file*, *Tachbit: check file*, *Tachbit: show C code*

## Needs

The `tb` command. Install Tachbit first:

```
curl -fsSL https://raw.githubusercontent.com/superbro456/Tachbit/main/tools/get.sh | sh
```

If `tb` is somewhere else, set **tachbit.path** in Settings.

## Other editors (LSP)

The same helper is a language server: `tb-lsp` (installed by `tools/install.sh`, needs Node.js),
or `node tools/vscode/lsp/server.js`. It speaks LSP over stdin/stdout.

- **Neovim**: `vim.lsp.start({ name = 'tachbit', cmd = { 'tb-lsp' }, filetypes = { 'tachbit' } })`
- **Helix** (`languages.toml`): `[language-server.tachbit] command = "tb-lsp"`
- **Zed / Sublime LSP**: command `tb-lsp`, language `tachbit`, files `*.tb`

## Settings

| Setting | Default | What |
|---|---|---|
| `tachbit.path` | `~/.tachbit/bin/tb` | where the `tb` command is |
| `tachbit.checkOnSave` | `true` | check the file on save and underline errors |

MIT license.
