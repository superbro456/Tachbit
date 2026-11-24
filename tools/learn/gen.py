"""Собирает раздел «Уроки» сайта: docs/learn/.

Данные:
  l1.py, l2.py   — книга «Язык Tachbit» (28 уроков), вывод примеров в lang_out/ (делает run.py)
  days.py        — курс «Как думает программа» (30 паттернов), код в days_code/, вывод в days_out/
  refdata.py     — указатели: слова, встроенные функции, сообщения компилятора

Запуск: python3 tools/learn/run.py (проверить примеры), затем python3 tools/learn/gen.py
"""
import html, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from l1 import L as L1
from l2 import L as L2
from days import D
from refdata import KEYWORDS, BUILTINS, MESSAGES

L = L1 + L2
DOCS = os.path.join(HERE, "..", "..", "docs", "learn")

KW = ["func", "gives", "return", "if", "else", "each", "in", "count", "from", "to", "repeat", "while",
      "times", "stop", "skip", "obj", "enum", "match", "use", "modpub", "modpri", "try", "or", "fail", "and", "not",
      "true", "false", "list", "dict", "channel", "background", "shared", "lock", "unlock", "temp", "danger", "move",
      "link", "const", "on", "all", "cores", "by", "vec", "columns", "together", "shape", "at", "bits", "bytes", "big",
      "timer", "keyboard", "err", "for", "c"]
TYPES = ["int", "dec", "text", "bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32"]

PARTS = [
    (1, 12, "Основа языка", "Значения, решения, циклы, текст, списки, словари, функции, свои типы, ошибки."),
    (13, 17, "Настоящие программы", "Модули, числа и время, файлы, сеть, память."),
    (18, 20, "Несколько ядер и скорость", "Фон, замки, каналы, все ядра, SIMD."),
    (21, 24, "Окна, игры, звук", "2D, 3D, звук, кнопки и поля."),
    (25, 28, "Система", "Память руками, байты, C, своё ядро, команды."),
]
WEEKS = [
    (1, 7, "Основа логики", "Состояние, решения, циклы, модели, функции, ошибки."),
    (8, 14, "Как устроены приложения", "Справочники, сохранение, слои, цикл событий, модель и вид, автоматы, отмена."),
    (15, 22, "Как устроены игры", "Игровой цикл, физика, столкновения, сущности, экраны, плавность, события, ИИ."),
    (23, 30, "Как устроена система", "Службы, память, параллельность, очереди, буферы, планировщик, права, общий скелет."),
]


def hl(code, shell=False):
    out = []
    for line in code.rstrip("\n").split("\n"):
        com = ""
        i = line.find("#" if shell else "//")
        if i >= 0:
            line, com = line[:i], line[i:]
        buf = ""
        for p in re.split(r'("(?:[^"\\]|\\.)*")', line):
            if p.startswith('"') and p.endswith('"') and len(p) >= 2:
                buf += '<span class="st">' + html.escape(p, quote=False) + "</span>"
                continue
            e = html.escape(p, quote=False)
            if shell:
                e = re.sub(r"^(\s*)(tb|brew|qemu-system-x86_64|\./tools/kernel\.sh|\./omnium/run\.sh)\b", r'\1<span class="kw">\2</span>', e)
            else:
                e = re.sub(r"\b(" + "|".join(KW) + r")\b", r'<span class="kw">\1</span>', e)
                e = re.sub(r"\b(" + "|".join(TYPES) + r")\b", r'<span class="ty">\1</span>', e)
                e = re.sub(r"(?<![\w.])(0x[0-9A-Fa-f]+|\d+(?:\.\d+)?)\b", r'<span class="nu">\1</span>', e)
            buf += e
        if com:
            buf += '<span class="cm">' + html.escape(com, quote=False) + "</span>"
        out.append(buf)
    return "\n".join(out)


