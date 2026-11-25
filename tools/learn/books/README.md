# Четыре книги Tachbit (PDF)

1. «Язык Tachbit» — `lang/build.py` (уроки из `../l1.py`, `../l2.py`, вывод примеров из `../lang_out/`).
2–3. «Как думает программа», часть 1 (дни 1–14) и часть 2 (дни 15–30) — `patterns/build.py 1` и `patterns/build.py 2` (тексты `../days.py`, код дней `../days_code/`).
4. «Справочник Tachbit» — `ref/data.py` (22 раздела), `ref/runall.py` запускает примеры, `ref/gen.py` делает HTML.

HTML → PDF: `node pdf.js файл.html файл.pdf` (Playwright). Пути к рабочим папкам внутри скриптов
указывают на папку, где книги собирались; поправьте их перед сборкой.
Готовые PDF лежат в `docs/learn/files/`.
