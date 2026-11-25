import html, re, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from l1 import L as L1
from l2 import L as L2
L = L1 + L2

KW = ["func", "gives", "return", "if", "else", "each", "in", "count", "from", "to", "repeat", "while",
      "times", "stop", "skip", "obj", "enum", "match", "use", "modpub", "modpri", "try", "or", "fail", "and", "not",
      "true", "false", "list", "dict", "channel", "background", "shared", "lock", "unlock", "temp", "danger", "move",
      "link", "const", "on", "all", "cores", "by", "vec", "columns", "together", "shape", "at", "bits", "bytes", "big",
      "timer", "keyboard", "err", "for", "c"]
TYPES = ["int", "dec", "text", "bool", "u8", "u16", "u32", "u64", "i8", "i16", "i32", "i64", "f32"]

PARTS = [
    (1, 12, "Часть 1. Основа языка", "Значения, решения, циклы, текст, списки, словари, функции, свои типы, ошибки."),
    (13, 17, "Часть 2. Настоящие программы", "Модули, числа и время, файлы, сеть, память."),
    (18, 20, "Часть 3. Несколько ядер и скорость", "Фон, замки, каналы, все ядра, SIMD."),
    (21, 24, "Часть 4. Окна, игры, звук", "2D, 3D, звук, кнопки и поля."),
    (25, 28, "Часть 5. Система", "Память руками, байты, C, своё ядро, команды."),
]


def part_of(n):
    for a, b, t, _ in PARTS:
        if a <= n <= b:
            return t.split(".")[0]


def hl(code, shell=False):
    out = []
    for line in code.rstrip("\n").split("\n"):
        com = ""
        i = line.find("#" if shell else "//")
        if i >= 0:
            line, com = line[:i], line[i:]
        parts = re.split(r'("(?:[^"\\]|\\.)*")', line)
        buf = ""
        for p in parts:
            if p.startswith('"') and p.endswith('"') and len(p) >= 2:
                buf += '<span class="s">' + html.escape(p) + "</span>"
            else:
                e = html.escape(p)
                if not shell:
                    e = re.sub(r"\b(" + "|".join(KW) + r")\b", r'<span class="k">\1</span>', e)
                    e = re.sub(r"\b(" + "|".join(TYPES) + r")\b", r'<span class="t">\1</span>', e)
                    e = re.sub(r"(?<![\w.])(0x[0-9A-Fa-f]+|\d+(?:\.\d+)?)\b", r'<span class="n">\1</span>', e)
                else:
                    e = re.sub(r"^(\s*)(tb|qemu-system-x86_64|brew|\./tools/kernel\.sh|\./omnium/run\.sh)\b", r'\1<span class="k">\2</span>', e)
                buf += e
        if com:
            buf += '<span class="c">' + html.escape(com) + "</span>"
        out.append(buf)
    return "\n".join(out)


