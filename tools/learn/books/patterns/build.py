import html, re, os, sys
sys.path.insert(0, os.path.dirname(__file__))
from days import D

HERE = os.path.dirname(os.path.abspath(__file__))
KW = ["func", "gives", "return", "if", "else", "each", "in", "count", "from", "to", "repeat", "while",
      "times", "stop", "skip", "obj", "enum", "match", "use", "modpub", "try", "or", "fail", "and", "not",
      "true", "false", "list", "dict", "channel", "background", "shared", "lock", "temp", "danger", "move"]
TYPES = ["int", "dec", "text", "bool"]


def hl(code):
    out = []
    for line in code.rstrip("\n").split("\n"):
        com = ""
        i = line.find("//")
        if i >= 0:
            line, com = line[:i], line[i:]
        parts = re.split(r'("[^"]*")', line)
        buf = ""
        for p in parts:
            if p.startswith('"') and p.endswith('"') and len(p) >= 2:
                buf += '<span class="s">' + html.escape(p) + "</span>"
            else:
                e = html.escape(p)
                e = re.sub(r"\b(" + "|".join(KW) + r")\b", r'<span class="k">\1</span>', e)
                e = re.sub(r"\b(" + "|".join(TYPES) + r")\b", r'<span class="t">\1</span>', e)
                e = re.sub(r"(?<![\w.])(\d+(?:\.\d+)?)\b", r'<span class="n">\1</span>', e)
                buf += e
        if com:
            buf += '<span class="c">' + html.escape(com) + "</span>"
        out.append(buf)
    return "\n".join(out)


WEEKS = [
    (1, 7, "Неделя 1. Основа логики", "Состояние, решения, циклы, модели, функции, ошибки. Без этого не работает ни одна программа."),
    (8, 14, "Неделя 2. Как устроены приложения", "Справочники, сохранение, слои, цикл событий, модель и вид, автоматы, отмена."),
    (15, 22, "Неделя 3. Как устроены игры", "Игровой цикл, физика, столкновения, сущности, экраны, плавность, события, ИИ."),
    (23, 30, "Неделя 4. Как устроена система", "Службы, память, параллельность, очереди, буферы, планировщик, права, общий скелет."),
]

