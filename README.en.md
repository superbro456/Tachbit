<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="assets/tachbit-logo-dark.svg">
    <img src="assets/tachbit-logo-light.svg" alt="Tachbit" width="320">
  </picture>
</p>

# Tachbit 1.1

**Write complex programs in a clear language and get a real, fast program, not a prototype.**

Code reads almost like Python and runs like C. The compiler does the hard part (memory, cores, safety checks) and explains mistakes in plain words before the program runs. Tachbit compiles to C, then `cc` (clang or gcc) makes a native program. The compiler is written in Tachbit itself and builds itself.

[Русская версия (полная документация)](README.md)

Compiler messages and error texts are in Russian for now; the keywords are English.

## Install

You need `cc` (on a Mac: `xcode-select --install`) and `git`.

```
git clone https://github.com/superbro456/Tachbit
cd Tachbit
./tools/install.sh          # builds the compiler and puts the `tb` command on your PATH
./tools/install_vscode.sh   # optional: VS Code colors, run on F5, errors in the editor
```

```
tb run examples/hello.tb    # build and run
tb build file.tb            # make a program next to the file
tb check file.tb            # only look for errors
tb new shop && tb start shop
```

## A taste

```
obj Item
    name: text
    price: int

enum Status
    new
    paid

func total(items: list Item) gives r: int
    s = 0
    each it in items
        s += it.price
    return s

func main()
    items = [Item(name: "tea", price: 120), Item(name: "coffee", price: 200)]
    print("total:", total(items))
    st = Status.paid
    match st
        new
            print("new order")
        paid
            print("paid")
```

Programs start at `func main()`. Blocks are indentation, like Python. Types are inferred (`x = 5`), or written (`total = int 0`).

## What is in the language

| Area | Words |
|---|---|
| basics | `int`, `dec`, `bool`, `text`, `list T`, `dict K V`, `obj`, `enum`, `const`, `link` (reference to a place) |
| control | `if` / `else`, `each x in xs`, `count i from 1 to 10`, `repeat N times`, `repeat while`, `skip`, `stop`, `match` |
| functions | `func f(a: int) gives r: int`, named arguments, several outputs (`q, r = divide(a: 17, b: 5)`) |
| errors | `fail "text"`, `try f()`, `f() or fallback`, `f() else err` — a call that can fail cannot be ignored |
| modules | `use money`, `modpub`, `modpri` |
| speed | `on all cores`, `shared`, `by vec` (SIMD), `obj ... by columns`, `temp`, `each piece in data by N`, `together` |
| threads | `background f(...)` + `.wait`, `channel T` (`.send`, `.receive`, `.close`, `each x in ch`), `move`, `lock` / `unlock` |
| no OS | `tb bare file.tb` makes a bootable kernel; `danger` blocks for memory, ports, `asm`, interrupts |
| library | text (`split`, `join`, `find`, `replace`, ...), files and folders, `input`, `args`, `sort`, math, `random`, `time_ms`, `date_now`, `unix_time`, `http_get`, `json_get`, `json_text`, windows and drawing for games |

## Safety

Checked before the program runs:

- out-of-range index, division by zero, empty `.pop()` and a closed empty channel stop the program with a clear message instead of corrupting memory;
- deep recursion reports a stack overflow (in every thread);
- inside `on all cores`, data created outside the loop is read-only — and the compiler follows that data through other names (`row = matrix[i]`), function results, and into functions that modify their parameters;
- a `shared` variable changed by a thread must be changed under `lock`; a lock inside the same lock and two locks taken in opposite orders are compile errors;
- a `link` to a list element blocks growing that list while the link is alive;
- `background` takes only plain values, channels, or a list handed over with `move` (no second name, not stored anywhere, not used afterwards);
- removing keys from a dict while `each` walks it is an error.

Not caught yet (honest list): integer overflow wraps silently; the same object placed twice in a list under `each ... on all cores`; interrupt handlers vs. main code in the no-OS mode; nothing at all inside `danger` (by design). Memory is managed for you: loop steps, functions and `temp` free their memory right away, and since 1.1 a garbage collector frees long-lived objects, lists, dicts and texts nobody uses any more (`TB_GC_STATS=1` prints each collection).

## Speed

On the authors' Mac (Intel i9) Tachbit is as fast as plain C or faster on all 18 tasks in `bench/` (fib is a tie). Where Tachbit wins thanks to a special word (`on all cores`, `temp`, `by vec`, ...), `bench/*_smart.c` gives C the same trick, and the README table shows both. Run `./bench/run.sh` to measure on your machine.

## Layout

| Folder | What |
|---|---|
| `compiler/` | the compiler, written in Tachbit (`lexer`, `parser`, `check`, `emit`, `runtime.h`) |
| `compiler/seed/tb.c` | the compiler's own C output, used to build the first `tb` on a new machine |
| `tests/` | `./tests/run_tests.sh` and `./tests/run_bare.sh` |
| `examples/`, `apps/` | examples, a step-by-step tutorial (`examples/learn`), a file manager, a game, a kernel |
| `editor/vscode/` | VS Code extension |
| `bench/` | benchmarks against C and Python |

## License

MIT — see [LICENSE](LICENSE).