CSS = r"""
@page { size: A4; margin: 12mm 14mm 13mm 14mm; }
:root { --ink:#1d2333; --mute:#5d6680; --line:#e3e6ef; --soft:#f5f6fb; --acc:#2f6f50; --acc2:#14a39a; --vio:#6a4fd6; }
* { box-sizing: border-box; }
html { -webkit-print-color-adjust: exact; print-color-adjust: exact; }
body { margin:0; font-family: Inter, "DejaVu Sans", sans-serif; color:var(--ink); font-size:10pt; line-height:1.42; background:#fff; }
h1,h2,h3 { line-height:1.2; margin:0; }
code, pre { font-family: "JetBrains Mono", "DejaVu Sans Mono", monospace; }
code { background:var(--soft); border-radius:4px; padding:0 4px; font-size:9.2pt; }
p { margin: 0 0 7px; }
.page { page-break-after: always; }
.cover { height: 270mm; display:flex; flex-direction:column; justify-content:space-between;
  background: radial-gradient(120% 80% at 85% 5%, #dff0e6 0%, #f3f8f5 45%, #ffffff 75%); border-radius:18px; padding:22mm 16mm; }
.cover .tag { font-size:10pt; letter-spacing:.12em; text-transform:uppercase; color:var(--acc); font-weight:600; }
.cover h1 { font-size:36pt; margin-top:10mm; font-weight:700; }
.cover .sub { font-size:14pt; color:var(--mute); margin-top:6mm; max-width:150mm; }
.cover .chips { margin-top:12mm; display:flex; flex-wrap:wrap; gap:6px; max-width:160mm; }
.cover .chips span { font-family:"DejaVu Sans Mono",monospace; font-size:9pt; background:#fff; border:1px solid #cfe3d7; border-radius:999px; padding:3px 9px; color:var(--acc); }
.cover .foot { color:var(--mute); font-size:9.5pt; }
h2.sec { font-size:20pt; margin: 0 0 5mm; }
h3 { font-size:12pt; margin: 5mm 0 2mm; color:var(--acc); }
table { width:100%; border-collapse:collapse; font-size:9pt; margin: 2mm 0 4mm; }
th, td { text-align:left; vertical-align:top; padding:3px 7px; border-bottom:1px solid var(--line); }
th { color:var(--mute); font-weight:600; }
.box { background:var(--soft); border-radius:12px; padding:9px 14px; margin: 3mm 0; }
.lesson { page-break-before: always; }
.lhead { display:flex; align-items:baseline; gap:10px; border-bottom:2px solid var(--ink); padding-bottom:3mm; margin-bottom:3mm; }
.lhead .num { font-size:26pt; font-weight:700; color:var(--acc); }
.lhead h2 { font-size:17pt; }
.lhead .pt { margin-left:auto; color:var(--mute); font-size:9pt; white-space:nowrap; }
.goal { font-weight:600; margin-bottom:2mm; }
.words { display:flex; flex-wrap:wrap; gap:4px; margin: 0 0 3mm; }
.words span { font-family:"DejaVu Sans Mono",monospace; font-size:8.2pt; background:#eef6f1; color:var(--acc); border-radius:6px; padding:1px 6px; }
pre.code { background:#161a28; color:#e6e9f2; border-radius:10px; padding:9px 12px; font-size:8.1pt; line-height:1.36; white-space:pre-wrap; margin: 2mm 0; page-break-inside: avoid; }
pre.code .k { color:#c4a3ff; } pre.code .t { color:#7ad3f5; } pre.code .s { color:#9fdf8f; } pre.code .n { color:#ffc27a; } pre.code .c { color:#7f88a3; font-style:italic; }
pre.sh { background:#20252f; }
.lbl { font-size:8.4pt; color:var(--mute); text-transform:uppercase; letter-spacing:.06em; margin-top:2mm; }
pre.out { background:var(--soft); border-radius:8px; padding:6px 12px; font-size:8.1pt; margin:1mm 0 2mm; white-space:pre-wrap; page-break-inside: avoid; }
.rules { border-left:4px solid var(--acc2); background:#f2fbfa; border-radius:0 10px 10px 0; padding:7px 12px; margin:3mm 0; font-size:9.4pt; page-break-inside: avoid; }
.rules b { color:var(--acc2); } .rules ul { margin:3px 0 0 16px; padding:0; } .rules li { margin:2px 0; }
.task { background:#fff7e0; border-radius:10px; padding:8px 12px; margin-top:3mm; font-size:9.6pt; page-break-inside: avoid; }
.task b { color:#a06a00; }
.toc td:first-child { width:9mm; font-weight:700; color:var(--acc); }
.toc tr.p td { background:#f3f8f5; font-weight:700; color:var(--ink); padding-top:5px; }
.ref { table-layout:fixed; } .ref td:first-child { font-family:"DejaVu Sans Mono",monospace; color:var(--vio); font-size:8.2pt; width:48%; word-break:break-word; } .ref td:nth-child(3), .ref th:nth-child(3) { width:14mm; }
.ref td:last-child { white-space:nowrap; color:var(--mute); }
.ref tr.g td { background:#f3f8f5; font-weight:700; color:var(--ink); font-family:Inter,sans-serif; font-size:9pt; }
.cols2 { column-count:2; column-gap:8mm; }
"""


