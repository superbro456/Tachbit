# Делает src/runtime_text.c из src/runtime.h: библиотеку C как одну строку,
# которую компилятор кладёт в начало каждой программы.
# Запуск из корня репозитория:  python3 tools/embed_runtime.py
import os

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
src = open(os.path.join(root, "src", "runtime.h"), encoding="utf-8").read()

lines = ["// Сделано tools/embed_runtime.py из src/runtime.h. Не правь руками.",
         "char *runtime_text ="]
for line in src.splitlines():
    escaped = line.replace("\\", "\\\\").replace('"', '\\"')
    lines.append('    "' + escaped + '\\n"')
lines.append("    ;")
open(os.path.join(root, "src", "runtime_text.c"), "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("готово: src/runtime_text.c")
