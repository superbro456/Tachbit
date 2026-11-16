# Делает compiler-c/runtime_text.c из compiler-c/runtime.h: библиотеку C как одну строку,
# которую компилятор кладёт в начало каждой программы.
# Запуск из корня репозитория:  python3 tools/embed_runtime.py
import os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(root, "compiler-c", "runtime.h"), encoding="utf-8").read()

lines = ["// Сделано tools/embed_runtime.py из compiler-c/runtime.h. Не правь руками.",
         "char *runtime_text ="]
for line in src.splitlines():
    escaped = line.replace("\\", "\\\\").replace('"', '\\"')
    lines.append('    "' + escaped + '\\n"')
lines.append("    ;")
open(os.path.join(root, "compiler-c", "runtime_text.c"), "w", encoding="utf-8").write("\n".join(lines) + "\n")

# то же для компилятора на Tachbit: функция runtime_text() в compiler-tb/runtime_text.tb
tb = ["// Сделано tools/embed_runtime.py из compiler-c/runtime.h. Не правь руками.",
      "modpub func runtime_text()",
      "    lines = list text"]
for line in src.splitlines():
    escaped = line.replace("\\", "\\\\").replace('"', '\\"')
    tb.append('    lines.add("' + escaped + '\\n")')
tb.append('    return join(lines, "")')
open(os.path.join(root, "compiler-tb", "runtime_text.tb"), "w", encoding="utf-8").write("\n".join(tb) + "\n")
print("готово: compiler-c/runtime_text.c и compiler-tb/runtime_text.tb")