def cover():
    chips = "".join(f"<span>{w}</span>" for w in ["func", "if", "each", "count", "list", "dict", "obj", "enum", "match", "fail", "try", "link", "background", "channel", "on all cores", "temp", "danger", "shape", "use c", "window_open", "draw_mesh", "play_sound", "tb bare"])
    return f"""
<div class="page"><div class="cover">
<div>
<div class="tag">Tachbit 1.3 · справочный курс</div>
<h1>Язык Tachbit<br>от первой строки до своего ядра</h1>
<div class="sub">Все слова, команды и встроенные функции языка: что делает, как пишется, проверенный пример с настоящим выводом и задание. 28 уроков и полный указатель.</div>
<div class="chips">{chips}</div>
</div>
<div class="foot">Версия от 25 ноября 2026 · Ядро языка заморожено с 1.2, в 1.3 добавлены только новые слова · Каждый пример проверен компилятором Tachbit 1.3</div>
</div></div>"""


def intro():
    rows = ""
    for a, b, t, d in PARTS:
        rows += f'<tr class="p"><td></td><td>{t} <span style="font-weight:400;color:#5d6680">— {d}</span></td></tr>'
        for n in range(a, b + 1):
            rows += f"<tr><td>{n}</td><td>{L[n-1]['title']}</td></tr>"
    return f"""
<div class="page">
<h2 class="sec">Как устроена эта книга</h2>
<p>Первый курс («Как думает программа») учил логике: как устроены приложения, игры и системы. Эта книга — про сам язык. В каждом уроке одна тема и все её слова: что слово делает, как пишется и где ошибиться. Код набирайте руками, запускайте <code>tb run main.tb</code> и сравнивайте вывод с книгой.</p>
<div class="box"><b>Как читать уроки.</b> Сверху — цель и список слов урока. Потом объяснения с примерами. Под каждым примером — его настоящий вывод: все примеры запущены компилятором Tachbit 1.3. Примеры с окном, звуком и своим ядром проверены компилятором, но запускать их нужно на своём компьютере. В рамке «Запомнить» — правила, на которых чаще ошибаются. В конце — задание.</div>
<p>В конце книги три указателя: все слова языка, все встроенные функции и частые сообщения компилятора.</p>
<h3>Содержание</h3>
<table class="toc">{rows}<tr class="p"><td></td><td>Указатели: слова языка · встроенные функции · сообщения компилятора</td></tr></table>
</div>"""


def show_out(n, k, part):
    mode = part[3]
    if mode == "none":
        return ""
    if mode in ("check", "lib-check"):
        return '<div class="lbl">Проверка</div><pre class="out">tb check: ошибок нет. Откроется окно — запускайте на своём компьютере.</pre>'
    o = open(os.path.join(HERE, "out", f"{n:02d}_{k}.txt")).read().rstrip()
    lbl = "Вывод"
    if mode.startswith("in:"):
        lbl = "Вывод (ввели: " + html.escape(mode[3:].strip()) + ")"
    return f'<div class="lbl">{lbl}</div><pre class="out">{html.escape(o)}</pre>'


def lesson(n):
    d = L[n - 1]
    words = "".join(f"<span>{html.escape(w)}</span>" for w in d["words"])
    body = ""
    for k, part in enumerate(d["parts"]):
        head, txt, code, mode = part[:4]
        shell = mode == "none" and (code.lstrip().startswith(("tb ", "brew", "TB_")))
        body += f"<h3>{head}</h3><p>{txt}</p>"
        body += f'<pre class="code{" sh" if shell else ""}">{hl(code, shell)}</pre>'
        body += show_out(n, k, part)
    rules = "".join(f"<li>{r}</li>" for r in d["rules"])
    return f"""
<div class="lesson">
<div class="lhead"><div class="num">{n}</div><h2>{d['title']}</h2><div class="pt">{part_of(n)}</div></div>
<div class="goal">Цель: {d['goal']}</div>
<div class="words">{words}</div>
{body}
<div class="rules"><b>Запомнить</b><ul>{rules}</ul></div>
<div class="task"><b>Задание.</b> {d['task']}</div>
</div>"""