CSS = r"""
@page { size: A4; margin: 12mm 14mm 13mm 14mm; }
:root { --ink:#1d2333; --mute:#5d6680; --line:#e3e6ef; --soft:#f5f6fb;
  --app:#2f7de1; --game:#e8892b; --os:#7b5ce0; --acc:#14a39a; }
* { box-sizing: border-box; }
html { -webkit-print-color-adjust: exact; print-color-adjust: exact; }
body { margin:0; font-family: Inter, "DejaVu Sans", sans-serif; color:var(--ink); font-size:10pt; line-height:1.42; background:#fff; }
h1,h2,h3 { font-family: "Inter Display", Inter, sans-serif; line-height:1.2; margin:0; }
code, pre { font-family: "JetBrains Mono", "DejaVu Sans Mono", monospace; }
code { background:var(--soft); border-radius:4px; padding:0 4px; font-size:9.4pt; }
p { margin: 0 0 8px; }
.page { page-break-after: always; }
.cover { height: 270mm; display:flex; flex-direction:column; justify-content:space-between;
  background: radial-gradient(120% 80% at 80% 10%, #e9e4ff 0%, #f4f8ff 45%, #ffffff 75%); border-radius:18px; padding:22mm 16mm; }
.cover .tag { font-size:10pt; letter-spacing:.12em; text-transform:uppercase; color:var(--os); font-weight:600; }
.cover h1 { font-size:34pt; margin-top:10mm; font-weight:700; }
.cover .sub { font-size:14pt; color:var(--mute); margin-top:6mm; max-width:140mm; }
.trio { display:flex; gap:8px; margin-top:12mm; }
.trio div { flex:1; border-radius:14px; padding:12px 14px; color:#fff; font-weight:600; font-size:12pt; }
.trio .a { background:var(--app); } .trio .g { background:var(--game); } .trio .o { background:var(--os); }
.trio small { display:block; font-weight:400; font-size:9pt; opacity:.9; margin-top:4px; }
.cover .foot { color:var(--mute); font-size:9.5pt; }
h2.sec { font-size:20pt; margin: 0 0 6mm; }
h3 { font-size:12.5pt; margin: 6mm 0 3mm; }
table { width:100%; border-collapse:collapse; font-size:9pt; margin: 2mm 0 4mm; }
th, td { text-align:left; vertical-align:top; padding:4px 7px; border-bottom:1px solid var(--line); }
th { color:var(--mute); font-weight:600; font-size:9pt; }
.ok { color:#178a4c; font-weight:600; } .part { color:#c07a12; font-weight:600; } .no { color:#c2413b; font-weight:600; }
.box { background:var(--soft); border-radius:12px; padding:10px 14px; margin: 3mm 0; }
.verdict { border-left:4px solid var(--acc); }
.loop { display:flex; align-items:center; gap:6px; margin: 3mm 0 4mm; flex-wrap:wrap; }
.loop span { background:#fff; border:1.5px solid var(--line); border-radius:999px; padding:5px 12px; font-weight:600; font-size:9.5pt; }
.loop i { color:var(--mute); font-style:normal; }
.worlds { display:flex; gap:8px; margin: 3mm 0; }
.worlds > div { flex:1; border-radius:12px; padding:10px 12px; border:1.5px solid var(--line); font-size:9.4pt; }
.worlds b { display:block; margin-bottom:4px; font-size:10.5pt; }
.worlds .a b { color:var(--app);} .worlds .g b { color:var(--game);} .worlds .o b { color:var(--os);}
.day { page-break-before: always; }
.dayhead { display:flex; align-items:baseline; gap:10px; border-bottom:2px solid var(--ink); padding-bottom:3mm; margin-bottom:4mm; }
.dayhead .num { font-size:26pt; font-weight:700; color:var(--os); font-family:"Inter Display",Inter,sans-serif; }
.dayhead h2 { font-size:17pt; }
.dayhead .wk { margin-left:auto; color:var(--mute); font-size:9pt; white-space:nowrap; }
.goal { font-weight:600; margin-bottom:3mm; }
.where { display:flex; gap:6px; margin: 3mm 0; }
.where > div { flex:1; border-radius:10px; padding:7px 10px; font-size:9.2pt; line-height:1.4; }
.where .a { background:#eaf2fd; } .where .g { background:#fdf1e4; } .where .o { background:#f0ebfd; }
.where b { display:block; font-size:8.6pt; text-transform:uppercase; letter-spacing:.06em; margin-bottom:2px; }
.where .a b { color:var(--app);} .where .g b { color:var(--game);} .where .o b { color:var(--os);}
.pattern { border:1.5px solid var(--acc); border-radius:10px; padding:7px 12px; margin:3mm 0; font-size:9.8pt; }
.pattern b { color:var(--acc); }
pre.code { background:#161a28; color:#e6e9f2; border-radius:10px; padding:10px 12px; font-size:8.1pt; line-height:1.36; white-space:pre-wrap; margin: 2mm 0; page-break-inside: avoid; }
pre.code .k { color:#c4a3ff; } pre.code .t { color:#7ad3f5; } pre.code .s { color:#9fdf8f; } pre.code .n { color:#ffc27a; } pre.code .c { color:#7f88a3; font-style:italic; }
.lbl { font-size:8.6pt; color:var(--mute); text-transform:uppercase; letter-spacing:.06em; margin-top:3mm; }
pre.out { background:var(--soft); border-radius:8px; padding:6px 12px; font-size:8.1pt; margin:1mm 0 2mm; white-space:pre-wrap; page-break-inside: avoid; }
.task { background:#fff7e0; border-radius:10px; padding:8px 12px; margin-top:3mm; font-size:9.8pt; page-break-inside: avoid; }
.task b { color:#a06a00; }
.cheat td { padding:3px 7px; font-size:8.6pt; }
.cheat td:first-child { font-weight:600; white-space:nowrap; color:var(--os); }
.cheat td:nth-child(2) { font-weight:600; }
.weeks td:first-child { white-space:nowrap; font-weight:600; }
"""