def page(title, desc, depth, head, body, on=""):
    up = "../" * depth
    return f"""<!doctype html>
<html lang="ru">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<meta name="color-scheme" content="light">
<meta name="theme-color" content="#05070d">
<title>{html.escape(title)}</title>
<meta name="description" content="{html.escape(desc)}">
<meta property="og:title" content="{html.escape(title)}">
<meta property="og:image" content="{up}img/tachbit-icon-light-512.png">
<link rel="icon" href="{up}img/tachbit-icon-light-512.png">
<link rel="stylesheet" href="{up}learn/learn.css">
</head>
<body>
<div class="top"><div class="wrap">
<header>
  <a href="{up}index.html"><img src="{up}img/tachbit-logo-dark.svg" alt="Tachbit"></a>
  <nav>
    <a href="{up}index.html" class="hide">Главная</a>
    <a href="{up}learn/" class="{'on' if on == 'learn' else ''}">Уроки</a>
    <a href="{up}learn/lang/ref.html" class="{'on' if on == 'ref' else ''}">Указатель</a>
    <a href="https://github.com/superbro456/Tachbit">GitHub</a>
  </nav>
</header>
<div class="head">{head}</div>
</div></div>
<div class="wrap">
{body}
<footer>
  <img src="{up}img/tachbit-icon-light-512.png" alt="">
  <span>Tachbit бесплатный и открытый, лицензия MIT. Все примеры проверены компилятором Tachbit.</span>
  <a href="{up}index.html">Главная</a>
  <a href="{up}learn/">Уроки</a>
  <a href="https://github.com/superbro456/Tachbit/blob/main/docs/guide.md">Справочник</a>
</footer>
</div>
<script>if(window.matchMedia('(max-width:900px)').matches){{var s=document.querySelector('details.side');if(s)s.removeAttribute('open')}}</script>
</body>
</html>
"""


def write(rel, text):
    p = os.path.join(DOCS, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "w").write(text)


# ---------- язык ----------

def lang_side(cur):
    s = '<details class="side" open><summary>Все уроки языка</summary>'
    for a, b, t, _ in PARTS:
        s += f"<h4>{t}</h4>"
        for n in range(a, b + 1):
            on = ' class="on"' if n == cur else ""
            s += f'<a href="{n:02d}.html"{on}><b>{n}</b>{L[n-1]["title"]}</a>'
    on = ' class="on"' if cur == "ref" else ""
    s += f'<h4>В конце</h4><a href="ref.html"{on}><b>✱</b>Указатели</a>'
    return s + "</details>"


def part_name(n):
    for a, b, t, _ in PARTS:
        if a <= n <= b:
            return t


def lang_out(n, k, mode):
    if mode == "none":
        return ""
    if mode in ("check", "lib-check"):
        return '<div class="lbl">Проверка</div><pre class="out">tb check: ошибок нет. Откроется окно, поэтому запускайте у себя на компьютере.</pre>'
    o = open(os.path.join(HERE, "lang_out", f"{n:02d}_{k}.txt")).read().rstrip()
    lbl = "Вывод"
    if mode.startswith("in:"):
        lbl = "Вывод (ввели: " + html.escape(mode[3:].strip()) + ")"
    return f'<div class="lbl">{lbl}</div><pre class="out">{html.escape(o, quote=False)}</pre>'


def pn2(prev, nxt):
    s = '<div class="pn2">'
    if prev:
        s += f'<a href="{prev[0]}"><small>← назад</small>{prev[1]}</a>'
    if nxt:
        s += f'<a class="nx" href="{nxt[0]}"><small>дальше →</small>{nxt[1]}</a>'
    return s + "</div>"


def lang_lesson(n):
    d = L[n - 1]
    chips = "".join(f"<span>{html.escape(w)}</span>" for w in d["words"])
    head = (f'<div class="crumb"><a href="../">Уроки</a> › <a href="./">Язык Tachbit</a> › {part_name(n)}</div>'
            f'<h1><span class="no">{n:02d}</span>{d["title"]}</h1><p class="goal">{d["goal"]}</p><div class="chips">{chips}</div>')
    body = ""
    for k, part in enumerate(d["parts"]):
        h, txt, code, mode = part[:4]
        shell = mode == "none" and code.lstrip().startswith(("tb ", "brew", "TB_"))
        body += f"<h2>{h}</h2><p>{txt}</p>"
        body += f'<pre class="code{" sh" if shell else ""}">{hl(code, shell)}</pre>'
        body += lang_out(n, k, mode)
    rules = "".join(f"<li>{r}</li>" for r in d["rules"])
    body += f'<div class="rules"><b>Запомнить</b><ul>{rules}</ul></div>'
    body += f'<div class="task"><b>Задание</b>{d["task"]}</div>'
    prev = (f"{n-1:02d}.html", f"{n-1}. {L[n-2]['title']}") if n > 1 else None
    nxt = (f"{n+1:02d}.html", f"{n+1}. {L[n]['title']}") if n < len(L) else ("ref.html", "Указатели")
    body += pn2(prev, nxt)
    main = f'<div class="page">{lang_side(n)}<main>{body}</main></div>'
    write(f"lang/{n:02d}.html", page(f"{n}. {d['title']} · Язык Tachbit", d["goal"], 2, head, main, "learn"))