KEYWORDS = [
    ("func", "объявить функцию", 8), ("return", "отдать ответ и выйти", 8), ("gives", "несколько ответов функции", 8),
    ("if / else if / else", "решение", 3), ("and / or / not", "соединить условия", 3), ("true / false", "да / нет", 2),
    ("const", "значение, которое нельзя менять", 2), ("int dec text bool", "типы", 2),
    ("repeat N times", "N раз", 4), ("repeat while", "пока верно", 4), ("count i from A to B", "счётчик", 4), ("each x in …", "по элементам", 4),
    ("skip / stop", "следующий шаг / выйти из цикла", 4),
    ("list T / [ ]", "список", 6), ("dict K V / { }", "словарь", 7), ("obj", "свой тип", 9), ("link", "ярлык на место", 10),
    ("enum", "варианты", 11), ("match", "разбор по вариантам", 11),
    ("fail", "закончить функцию ошибкой", 12), ("try", "передать ошибку выше", 12), ("or", "запасное значение при ошибке", 12), ("else err", "свой блок на ошибку", 12),
    ("use", "подключить модуль", 13), ("modpub / modpri", "видно другим файлам / только своему", 13),
    ("temp", "временная память блока", 17),
    ("background / .wait / .crashed", "работа в фоне", 18), ("shared", "общая переменная", 18), ("lock / unlock", "замок", 18),
    ("channel T", "очередь между потоками", 19), ("move", "отдать список целиком", 19), ("on all cores", "цикл на всех ядрах", 19),
    ("by vec", "SIMD-сумма", 20), ("obj … by columns", "хранить колонками", 20), ("each piece in … by N", "кусками", 20), ("together", "несколько чтений сразу", 20),
    ("danger", "блок опасных слов", 25), ("shape / at", "раскладка байтов", 25), ("u8…u64 i8…i64 bits bytes big", "поля shape и типы C", 25),
    ("use c", "звать C", 26), ("for c", "видно из C", 26),
    ("on timer / on keyboard / on interrupt N", "функция прерывания (ядро)", 27), ("//", "комментарий", 1),
]

