# Делает archive/compiler-c/runtime_text.c из archive/compiler-c/runtime.h: библиотеку C как одну строку,
# которую компилятор кладёт в начало каждой программы.
# Запуск из корня репозитория:  python3 tools/embed_runtime.py
import os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(root, "archive", "compiler-c", "runtime.h"), encoding="utf-8").read()

lines = ["// Сделано tools/embed_runtime.py из archive/compiler-c/runtime.h. Не правь руками.",
         "char *runtime_text ="]
for line in src.splitlines():
    escaped = line.replace("\\", "\\\\").replace('"', '\\"')
    lines.append('    "' + escaped + '\\n"')
lines.append("    ;")
open(os.path.join(root, "archive", "compiler-c", "runtime_text.c"), "w", encoding="utf-8").write("\n".join(lines) + "\n")

# то же для компилятора на Tachbit: функция runtime_text() в compiler/runtime_text.tb
# (у него своя библиотека compiler/runtime.h: с 0.7 в ней есть то, чего нет у стартера)
src = open(os.path.join(root, "compiler", "runtime.h"), encoding="utf-8").read()
tb = ["// Сделано tools/embed_runtime.py из compiler/runtime.h. Не правь руками.",
      "modpub func runtime_text()",
      "    lines = list text"]
for line in src.splitlines():
    escaped = line.replace("\\", "\\\\").replace('"', '\\"')
    tb.append('    lines.add("' + escaped + '\\n")')
tb.append('    return join(lines, "")')
# и маленький компоновщик ядер (tachbit bare без ld.lld): функция linker_text()
src = open(os.path.join(root, "tools", "elf_link.py"), encoding="utf-8").read()
tb += ["", "modpub func linker_text()", "    lines = list text"]
for line in src.splitlines():
    escaped = line.replace("\\", "\\\\").replace('"', '\\"')
    tb.append('    lines.add("' + escaped + '\\n")')
tb.append('    return join(lines, "")')
open(os.path.join(root, "compiler", "runtime_text.tb"), "w", encoding="utf-8").write("\n".join(tb) + "\n")
print("готово: archive/compiler-c/runtime_text.c и compiler/runtime_text.tb")