def lang_ref():
    kw = "".join(f'<tr><td>{html.escape(a)}</td><td>{b}</td><td><a href="{c:02d}.html">урок {c}</a></td></tr>' for a, b, c in KEYWORDS)
    bi = ""
    for g, items in BUILTINS:
        bi += f'<tr class="g"><td colspan="3">{g}</td></tr>'
        bi += "".join(f'<tr><td>{html.escape(a)}</td><td>{b}</td><td><a href="{c:02d}.html">урок {c}</a></td></tr>' for a, b, c in items)
    ms = "".join(f"<tr><td>«{html.escape(a)}»</td><td>{b}</td></tr>" for a, b in MESSAGES)
    head = ('<div class="crumb"><a href="../">Уроки</a> › <a href="./">Язык Tachbit</a></div>'
            '<h1>Указатели</h1><p class="goal">Все слова языка, все встроенные функции и частые сообщения компилятора. У каждой строки ссылка на урок, где это объяснено.</p>')
    body = f"""<h2 id="words">1. Слова языка</h2>
<table class="ref"><tr><th>Слово</th><th>Что делает</th><th>Где</th></tr>{kw}</table>
<h2 id="builtins">2. Встроенные функции</h2>
<p>✱ значит, что функция может закончиться ошибкой: зовите её с <code>try</code>, <code>or</code> или <code>else err</code>.</p>
<table class="ref"><tr><th>Функция</th><th>Что делает</th><th>Где</th></tr>{bi}</table>
<p>Ещё методы: у списка <code>.add .pop .clear</code>, у dict <code>.has .remove .keys .values .clear</code>, у канала <code>.send .receive .close .ready</code>, у фона <code>.wait .crashed</code>. Библиотеки <code>lib/ui.tb</code>, <code>lib/physics.tb</code>, <code>lib/anim.tb</code> описаны в <a href="https://github.com/superbro456/Tachbit/blob/main/docs/guide.md">справочнике</a>.</p>
<h2 id="messages">3. Частые сообщения компилятора</h2>
<p>Компилятор пишет номер строки и почти всегда подсказывает, как исправить.</p>
<table><tr><th style="width:42%">Сообщение</th><th>Что значит и что делать</th></tr>{ms}</table>"""
    body += pn2((f"{len(L):02d}.html", f"{len(L)}. {L[-1]['title']}"), None)
    main = f'<div class="page">{lang_side("ref")}<main>{body}</main></div>'
    write("lang/ref.html", page("Указатели · Язык Tachbit", "Слова языка, встроенные функции и сообщения компилятора Tachbit.", 2, head, main, "ref"))


def lang_index():
    head = ('<div class="crumb"><a href="../">Уроки</a></div>'
            '<h1>Язык <em>Tachbit</em></h1><p class="goal">28 уроков: все слова, команды и встроенные функции языка. В каждом уроке объяснение, проверенный пример с настоящим выводом и задание.</p>')
    body = '<div class="dl"><a href="01.html">Начать с урока 1</a><a class="l" href="ref.html">Указатели</a><a class="l" href="../files/tachbit-yazyk-1.2.pdf">Книга PDF</a><a class="l" href="../files/tachbit-yazyk-kod.zip">Код примеров .zip</a></div>'
    for a, b, t, d in PARTS:
        body += f'<div class="grp">{t} · {d}</div><div class="grid">'
        body += "".join(f'<a href="{n:02d}.html"><b>{n}</b><span>{L[n-1]["title"]}<small>{L[n-1]["goal"]}</small></span></a>' for n in range(a, b + 1))
        body += "</div>"
    write("lang/index.html", page("Язык Tachbit · уроки", "28 уроков по языку Tachbit с проверенными примерами.", 2, head, f'<div style="padding:30px 0 0">{body}</div>', "learn"))