BUILTINS = [
    ("Вывод и ввод", [("print(a, b, …)", "вывести через пробел и перевести строку", 1), ("show(…)", "вывести без перевода строки", 1),
                      ("input()", "строка с клавиатуры ✱", 15), ("args()", "слова после имени файла", 15)]),
    ("Текст", [("len(s)", "сколько букв (у списка и dict — элементов)", 5), ("cut(s, a, b)", "кусок с буквы a по b", 5), ("find(s, x)", "номер буквы или -1", 5),
               ("replace(s, a, b)", "заменить", 5), ("split(s, sep) / join(xs, sep)", "разрезать / склеить", 5), ("upper / lower / trim", "регистр, пробелы по краям", 5),
               ("starts_with / ends_with", "начинается / кончается", 5), ("code(s) / char(n)", "номер буквы / буква по номеру", 5),
               ("to_int(s) / to_dec(s)", "текст в число ✱", 5), ("byte_len / byte_at / byte_char", "работа с байтами", 5)]),
    ("Числа", [("round / abs / min / max / sqrt", "округлить, модуль, меньшее, большее, корень", 14), ("sin / cos / tan / asin / acos / atan / atan2 / pi()", "углы (радианы)", 14), ("pow / exp / log / floor / ceil", "степень, логарифм, вниз и вверх до целого", 14), ("random(a, b)", "случайное целое", 14),
               ("bit_and / bit_or / bit_xor / bit_not", "биты", 14), ("shift_left / shift_right", "сдвиг битов", 14), ("sort(xs)", "отсортировать список", 6)]),
    ("Время", [("time_ms() / wait_ms(n)", "миллисекунды / подождать", 14), ("date_now() / unix_time()", "дата текстом / секунды с 1970", 14)]),
    ("Файлы и система", [("read_file / write_file", "прочитать / записать файл ✱", 15), ("file_exists / file_size", "есть ли файл / размер", 15),
                         ("list_dir / is_folder / make_folder", "папки ✱", 15), ("move(a, b) / remove(p)", "переместить / удалить ✱", 15),
                         ("env(name)", "переменная окружения", 15), ("run(cmd) / exit(code)", "команда терминала / выйти", 15)]),
    ("Сеть", [("http_get(url)", "текст страницы ✱", 16), ("json_get(j, path) / json_text(s)", "JSON ✱", 16),
              ("net_listen / net_port / net_accept", "сервер ✱", 16), ("net_connect / net_send / net_receive / net_close", "связь ✱", 16)]),
    ("Окно и ввод", [("window_open(w, h, title)", "открыть окно ✱", 21), ("window_alive()", "следующий кадр", 21), ("window_w / window_h / window_sharp / window_scale", "размер, Retina", 21),
                     ("window_new / window_use / window_shown / window_close", "ещё окна ✱", 21), ("window_fullscreen", "полный экран", 21), ("frame_ms / fps / set_fps", "время кадра и частота", 21), ("key_down(name) / typed_text()", "клавиши / набранный текст", 21),
                     ("mouse_x / mouse_y / mouse_down / mouse_wheel / mouse_back / mouse_forward", "мышь", 21), ("mouse_lock / mouse_dx / mouse_dy", "мышь для шутера", 21), ("pad_count / pad_down / pad_axis", "геймпад", 21),
                     ("clipboard_get / clipboard_set / set_cursor", "буфер обмена, курсор", 21)]),
    ("2D", [("rgb / rgba", "цвет", 21), ("clear_window", "залить окно", 21), ("draw_pixel / draw_rect / draw_circle / draw_line / draw_triangle", "фигуры", 21),
            ("draw_round / blur_rect / draw_shadow", "скругление, матовое стекло, тень", 21), ("draw_text", "текст встроенным шрифтом", 21),
            ("load_font / draw_font / text_width", "шрифты TTF ✱", 21), ("load_image / draw_image / draw_image_alpha", "картинки ✱", 21),
            ("image_w / image_h / image_scaled / screen_image / image_from_mem", "размер, масштаб, снимок", 21)]),
    ("3D", [("mesh_new / mesh_add / mesh_add_uv / mesh_image", "сетка", 22), ("draw_mesh / draw_mesh_rot", "нарисовать сетку", 22),
            ("camera / sunlight", "камера и солнце", 22), ("point_light / spot_light / light_off / light_shadow", "лампы и фонари", 22), ("fog / shadows", "туман и тени", 22), ("mesh_smooth / mesh_shine / mesh_glow / mesh_alpha", "плавный свет, блеск, свечение, стекло", 22), ("bone_add / bone_box / bone_weight / bone_turn / bone_move", "кости", 22), ("mesh_clear / mesh_count / mesh_lod / mesh_far / mesh_free", "менять сетку, большие миры", 22), ("gpu func / mesh_shader / shader_set / screen_shader", "свои шейдеры", 22)]),
    ("Звук", [("load_sound", "загрузить WAV ✱", 23), ("play_sound / loop_sound / play_tone", "играть", 23), ("play_sound_at / loop_sound_at / move_sound / listener", "звук в 3D", 23),
              ("stop_sound / sound_playing / sound_volume / sound_pitch / master_volume", "управление", 23)]),
    ("Только в danger", [("new_mem / grow_mem / free_mem / copy_mem / fill_mem", "память руками", 25), ("read8…64 / write8…64", "числа по адресу", 25),
                         ("address_of / asm", "адрес списка / команда процессора", 25), ("zone_new / zone_take / zone_used / zone_clear / zone_free", "зоны", 25),
                         ("syscall(n, …)", "вызов ядра ОС", 26)]),
    ("Только в своём ядре", [("port_in8…32 / port_out8…32", "порты устройств", 27), ("page_new / page_free", "страницы памяти", 27),
                             ("interrupts_on / interrupts_off", "прерывания", 27), ("uptime_ms / read_key / yield", "время, клавиша, уступить", 27),
                             ("module_count / module_addr / module_size / kernel_symbol / call_address", "программы Omnium", 27)]),
]

