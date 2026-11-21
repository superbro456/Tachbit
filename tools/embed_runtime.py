# Делает compiler/runtime_text.tb из compiler/runtime.h: библиотеку C как одну строку,
# которую компилятор кладёт в начало каждой программы.
# Запуск из корня репозитория:  python3 tools/embed_runtime.py
import os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# то же для компилятора на Tachbit: функция runtime_text() в compiler/runtime_text.tb
# (у него своя библиотека compiler/runtime.h: с 0.7 в ней есть то, чего нет у стартера)
src = open(os.path.join(root, "compiler", "runtime.h"), encoding="utf-8").read()
# куски ядра Омниума лежат в omnium/kernel: строка «//@include путь» заменяется файлом
def expand(text):
    out = []
    for line in text.splitlines():
        if line.startswith("//@include "):
            out.append(open(os.path.join(root, line[len("//@include "):].strip()), encoding="utf-8").read().rstrip("\n"))
        else:
            out.append(line)
    return "\n".join(out) + "\n"
src = expand(src)
tb = ["// Сделано tools/embed_runtime.py из compiler/runtime.h. Не правь руками.",
      "modpub func runtime_text()",
      "    lines = list text"]
for line in src.splitlines():
    escaped = line.replace("\\", "\\\\").replace('"', '\\"')
    tb.append('    lines.add("' + escaped + '\\n")')
tb.append('    return join(lines, "")')
# и маленький компоновщик ядер (tachbit bare без ld.lld): функция linker_text()
src = open(os.path.join(root, "omnium", "kernel", "elf_link.py"), encoding="utf-8").read()
tb += ["", "modpub func linker_text()", "    lines = list text"]
for line in src.splitlines():
    escaped = line.replace("\\", "\\\\").replace('"', '\\"')
    tb.append('    lines.add("' + escaped + '\\n")')
tb.append('    return join(lines, "")')
open(os.path.join(root, "compiler", "runtime_text.tb"), "w", encoding="utf-8").write("\n".join(tb) + "\n")
print("готово: compiler/runtime_text.tb")