def readiness():
    rows = [
        ("Приложения с окнами", "ok", "Готово",
         "Окна (и несколько окон сразу), кнопки, поля ввода, списки, меню, вкладки, перетаскивание (<code>lib/ui.tb</code>), файлы, JSON, HTTP, дата, звук, буфер обмена, чёткий шрифт на Retina. Пример — файловый менеджер <code>examples/apps/files</code>.",
         "Готовых библиотек пока мало."),
        ("2D-игры", "ok", "Готово",
         "Игровой цикл с <code>frame_ms()</code>, клавиатура, мышь и геймпад, рисование, звук и свой микшер, плавность (<code>lib/anim.tb</code>), физика (<code>lib/physics.tb</code>).",
         "Музыка в OGG пока не читается, только WAV."),
        ("3D-игры", "part", "Средние",
         "Сетки, камера, поворот во все стороны, солнце, лампы, туман, тени, стекло, кости, готовые модели OBJ/glTF (<code>lib/model.tb</code>), свои шейдеры на Tachbit (<code>gpu func</code>), большие миры (LOD). Всё считает видеокарта.",
         "Редактора сцен нет: мир собирается кодом. Часть шейдеров на Mac ещё не проверена."),
        ("Своя ОС (Omnium)", "part", "Работает в эмуляторе",
         "64-битное ядро, несколько ядер процессора, клетки и каналы, память с квотами, диски SATA и NVMe, файловые системы OFS3 и exFAT, экран 1920×1080, мышь, оболочка «Капля», картинки JPEG/GIF/BMP.",
         "Проверено только в QEMU, на настоящем компьютере не запускалось. Сети пока нет."),
        ("Системные программы на Linux/Mac", "ok", "Готово",
         "Ручная память (<code>danger</code>), раскладка байтов (<code>shape</code>), C-библиотеки (<code>use c</code>), системные вызовы, атомарные счётчики, сборка своей библиотеки для C.",
         "Нужна аккуратность: в <code>danger</code> компилятор помогает меньше."),
    ]
    tr = "".join(
        f'<tr><td><b>{a}</b></td><td class="{c}">{s}</td><td>{h}</td><td>{n}</td></tr>' for a, c, s, h, n in rows)
    return f"""
<div class="page">
<h2 class="sec">Честно: готов ли Tachbit</h2>
<p>Состояние на 25 ноября 2026 года (Tachbit 1.3).</p>
<div class="box verdict"><p style="margin:0"><b>Коротко:</b> учиться и делать свои приложения, 2D-игры и простые 3D-игры уже можно. Свою ОС можно собирать и запускать в эмуляторе. 3D-игры со светом, тенями и готовыми моделями тоже можно. Ставить Omnium на ноутбук пока рано: ОС не проверена на живом железе.</p></div>
<table><tr><th style="width:20%">Что</th><th style="width:13%">Статус</th><th>Что уже есть</th><th style="width:28%">Чего пока нет</th></tr>{tr}</table>
<h3>Почему курс про логику, а не про синтаксис</h3>
<p>Синтаксис Tachbit можно выучить за пару недель по <code>docs/guide.md</code>. Труднее понять, <i>как думает</i> программа: где она хранит данные, кто их меняет, в каком порядке всё происходит. Эти приёмы называют <b>паттернами</b>. Они одинаковы в любом языке, поэтому всё, что вы здесь выучите, пригодится и в других языках.</p>
<p>Каждый день разбирает один паттерн и показывает, где он живёт в приложении, в игре и в операционной системе. Код в курсе проверен компилятором Tachbit, а вывод под кодом получен настоящим запуском.</p>
</div>"""