MESSAGES = [
    ("это слово занято языком, возьми другое имя", "Имя переменной или параметра совпало со словом языка (<code>times</code>, <code>bool</code>, <code>size</code> у shape…). Переименуйте."),
    ("результат вызова пропадает: сохрани его", "Функция что-то отдаёт (например, номер голоса у <code>play_sound</code>), а вы это выбросили. Напишите <code>v = …</code>."),
    ("эта функция спрятана в своём модуле: поставь перед ней modpub", "Вы зовёте из другого файла функцию без <code>modpub</code>."),
    ("ожидалось значение, а тут: try", "<code>try</code> нельзя писать прямо внутри <code>print(…)</code> и других выражений. Сначала <code>x = try f()</code>, потом <code>print(x)</code>."),
    ("лишнее в конце строки: or", "<code>or</code> нужен запасной ответ, а у функции без ответа (<code>net_send</code>) его нет. Используйте <code>try</code> или <code>else err</code>."),
    ("не то количество значений в вызове", "Не хватает или лишний аргумент. Компилятор рядом подскажет, как пишется вызов."),
    ("функция может закончиться ошибкой: напиши try f(), f() or запасное или f() else err", "Функция с <code>fail</code> (или <code>read_file</code>, <code>to_int</code>…) вызвана просто так."),
    ("ожидался знак: )", "Чаще всего — лишнее в описании параметров (например, <code>link x: int</code>: у ярлыка тип не пишется)."),
    ("стек переполнен: функция зовёт себя слишком глубоко", "Рекурсия без остановки."),
    ("номер за пределами списка / деление на ноль / переполнение int", "Аварии во время работы: программа останавливается и пишет строку."),
]


def appendix():
    kw = "".join(f"<tr><td>{html.escape(a)}</td><td>{b}</td><td>урок {c}</td></tr>" for a, b, c in KEYWORDS)
    bi = ""
    for g, items in BUILTINS:
        bi += f'<tr class="g"><td colspan="3">{g}</td></tr>'
        bi += "".join(f"<tr><td>{html.escape(a)}</td><td>{b}</td><td>урок {c}</td></tr>" for a, b, c in items)
    ms = "".join(f"<tr><td>«{html.escape(a)}»</td><td>{b}</td></tr>" for a, b in MESSAGES)
    return f"""
<div class="lesson">
<h2 class="sec">Указатель 1. Слова языка</h2>
<table class="ref"><tr><th>Слово</th><th>Что делает</th><th>Где</th></tr>{kw}</table>
</div>
<div class="lesson">
<h2 class="sec">Указатель 2. Встроенные функции</h2>
<p>✱ — может сломаться: зовите с <code>try</code>, <code>or</code> или <code>else err</code>.</p>
<table class="ref"><tr><th>Функция</th><th>Что делает</th><th>Где</th></tr>{bi}</table>
<p>Ещё методы: у списка <code>.add .pop .clear</code>, у dict <code>.has .remove .keys .values .clear</code>, у канала <code>.send .receive .close .ready</code>, у фона <code>.wait .crashed</code>. Библиотеки <code>lib/ui.tb</code>, <code>lib/physics.tb</code>, <code>lib/anim.tb</code> описаны в <code>docs/guide.md</code>.</p>
</div>
<div class="lesson">
<h2 class="sec">Указатель 3. Частые сообщения компилятора</h2>
<p>Все эти сообщения автор книги получил сам, пока писал примеры. Компилятор пишет номер строки и почти всегда — как исправить.</p>
<table><tr><th style="width:42%">Сообщение</th><th>Что значит и что делать</th></tr>{ms}</table>
<div class="box"><b>Дальше.</b> Полный справочник со всеми подробностями — <code>docs/guide.md</code>. Готовые программы для чтения — <code>examples/</code> (игры, «Файлы», 3D) и <code>omnium/</code> (своя ОС на Tachbit).</div>
</div>"""


body = cover() + intro() + "".join(lesson(n) for n in range(1, len(L) + 1)) + appendix()
doc = f"<!doctype html><html lang='ru'><head><meta charset='utf-8'><title>Язык Tachbit</title><style>{CSS}</style></head><body>{body}</body></html>"
open(os.path.join(HERE, "lang.html"), "w").write(doc)
print("ok", len(L))