# ---------- паттерны ----------

def week_name(n):
    for a, b, t, _ in WEEKS:
        if a <= n <= b:
            return t


def pat_side(cur):
    s = '<details class="side" open><summary>Все 30 паттернов</summary>'
    for a, b, t, _ in WEEKS:
        s += f"<h4>{t}</h4>"
        for n in range(a, b + 1):
            on = ' class="on"' if n == cur else ""
            s += f'<a href="{n:02d}.html"{on}><b>{n}</b>{D[n]["pattern"][0]}</a>'
    return s + "</details>"


def pat_day(n):
    d = D[n]
    pn, pr = d["pattern"]
    code = open(os.path.join(HERE, "days_code", f"day{n:02d}.tb")).read()
    head = (f'<div class="crumb"><a href="../">Уроки</a> › <a href="./">Паттерны</a> › {week_name(n)}</div>'
            f'<h1><span class="no">{n:02d}</span>{d["title"]}</h1><p class="goal">{d["goal"]}</p>')
    body = "".join(f"<p>{p}</p>" for p in d["text"])
    body += f'<div class="pattern"><div class="pn">Паттерн «{pn}»</div><p>{pr}</p></div>'
    body += f'<div class="where"><div class="a"><b>Приложение</b>{d["app"]}</div><div class="g"><b>Игра</b>{d["game"]}</div><div class="o"><b>ОС</b>{d["os"]}</div></div>'
    body += f'<h2>Код</h2><div class="fname">day{n:02d}.tb</div><pre class="code">{hl(code)}</pre>'
    if d.get("window"):
        body += '<div class="lbl">Что будет</div><pre class="out">Откроется окно. Запускайте у себя на компьютере.</pre>'
    else:
        o = open(os.path.join(HERE, "days_out", f"day{n:02d}.txt")).read().rstrip()
        lbl = "Вывод"
        if d.get("run_in"):
            lbl = f"Вывод (ввели: {html.escape(d['run_in'])})"
        if n == 23:
            o = re.sub(r"\d{4}-\d\d-\d\d \d\d:\d\d:\d\d", "2026-11-22 12:30:00", o)
        body += f'<div class="lbl">{lbl}</div><pre class="out">{html.escape(o, quote=False)}</pre>'
    body += f'<div class="task"><b>Задание</b>{d["task"]}</div>'
    prev = (f"{n-1:02d}.html", f"{n-1}. {D[n-1]['pattern'][0]}") if n > 1 else None
    nxt = (f"{n+1:02d}.html", f"{n+1}. {D[n+1]['pattern'][0]}") if n < 30 else None
    body += pn2(prev, nxt)
    main = f'<div class="page">{pat_side(n)}<main>{body}</main></div>'
    write(f"patterns/{n:02d}.html", page(f"{n}. {pn} · Паттерны Tachbit", d["goal"], 2, head, main, "learn"))