def bigpicture():
    wk = "".join(f"<tr><td>Дни {a}–{b}</td><td><b>{t.split('. ',1)[1]}</b><br>{d}</td></tr>" for a, b, t, d in WEEKS)
    return f"""
<div class="page">
<h2 class="sec">Большая картина</h2>
<p>Приложение, игра и ОС выглядят совсем разными, но внутри у них один скелет. Есть <b>состояние</b> (что программа помнит), <b>события</b> (что его меняет), <b>цикл</b> (кто ждёт события) и <b>слои</b> (кто за что отвечает). Разница только в том, как крутится цикл.</p>
<div class="worlds">
 <div class="a"><b>Приложение</b>Спит, пока ничего не происходит. Клик или сообщение будит его, оно меняет данные, перерисовывает окно и снова засыпает.</div>
 <div class="g"><b>Игра</b>Не спит никогда. 60 раз в секунду: сколько прошло времени, что нажато, сдвинуть мир, нарисовать кадр заново.</div>
 <div class="o"><b>Операционная система</b>Служит другим программам: делит между ними процессор, память, диск и экран, следит, чтобы никто не мешал другому.</div>
</div>
<p style="margin-top:4mm"><b>Цикл приложения</b></p>
<div class="loop"><span>ждать событие</span><i>→</i><span>разобрать</span><i>→</i><span>изменить состояние</span><i>→</i><span>перерисовать</span><i>↺</i></div>
<p><b>Цикл игры</b></p>
<div class="loop"><span>dt</span><i>→</i><span>ввод</span><i>→</i><span>обновить мир</span><i>→</i><span>нарисовать кадр</span><i>↺ 60 раз в секунду</i></div>
<p><b>Цикл ядра ОС</b></p>
<div class="loop"><span>прерывание</span><i>→</i><span>кто это: таймер, клавиша, диск</span><i>→</i><span>разбудить нужную службу</span><i>→</i><span>выбрать следующую задачу</span><i>↺</i></div>
<h3>План на 30 дней</h3>
<table class="weeks">{wk}</table>
<h3>Как заниматься</h3>
<p>Около часа в день. Прочитайте объяснение и наберите код руками: не копируйте его, так логика запоминается лучше. Запустите <code>tb run dayNN.tb</code> и сравните вывод с тем, что в книге. Потом сделайте задание. Если задание не получилось, это нормально: вернитесь к нему через день.</p>
<p>Готовые файлы примеров лежат рядом с этой книгой в архиве <code>tachbit-kurs-kod.zip</code>. Курс разделён на две книги: в первой части дни 1–14 (логика и приложения), во второй дни 15–30 (игры и система).</p>
</div>"""


def week_of(n):
    for a, b, t, _ in WEEKS:
        if a <= n <= b:
            return t.split(".")[0]


