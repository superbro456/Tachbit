# Tachbit for VS Code

Support for the [Tachbit](https://github.com/superbro456/Tachbit) language (`.tb` files).

- syntax colors and two themes: **Tachbit Dark** and **Tachbit Light**
- run the current file with **▶** above the code, **F5**, **Cmd/Ctrl+Enter** or the green Run button (also on the Touch Bar)
- errors are underlined right in the code when you save
- snippets for `func`, `obj`, `each`, `if` and more
- commands: *Tachbit: run file*, *Tachbit: check file*, *Tachbit: show C code*

## Needs

The `tb` command. Install Tachbit first:

```
curl -fsSL https://raw.githubusercontent.com/superbro456/Tachbit/main/tools/get.sh | sh
```

If `tb` is somewhere else, set **tachbit.path** in Settings.

## Settings

| Setting | Default | What |
|---|---|---|
| `tachbit.path` | `~/.tachbit/bin/tb` | where the `tb` command is |
| `tachbit.checkOnSave` | `true` | check the file on save and underline errors |

MIT license.