def pat_index():
    head = ('<div class="crumb"><a href="../">Уроки</a></div>'
            '<h1>Как думает <em>программа</em></h1><p class="goal">30 паттернов: приёмы, из которых собраны приложения, игры и операционные системы. Один паттерн в день, у каждого код на Tachbit и задание.</p>')
    body = """<h2 class="sec" style="margin-top:36px"><span class="dot"></span>Большая картина</h2>
<p>Приложение, игра и ОС выглядят совсем разными, но внутри у них один скелет. Есть <b>состояние</b> (что программа помнит), <b>события</b> (что его меняет), <b>цикл</b> (кто ждёт события) и <b>слои</b> (кто за что отвечает). Разница только в том, как крутится цикл.</p>
<p><b>Приложение</b></p><div class="loop"><span>ждать событие</span><i>→</i><span>разобрать</span><i>→</i><span>изменить состояние</span><i>→</i><span>перерисовать</span><i>↺</i></div>
<p><b>Игра</b></p><div class="loop"><span>dt</span><i>→</i><span>ввод</span><i>→</i><span>обновить мир</span><i>→</i><span>нарисовать кадр</span><i>↺ 60 раз в секунду</i></div>
<p><b>Ядро ОС</b></p><div class="loop"><span>прерывание</span><i>→</i><span>кто это</span><i>→</i><span>разбудить службу</span><i>→</i><span>выбрать следующую задачу</span><i>↺</i></div>
<div class="dl"><a href="01.html">Начать с паттерна 1</a><a class="l" href="../files/tachbit-kurs-30-dney.pdf">Курс PDF</a><a class="l" href="../files/tachbit-kurs-kod.zip">Код .zip</a></div>"""
    for a, b, t, d in WEEKS:
        body += f'<div class="grp">Неделя {(a + 6) // 7} · {t}</div><div class="grid">'
        body += "".join(f'<a href="{n:02d}.html"><b>{n}</b><span>{D[n]["pattern"][0]}<small>{D[n]["pattern"][1]}</small></span></a>' for n in range(a, b + 1))
        body += "</div>"
    body += '<div class="box" style="margin-top:30px"><b>Главная мысль курса.</b> Перед кодом ответьте на четыре вопроса. Что программа помнит? Какие события её меняют? Какой у неё цикл? На какие слои она делится? Если ответы есть, код пишется почти сам.</div>'
    write("patterns/index.html", page("Паттерны · Как думает программа", "30 паттернов программирования на Tachbit.", 2, head, f"<div>{body}</div>", "learn"))


# ---------- хаб ----------

def hub():
    head = ('<h1>Уроки <em>Tachbit</em></h1>'
            '<p class="goal">Два курса на русском. «Язык» учит сам Tachbit: слова, функции, команды. «Паттерны» учат думать как программист: как устроены приложения, игры и системы. Все примеры запущены настоящим компилятором.</p>'
            '<p class="goal" style="font-size:14px;margin-top:10px;color:#8fa1b5" lang="en">Lessons are in Russian for now.</p>')
    body = """<div class="hub">
<a class="big" href="lang/"><div class="n">28 УРОКОВ</div><h3>Язык Tachbit</h3><p>От первой строки до своего ядра: значения, циклы, списки, функции, ошибки, файлы, сеть, ядра, окна, 3D, звук, C.</p></a>
<a class="big" href="patterns/"><div class="n">30 ПАТТЕРНОВ</div><h3>Как думает программа</h3><p>Состояние, охранник, справочник, слои, конечный автомат, игровой цикл, сущности, службы, очереди, планировщик.</p></a>
</div>
<h2 class="sec"><span class="dot"></span>С чего начать</h2>
<p class="sub">Если вы только начинаете, идите параллельно: утром урок языка, вечером паттерн. Код набирайте руками, запускайте <code>tb run файл.tb</code> и сравнивайте вывод с уроком.</p>"""
    for a, b, t, d in PARTS:
        pass
    body += '<div class="grp">Язык · первые уроки</div><div class="grid">'
    body += "".join(f'<a href="lang/{n:02d}.html"><b>{n}</b><span>{L[n-1]["title"]}</span></a>' for n in range(1, 7))
    body += '</div><div class="grp">Паттерны · первая неделя</div><div class="grid">'
    body += "".join(f'<a href="patterns/{n:02d}.html"><b>{n}</b><span>{D[n]["pattern"][0]}</span></a>' for n in range(1, 7))
    body += """</div>
<h2 class="sec"><span class="dot"></span>Скачать</h2>
<p class="sub">Те же курсы книгами и все примеры файлами.</p>
<div class="dl"><a class="l" href="files/tachbit-yazyk-1.2.pdf">Язык Tachbit, PDF</a><a class="l" href="files/tachbit-yazyk-kod.zip">Код уроков языка</a><a class="l" href="files/tachbit-kurs-30-dney.pdf">Как думает программа, PDF</a><a class="l" href="files/tachbit-kurs-kod.zip">Код паттернов</a></div>"""
    write("index.html", page("Уроки Tachbit", "Уроки по языку Tachbit и паттернам программирования, с проверенными примерами.", 1, head, f"<div>{body}</div>", "learn"))


for n in range(1, len(L) + 1):
    lang_lesson(n)
lang_ref()
lang_index()
for n in range(1, 31):
    pat_day(n)
pat_index()
hub()
print("ok", len(L), "уроков,", 30, "паттернов")