def day(n):
    d = D[n]
    code = open(os.path.join(HERE, "code", f"day{n:02d}.tb")).read()
    paras = "".join(f"<p>{p}</p>" for p in d["text"])
    if d.get("window"):
        out = '<div class="lbl">Что будет</div><pre class="out">Откроется окно. Запускайте на своём компьютере, где есть экран.</pre>'
    else:
        o = open(os.path.join(HERE, "out", f"day{n:02d}.txt")).read().rstrip()
        lbl = "Вывод"
        if d.get("run_in"):
            lbl = f"Вывод (ввели: {html.escape(d['run_in'])})"
        if n == 23:
            o = re.sub(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d", "2026-11-22 12:30:00", o)
        out = f'<div class="lbl">{lbl}</div><pre class="out">{html.escape(o)}</pre>'
    pn, pr = d["pattern"]
    return f"""
<div class="day">
<div class="dayhead"><div class="num">{n}</div><h2>{d['title']}</h2><div class="wk">{week_of(n)}</div></div>
<div class="goal">Цель: {d['goal']}</div>
{paras}
<div class="where">
 <div class="a"><b>Приложение</b>{d['app']}</div>
 <div class="g"><b>Игра</b>{d['game']}</div>
 <div class="o"><b>ОС</b>{d['os']}</div>
</div>
<div class="pattern"><b>Паттерн «{pn}».</b> {pr}</div>
<div class="lbl">Код: day{n:02d}.tb</div>
<pre class="code">{hl(code)}</pre>
{out}
<div class="task"><b>Задание.</b> {d['task']}</div>
</div>"""


def cheat(a=1, b=30, more=True):
    rows = "".join(f"<tr><td>{n}</td><td>{D[n]['pattern'][0]}</td><td>{D[n]['pattern'][1]}</td></tr>" for n in range(a, b + 1))
    return f"""
<div class="day">
<h2 class="sec">Шпаргалка: паттерны {a}–{b}</h2>
<table class="cheat"><tr><th>День</th><th>Паттерн</th><th>Правило</th></tr>{rows}</table>
</div>
""" + (MORE if more else "")


MORE = """
<div class="day">
<h2 class="sec">Что дальше</h2>
<p>Теперь читайте настоящий код и ищите в нём знакомые паттерны.</p>
<table>
<tr><th style="width:38%">Файл</th><th>Что там искать</th></tr>
<tr><td><code>examples/apps/files</code></td><td>Модель и вид, слои, цикл событий окна, справочники.</td></tr>
<tr><td><code>examples/apps/football/main.tb</code></td><td>Игровой цикл, физика, столкновения, автомат вратаря, простой ИИ бота.</td></tr>
<tr><td><code>examples/apps/playground/main.tb</code></td><td>Сущности, плавность из <code>lib/anim.tb</code>, камера, которая догоняет героя.</td></tr>
<tr><td><code>lib/ui.tb</code></td><td>Как устроена библиотека кнопок: состояние фокуса, события мыши, раскладка.</td></tr>
<tr><td><code>omnium/clock.tb</code>, <code>keyboard.tb</code></td><td>Службы с каналами: как часы и клавиатура отвечают другим клеткам.</td></tr>
<tr><td><code>omnium/ofs.tb</code></td><td>Загрузить, изменить, сохранить; журнал изменений на диске.</td></tr>
<tr><td><code>omnium/shell.tb</code>, <code>desktop.tb</code></td><td>Оболочка «Капля»: окна, пересечения, плавность, события.</td></tr>
</table>
<div class="box verdict"><p style="margin:0">Главная мысль курса: перед кодом ответьте на четыре вопроса. Что программа помнит? Какие события её меняют? Какой у неё цикл? На какие слои она делится? Если ответы есть, код пишется почти сам.</p></div>
</div>"""


def cover(part, sub):
    return f"""
<div class="page"><div class="cover">
<div>
<div class="tag">Tachbit · курс на 30 дней · часть {part}</div>
<h1>Как думает программа</h1>
<div class="sub">{sub}</div>
<div class="trio">
 <div class="a">Приложение<small>ждёт события</small></div>
 <div class="g">Игра<small>живёт 60 кадров в секунду</small></div>
 <div class="o">ОС<small>служит всем остальным</small></div>
</div>
</div>
<div class="foot">Версия от 25 ноября 2026 · Код проверен компилятором Tachbit 1.3</div>
</div></div>"""



import sys as _s
part = int(_s.argv[1])
if part == 1:
    body = cover(1, "Часть 1. Дни 1–14: основа логики и как устроены приложения. 14 паттернов, по одному в день, с проверенным кодом на Tachbit.") + readiness() + bigpicture() + "".join(day(n) for n in range(1, 15)) + cheat(1, 14, False)
else:
    body = cover(2, "Часть 2. Дни 15–30: как устроены игры и операционная система. 16 паттернов, по одному в день, с проверенным кодом на Tachbit.") + bigpicture() + "".join(day(n) for n in range(15, 31)) + cheat(15, 30, True)
title = f"Как думает программа. Часть {part}"
doc = f"<!doctype html><html lang='ru'><head><meta charset='utf-8'><title>{title}</title><style>{CSS}</style></head><body>{body}</body></html>"
open(os.path.join(HERE, f"kurs{part}.html"), "w").write(doc)
print("ok", part)
