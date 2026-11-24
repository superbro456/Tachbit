# Уроки 15–28.
L = []


def lesson(title, goal, parts, rules, task, words):
    L.append(dict(title=title, goal=goal, parts=parts, rules=rules, task=task, words=words))


lesson("Файлы, папки, ввод", "Сохранять данные между запусками и спрашивать пользователя.",
[
("Файлы",
 "<code>write_file</code> записывает текст в файл целиком, <code>read_file</code> читает его. Обе могут сломаться (нет прав, нет файла), поэтому их зовут с <code>try</code>, <code>or</code> или <code>else err</code>.",
'''func main()
    try write_file("notes.txt", "купить хлеб\\nпозвонить маме\\n")
    s = try read_file("notes.txt")
    each line in split(trim(s), "\\n")
        print("•", line)
    print(file_exists("notes.txt"), file_size("notes.txt"))
    missing = read_file("нет-такого.txt") or "(пусто)"
    print(missing)
''', "run"),
("Папки",
 "Для папок: <code>list_dir</code> (имена, у папок <code>/</code> в конце), <code>is_folder</code>, <code>make_folder</code>, <code>move</code> (переместить или переименовать), <code>remove</code> (файл или пустая папка).",
'''func main()
    try make_folder("box")
    try write_file("box/a.txt", "A")
    try move("box/a.txt", "box/b.txt")
    names = try list_dir("box")
    print(names, is_folder("box"))
    try remove("box/b.txt")
    try remove("box")
    print(file_exists("box"))
''', "run"),
("Ввод, слова запуска, окружение",
 "<code>input()</code> читает строку с клавиатуры (может сломаться, если ввод кончился). <code>args()</code> — список слов, написанных после имени файла: <code>tb run main.tb один два</code>. <code>env(\"HOME\")</code> — переменная окружения. <code>run(\"команда\")</code> запускает команду терминала и отдаёт код выхода. <code>exit(код)</code> завершает программу.",
'''func main()
    show("Как тебя зовут? ")
    name = input() or "гость"
    print("Привет,", name)
    print("слов после файла:", len(args()))
    print(len(env("HOME")) > 0)
    code_out = run("true")
    print("код выхода:", code_out)
    exit(0)
''', "in:Алия\n"),
],
["Путь пишется от папки, где запустили программу, или полный: <code>/Users/имя/…</code>.", "<code>\\n</code> в тексте — перевод строки, <code>\\t</code> — табуляция, <code>\\\"</code> — кавычка."],
"Сделайте «дневник»: программа читает <code>diary.txt</code> (если его нет — пустой), печатает старые записи, спрашивает новую через <code>input()</code> и дописывает её с датой <code>date_now()</code>.",
["read_file", "write_file", "file_exists", "file_size", "list_dir", "is_folder", "make_folder", "move", "remove", "input", "args", "env", "run", "exit"])

lesson("Интернет: HTTP, JSON и свой сервер", "Получать данные из сети и связывать программы.",
[
("Страница и JSON",
 "<code>http_get(адрес)</code> отдаёт текст страницы. <code>json_get(текст, путь)</code> достаёт значение по пути: ключи и номера через точку. <code>json_text(s)</code> делает из текста строку JSON в кавычках, чтобы собрать свой JSON.",
'''func main()
    j = "{\\"users\\": [{\\"name\\": \\"Алия\\", \\"age\\": 20}, {\\"name\\": \\"Вали\\"}]}"
    name = try json_get(j, "users.0.name")
    age = try json_get(j, "users.0.age")
    second = try json_get(j, "users.1")
    print(name, age)
    print(second)
    print("{\\"msg\\": " + json_text("он сказал \\"да\\"") + "}")
    // page = try http_get("https://example.com")   — нужен интернет
''', "run"),
("Свой сервер и клиент",
 "<code>net_listen(порт)</code> открывает сервер (0 — любой свободный, узнать какой: <code>net_port</code>). <code>net_accept</code> ждёт клиента. <code>net_connect(адрес, порт)</code> подключается. <code>net_send</code> и <code>net_receive</code> передают текст, <code>net_close</code> закрывает. Пустой ответ <code>\"\"</code> значит, что другая сторона закрыла связь.",
'''func serve(srv: int)
    c = net_accept(srv) else err
        return -1
    msg = net_receive(c) else err
        return -1
    net_send(c, "эхо: " + msg) else err
        return -1
    net_close(c)
    return 0

func main()
    srv = try net_listen(0)
    port = net_port(srv)
    s = background serve(srv)
    c = try net_connect("127.0.0.1", port)
    try net_send(c, "привет")
    answer = try net_receive(c)
    print(answer)
    net_close(c)
    print(s.wait)
''', "run"),
],
["Все сетевые слова, кроме <code>net_port</code> и <code>net_close</code>, могут сломаться: нет сети, порт занят, сервер не ответил.",
 "Чтобы сервер обслуживал многих, принимайте клиентов в цикле и отдавайте каждого в <code>background</code> (урок 18)."],
"Сделайте сервер-калькулятор: клиент шлёт <code>\"3 + 4\"</code>, сервер отвечает <code>\"7\"</code>. Проверьте его своим клиентом в той же программе.",
["http_get", "json_get", "json_text", "net_listen", "net_port", "net_accept", "net_connect", "net_send", "net_receive", "net_close"])

lesson("Память", "Понять, кто и когда отдаёт память, и где можно помочь.",
[
("Всё само",
 "В Tachbit не нужно писать <code>free</code>. Память шага цикла и функции компилятор отдаёт сам, сразу. То, что живёт дольше, убирает уборщик памяти: он сам находит объекты, на которые больше никто не смотрит. Программа, которая работает неделями, не растёт по памяти.",
'''func main()
    keep = dict int text
    count i from 1 to 300000
        order = "заказ №" + i + " на сумму " + (i % 1000)
        keep[i % 100] = order          // хранится только последние 100
    print(len(keep), keep[7])
''', "run"),
("temp: временная память руками",
 "Блок <code>temp</code> говорит: всё, что создано внутри, после блока не нужно. Память берётся большими кусками и отдаётся разом в конце — это очень быстро. Наружу можно вынести только числа.",
'''func main()
    total = 0
    repeat 10 times
        temp
            lines = list text
            count i from 1 to 10000
                lines.add("строка " + i)
            each s in lines
                total += len(s)
    print(total)
''', "run"),
],
["<code>.clear()</code> у списка и dict сразу отдаёт их память.", "<code>TB_GC_STATS=1 tb run файл.tb</code> печатает каждую уборку; <code>TB_GC=0</code> выключает уборщик.",
 "Внутри <code>temp</code> нельзя <code>return</code>, <code>fail</code>, <code>try</code>."],
"Возьмите пример с заказами и запустите его с <code>TB_GC_STATS=1</code>. Сколько было уборок? Потом поменяйте 100 на 100 000 и сравните.",
["temp", ".clear()", "TB_GC_STATS", "TB_GC"])

lesson("Работа в фоне: background, shared, lock", "Делать несколько дел одновременно.",
[
("background и .wait",
 "<code>background f(...)</code> запускает функцию в отдельном потоке (на другом ядре процессора) и сразу отдаёт «номерок». <code>.wait</code> ждёт, пока функция закончит, и забирает ответ. Пока фон работает, <code>main</code> делает своё.",
'''func sum_to(a: int, b: int)
    s = 0
    count i from a to b
        s += i
    return s

func main()
    x = background sum_to(1, 5000000)
    y = background sum_to(5000001, 10000000)
    z = sum_to(1, 10)                // main тем временем считает своё
    print(x.wait + y.wait, z)
''', "run"),
("shared и lock",
 "<code>shared</code> — переменная, общая для всей программы. Если её меняют из фона, это делают внутри <code>lock имя</code>: тогда в один момент блок выполняет только кто-то один, и сложения не теряются. <code>unlock имя</code> отпускает замок раньше конца блока.",
'''shared hits = int 0

func work(n: int)
    repeat n times
        lock counter
            hits += 1
    return 0

func main()
    a = background work(100000)
    b = background work(100000)
    print(a.wait + b.wait, hits)
''', "run"),
],
["В <code>background</code> можно передать только <code>int</code>, <code>dec</code>, <code>bool</code>, <code>text</code>, каналы и списки через <code>move</code>.",
 "Компилятор сам ловит: <code>shared</code> без замка, замок внутри того же замка, два замка в разном порядке.",
 "<code>a.crashed</code> — упала ли задача (в своей ОС Omnium падение задачи не роняет систему)."],
"Посчитайте простые числа до 2 000 000 в четырёх <code>background</code> по кускам и сравните время (<code>time_ms</code>) с подсчётом в одном потоке.",
["background", ".wait", ".crashed", "shared", "lock", "unlock"])

lesson("Каналы, move и все ядра", "Передавать данные между потоками и раздавать цикл всем ядрам.",
[
("channel",
 "Канал — очередь между потоками. <code>.send(x)</code> кладёт в конец, <code>.receive()</code> берёт из начала и ждёт, если пусто. <code>.close()</code> — больше писать не будут. <code>each x in ch</code> берёт, пока канал не закрыт и не пуст. <code>.ready()</code> — есть ли что взять без ожидания.",
'''func worker(jobs: channel int, results: channel text)
    each n in jobs
        results.send("готово " + n * n)
    return 0

func main()
    jobs = channel int
    results = channel text
    w = background worker(jobs, results)
    count i from 1 to 3
        jobs.send(i)
    jobs.close()
    count i from 1 to 3
        print(results.receive())
    print(w.wait)
''', "run"),
("move и on all cores",
 "<code>move</code> отдаёт список в фон целиком, без копии; после этого старое имя пустое. <code>on all cores</code> раздаёт цикл <code>count</code> или <code>each</code> всем ядрам процессора. Общий итог копят в <code>shared</code> через <code>+=</code>.",
'''func total(xs: list int)
    s = 0
    each x in xs
        s += x
    return s

shared found = int 0

func is_prime(n: int)
    if n < 2
        return false
    d = 2
    repeat while d * d <= n
        if n % d == 0
            return false
        d += 1
    return true

func main()
    data = list int
    count i from 1 to 1000
        data.add(i)
    a = background total(move data)
    print(a.wait)
    count n from 1 to 100000 on all cores
        if is_prime(n)
            found += 1
    print(found)
''', "run"),
],
["Внутри <code>on all cores</code> данные снаружи только читаются; компилятор следит даже через другие имена и функции.",
 "<code>TB_CORES=1 ./программа</code> — запустить на одном ядре для сравнения.",
 "По каналу идут <code>int</code>, <code>dec</code>, <code>bool</code>, <code>text</code> и <code>enum</code>."],
"Сделайте конвейер из трёх потоков: первый шлёт числа 1..20, второй возводит их в квадрат, третий складывает. Связь — двумя каналами.",
["channel T", ".send", ".receive", ".close", ".ready", "each x in ch", "move", "on all cores"])

lesson("Скорость одним словом", "Слова, которые ускоряют горячие циклы.",
[
("by vec и by columns",
 "<code>each x in nums by vec</code> складывает несколько чисел одной командой процессора (SIMD). <code>obj … by columns</code> хранит список объектов колонками: все <code>x</code> подряд, все <code>mass</code> подряд. Это быстрее, когда цикл читает одно-два качества из многих.",
'''obj Particle by columns
    x: dec
    mass: dec

func main()
    nums = list int
    count i from 1 to 1000
        nums.add(i)
    total = 0
    each x in nums by vec
        total += x * x
    print(total)

    parts = list Particle
    count i from 1 to 100
        parts.add(Particle(x: i * 1.0, mass: 2.0))
    m = 0.0
    each p in parts
        m += p.mass
    print(m)
''', "run"),
("Куски и together",
 "<code>each piece in data by N</code> идёт кусками по N элементов, чтобы кусок помещался в быстрый кэш процессора; <code>piece</code> — окно в сам список. <code>together</code> просит память принести несколько значений одновременно, а не по очереди.",
'''func main()
    data = list int
    count i from 1 to 10000
        data.add(i)
    sum = 0
    each piece in data by 4096
        each v in piece
            sum += v
    print(sum)

    ages = {"Алия": 20, "Вали": 17}
    together
        a = ages["Алия"]
        b = ages["Вали"]
        x = data[9999]
    print(a, b, x)
''', "run"),
],
["Эти слова не меняют смысл программы, только скорость. Сначала пишите просто, потом ускоряйте то, что медленно.",
 "Многое компилятор делает сам: «заказ наперёд», горячие и холодные качества obj, склейка текста за один раз, <code>list bool</code> битами."],
"Замерьте <code>time_ms</code> сумму квадратов 10 000 000 чисел обычным <code>each</code> и с <code>by vec</code>.",
["by vec", "by columns", "each piece in … by N", "together"])

lesson("Окно и 2D-рисование", "Открыть окно, рисовать и слушать клавиши и мышь.",
[
("Самая маленькая игра",
 "<code>window_open</code> открывает окно. <code>repeat while window_alive()</code> — кадр за кадром: показывает нарисованное, принимает клавиши и держит частоту экрана. Точка (0, 0) — левый верхний угол, <code>y</code> растёт вниз. Цвет — <code>rgb(красный, зелёный, синий)</code> от 0 до 255.",
'''func main()
    try window_open(400, 300, "Привет")
    x = 200.0
    repeat while window_alive()
        dt = frame_ms() / 1000            // секунды с прошлого кадра
        if key_down("left")
            x -= 200 * dt
        if key_down("right")
            x += 200 * dt
        clear_window(rgb(20, 20, 30))
        draw_rect(0, 250, 400, 50, rgb(60, 140, 80))
        draw_circle(round(x), 230, 20, rgb(47, 209, 191))
        draw_line(0, 0, mouse_x(), mouse_y(), rgba(255, 255, 255, 100))
        draw_text(10, 10, "Стрелки двигают круг", 1, rgb(255, 255, 255))
        if mouse_down()
            draw_pixel(mouse_x(), mouse_y(), rgb(255, 0, 0))
''', "check"),
("Картинки, шрифты, ввод",
 "Картинки PNG, JPEG, GIF и BMP: <code>load_image</code> → номер, <code>draw_image</code>. Шрифты TTF: <code>load_font</code>, <code>draw_font</code>, <code>text_width</code>. Набранные буквы — <code>typed_text()</code>, колёсико — <code>mouse_wheel()</code>, геймпад — <code>pad_down</code> и <code>pad_axis</code>.",
'''func main()
    try window_open(640, 360, "Картинки")
    hero = load_image("hero.png") or -1
    font = load_font("/System/Library/Fonts/Helvetica.ttc") or -1
    name = ""
    repeat while window_alive()
        name += typed_text()
        clear_window(rgb(240, 240, 245))
        if hero >= 0
            draw_image(hero, 20, 20)
        if font >= 0
            draw_font(font, 20, 300, "Имя: " + name, 24, rgb(30, 30, 40))
        if pad_count() > 0 and pad_down(0, "a")
            draw_round(400, 40, 200, 80, 16, "all", rgb(255, 95, 162))
        set_cursor("hand")
''', "check"),
],
["Клавиши: <code>left right up down space enter escape backspace tab shift cmd ctrl alt</code>, <code>f1…f12</code>, буквы и цифры. <code>cmd</code> = Command на Mac и Ctrl на Linux/Windows.",
 "<code>frame_ms()</code> — длина прошлого кадра: умножайте скорость на него, и игра идёт одинаково при любой частоте.",
 "Ещё: <code>window_w/h</code>, <code>window_sharp</code> (Retina), <code>set_fps</code>, <code>fps</code>, <code>clipboard_get/set</code>, <code>blur_rect</code>, <code>draw_shadow</code>, <code>draw_image_alpha</code>, <code>image_scaled</code>, <code>screen_image</code>."],
"Сделайте игру «лови звёзды»: звезда падает сверху в случайном месте, корзина внизу двигается стрелками, считайте очки и выводите их текстом.",
["window_open", "window_alive", "clear_window", "rgb", "rgba", "draw_pixel", "draw_rect", "draw_round", "draw_circle", "draw_line", "draw_triangle", "draw_text", "key_down", "mouse_x", "mouse_y", "mouse_down", "mouse_wheel", "mouse_back", "mouse_forward", "typed_text", "pad_count", "pad_down", "pad_axis", "load_image", "draw_image", "draw_image_alpha", "image_w", "image_h", "image_scaled", "screen_image", "load_font", "draw_font", "text_width", "frame_ms", "fps", "set_fps", "window_w", "window_h", "window_sharp", "window_scale", "set_cursor", "clipboard_get", "clipboard_set", "blur_rect", "draw_shadow"])

lesson("3D", "Сетки из треугольников, камера, свет и картинки на гранях.",
[
("Куб, камера и свет",
 "<code>mesh_new()</code> — пустая сетка, <code>mesh_add</code> добавляет в неё треугольник (x вправо, y вверх, z вперёд). <code>draw_mesh(m, x, y, z, поворот)</code> рисует сетку в мире. <code>camera</code> — где стоит глаз и куда смотрит. <code>sunlight</code> включает солнце. Считает видеокарта.",
'''func quad(m: int, x1: dec, y1: dec, z1: dec, x2: dec, y2: dec, z2: dec, x3: dec, y3: dec, z3: dec, x4: dec, y4: dec, z4: dec, c: int)
    mesh_add(m, x1, y1, z1, x2, y2, z2, x3, y3, z3, c)
    mesh_add(m, x1, y1, z1, x3, y3, z3, x4, y4, z4, c)

func main()
    try window_open(640, 480, "Куб")
    m = mesh_new()
    quad(m, -1.0, -1.0, -1.0, -1.0, 1.0, -1.0, 1.0, 1.0, -1.0, 1.0, -1.0, -1.0, rgb(230, 80, 80))
    quad(m, 1.0, -1.0, 1.0, 1.0, 1.0, 1.0, -1.0, 1.0, 1.0, -1.0, -1.0, 1.0, rgb(80, 200, 120))
    quad(m, -1.0, 1.0, -1.0, -1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, -1.0, rgb(80, 140, 240))
    camera(0, 2, -6, 0, 0, 0)
    sunlight(0.5, 1, -0.3, 0.25)
    turn = 0.0
    repeat while window_alive()
        turn += 60 * frame_ms() / 1000
        clear_window(rgb(15, 18, 28))
        draw_mesh(m, 0, 0, 0, turn)
        draw_mesh_rot(m, 3, 0, 0, turn, 30, 0)
''', "check"),
("Картинка на сетке",
 "<code>mesh_image(m, картинка)</code> рисует сетку картинкой. <code>mesh_add_uv</code> — треугольник, у каждого угла своё место на картинке (u, v от 0 до 1).",
'''func main()
    try window_open(640, 480, "Пол")
    tex = try load_image("grass.png")
    floor = mesh_new()
    mesh_image(floor, tex)
    mesh_add_uv(floor, -5, 0, -5, 0, 0, -5, 0, 5, 0, 4, 5, 0, 5, 4, 4)
    mesh_add_uv(floor, -5, 0, -5, 0, 0, 5, 0, 5, 4, 4, 5, 0, -5, 4, 0)
    camera(0, 3, -8, 0, 0, 0)
    repeat while window_alive()
        clear_window(rgb(150, 200, 255))
        draw_mesh(floor, 0, 0, 0, 0)
''', "check"),
],
["Ближнее закрывает дальнее само (глубина). Задние грани закрытых сеток не рисуются.",
 "<code>TB_GPU_INFO=1</code> — сказать при запуске, чем рисуем; <code>TB_GPU=0</code> — рисовать процессором.",
 "Примеры: <code>examples/basics/cube.tb</code>, игры <code>examples/apps/corridor</code> и <code>football</code>."],
"Постройте из кубов лестницу из 5 ступенек и облетите её камерой по кругу (камера в точке <code>(8·cos, 4, 8·sin)</code>, смотрит в центр).",
["mesh_new", "mesh_add", "mesh_add_uv", "mesh_image", "draw_mesh", "draw_mesh_rot", "camera", "sunlight", "draw_triangle"])

lesson("Звук", "Звуки, музыка и звук в 3D.",
[
("Играть звуки",
 "<code>load_sound</code> загружает WAV и отдаёт номер звука. <code>play_sound</code> играет один раз, <code>loop_sound</code> — по кругу; оба отдают номер голоса, которым потом управляют. <code>play_tone(частота, мс)</code> — простой писк.",
'''func main()
    try window_open(300, 200, "Звук")
    step = try load_sound("step.wav")
    music = try load_sound("music.wav")
    m = loop_sound(music)
    sound_volume(m, 0.4)
    master_volume(0.8)
    repeat while window_alive()
        if key_down("space")
            v = play_sound(step)          // номер голоса
        if key_down("t")
            beep = play_tone(440, 150)
        if key_down("s")
            stop_sound(m)
        clear_window(rgb(30, 30, 40))
''', "check"),
("Звук в мире",
 "<code>play_sound_at(звук, x, y, z)</code> ставит звук в 3D: громкость по расстоянию, сторона, эффект Доплера. <code>listener</code> — где уши (обычно те же числа, что у <code>camera</code>). <code>move_sound</code> двигает звучащий голос.",
'''func main()
    try window_open(300, 200, "3D-звук")
    car = try load_sound("engine.wav")
    v = loop_sound_at(car, -20, 0, 5)
    x = -20.0
    listener(0, 0, 0, 0, 0, 1)
    repeat while window_alive()
        x += 10 * frame_ms() / 1000
        move_sound(v, x, 0, 5)
        sound_pitch(v, 1.0)
        if not sound_playing(v)
            stop
        clear_window(rgb(30, 30, 40))
''', "check"),
],
["Свой микшер: 64 голоса, без щелчков, ограничитель на выходе.", "<code>TB_SOUND_FILE=out.wav</code> — писать звук в файл, <code>TB_SOUND=0</code> — без звука."],
"Добавьте звук в игру «лови звёзды»: писк при поимке, тон пониже при промахе и тихую музыку по кругу.",
["load_sound", "play_sound", "loop_sound", "play_sound_at", "loop_sound_at", "play_tone", "stop_sound", "sound_playing", "sound_volume", "sound_pitch", "move_sound", "master_volume", "listener"])

lesson("Приложения с кнопками: lib/ui", "Собрать окно с кнопками, полями и списками.",
[
("Немедленное рисование",
 "Библиотека <code>ui</code> написана на самом Tachbit. Каждый кадр программа заново зовёт <code>ui.button</code>, и функция сразу рисует кнопку и отвечает, нажали ли её. Хранить кнопки нигде не нужно. <code>hbox</code>/<code>vbox</code> раскладывают элементы без координат.",
'''use ui

func main()
    try window_open(520, 300, "Заметки")
    u = ui.start()
    notes = list text
    draft = ""
    repeat while window_alive()
        ui.frame(u)
        clear_window(u.bg)
        ui.pad(u, 16)
        ui.heading(u, "Мои заметки")
        ui.hbox(u, 32, 8)
        draft = ui.edit(u, "draft", draft, "новая заметка")
        if ui.btn_main(u, "Добавить") and draft != ""
            notes.add(draft)
            draft = ""
        ui.done(u)
        each n in notes
            ui.lbl(u, "• " + n)
        ui.end_frame(u)
''', "lib-check"),
],
["Что есть: кнопки, галочки, поля ввода с выделением и буфером обмена, списки, прокрутка, меню, вкладки, ползунки, подсказки, перетаскивание, окна внутри программы, вопрос «да/нет», тёмная и светлая тема.",
 "Пример большого приложения — файловый менеджер <code>examples/apps/files</code>."],
"Сделайте список дел: поле ввода, кнопка «Добавить», у каждой задачи галочка «сделано» и кнопка «удалить». Сохраняйте список в файл при выходе.",
["use ui", "ui.start", "ui.frame", "ui.end_frame", "ui.button", "ui.btn", "ui.edit", "ui.lbl", "ui.hbox", "ui.vbox", "ui.done", "ui.check", "ui.window"])

lesson("Железо: danger, память руками, shape, zone", "Работать с адресами, байтами и регистрами, как в C.",
[
("danger и память руками",
 "Слова, которыми можно испортить память, работают только внутри блока <code>danger</code>. Так сразу видно опасные места. <code>new_mem</code> берёт блок байт (заполнен нулями), <code>read/write 8/16/32/64</code> читают и пишут числа нужного размера, <code>free_mem</code> отдаёт блок.",
'''func main()
    danger
        buf = try new_mem(64)
        write32(buf, 7)
        write32(buf + 4, 1000)
        fill_mem(buf + 8, 0xFF, 4)
        copy_mem(buf + 16, buf, 8)
        print(read32(buf + 16), read32(buf + 20), read8(buf + 9))
        buf = try grow_mem(buf, 128)
        free_mem(buf)
''', "run"),
("shape: раскладка байтов",
 "<code>shape</code> описывает, какие поля лежат в памяти по порядку и какого они размера. <code>Тип at адрес</code> накладывает раскладку на память, и поля читаются как обычные <code>int</code>. Так описывают регистры устройств, заголовки пакетов и записи на диске.",
'''shape Header
    kind: u8
    flags: bits 4
    ver: bits 4
    length: u16 big
    id: u32

func main()
    print(Header.size)
    danger
        a = try new_mem(Header.size)
        h = Header at a
        h.kind = 2
        h.ver = 3
        h.length = 513
        h.id = 70000
        print(h.kind, h.ver, h.length, h.id, read8(a + 2), read8(a + 3))
        free_mem(a)
''', "run"),
("zone: много кусков, одна уборка",
 "Зона выдаёт куски памяти очень быстро и отдаёт их все сразу: удобно для кадра игры или одного запроса сервера.",
'''func main()
    danger
        z = try zone_new(4096)
        count i from 1 to 100
            p = zone_take(z, 24)
            write64(p, i)
        print(zone_used(z))
        zone_clear(z)
        print(zone_used(z))
        zone_free(z)
''', "run"),
],
["Размеры полей: <code>u8 u16 u32 u64</code>, <code>i8 … i64</code>, <code>bits N</code>, <code>bytes N</code>, <code>big</code> — старший байт первым.",
 "<code>TB_MEM_CHECK=1</code> ловит чтение после <code>free_mem</code>, выход за край и забытые блоки — со строками.",
 "Внутри <code>danger</code> числа при переполнении заворачиваются, как в железе."],
"Опишите <code>shape</code> для записи «товар»: код <code>u32</code>, цена <code>u32</code>, количество <code>u16</code>, флаг «есть на складе» <code>bits 1</code>. Положите 10 записей подряд в одну память и посчитайте сумму.",
["danger", "new_mem", "free_mem", "grow_mem", "copy_mem", "fill_mem", "read8…64", "write8…64", "address_of", "asm", "shape", "at", "u8…u64", "i8…i64", "bits", "bytes", "big", "zone_new", "zone_take", "zone_used", "zone_clear", "zone_free"])

lesson("Связь с C и системой", "Звать библиотеки C, быть библиотекой для C, звать ядро ОС.",
[
("use c",
 "Блок <code>use c</code> описывает функции C, которые хотите звать. Библиотеку называют в кавычках (<code>use c \"sqlite3\"</code>) или берут свой файл <code>.c</code>. Без имени — стандартная библиотека C, вызов через <code>c.</code>. Вызов C — только в <code>danger</code>.",
'''use c
    func abs(x: i32) gives i32
    func strlen(s: text) gives u64

func main()
    danger
        print(c.abs(-42), c.strlen("Tachbit"))
''', "run"),
("for c, tb lib и syscall",
 "Функция с <code>for c</code> видна из C под своим именем; <code>tb lib файл.tb</code> собирает <code>libфайл.a</code> и заголовок <code>файл.h</code>. <code>syscall(номер, …)</code> зовёт ядро Linux или Mac напрямую.",
'''func add(a: int, b: int) for c
    return a + b

func main()
    danger
        pid = syscall(39)          // getpid на Linux x86-64 (на Mac номер другой)
        print(pid > 0)
    print(add(2, 3))
''', "run"),
],
["Типы для C: <code>int i8 i16 i32 i64 u8 u16 u32 u64 dec f32 bool text</code>.", "Текст из C сразу копируется в память Tachbit.",
 "В своей ОС Omnium вместо syscall — каналы к службам."],
"Подключите <code>use c</code> функцию <code>getenv(name: text) gives text</code> и сравните её ответ с <code>env(\"HOME\")</code>.",
["use c", "c.функция", "for c", "tb lib", "syscall", "i32 u64 f32 …"])

lesson("Своё ядро: tb bare", "Программа, которая загружается без операционной системы.",
[
("Ядро на Tachbit",
 "<code>tb bare файл.tb</code> делает 64-битное ядро для x86-64: оно загружается само (QEMU или GRUB), без macOS и Linux. Внутри работают <code>print</code>, списки, obj, <code>background</code> (это настоящие задачи с переключением по таймеру). Функции с <code>on timer</code>, <code>on keyboard</code>, <code>on interrupt N</code> зовёт сам процессор.",
'''shared ticks = int 0

func on_tick() on timer          // 1000 раз в секунду
    ticks += 1

func on_key() on keyboard        // при каждом нажатии
    k = read_key()

func main()
    print("ядро запущено")
    wait_ms(100)
    print("тиков:", ticks, "время:", uptime_ms())
    danger
        write16(0xB8000, 0x0E41)       // жёлтая A в угол экрана
        status = port_in8(0x64)        // порт клавиатуры
        interrupts_on()
''', "none"),
("Запуск",
 "Нужен QEMU. Пример ядра — <code>examples/kernel/main.tb</code>, своя ОС Omnium — папка <code>omnium/</code>. <code>tb app файл.tb</code> собирает программу для Omnium (<code>--own</code> — своим машинным кодом, без LLVM).",
'''brew install qemu                      # Mac, один раз
tb bare kernel.tb                      # → kernel.elf
qemu-system-x86_64 -kernel kernel.elf -serial stdio
./tools/kernel.sh examples/kernel/main.tb --text
./omnium/run.sh                        # своя ОС Omnium
''', "none"),
],
["Только в ядре: <code>port_in/out</code>, <code>page_new/free</code>, <code>interrupts_on/off</code>, <code>uptime_ms</code>, <code>read_key</code>, <code>yield</code>, <code>module_*</code>, <code>kernel_symbol</code>, <code>call_address</code>.",
 "Падение задачи <code>background</code> в ядре роняет только её; <code>a.crashed</code> говорит, упала ли.",
 "Проверено в QEMU; на настоящем компьютере пока не запускалось."],
"Запустите <code>./tools/kernel.sh examples/kernel/main.tb</code> и найдите в коде, где ядро обрабатывает клавиши. Добавьте свою команду, которая печатает время работы ядра.",
["tb bare", "on timer", "on keyboard", "on interrupt N", "read_key", "uptime_ms", "yield", "port_in8…32", "port_out8…32", "page_new", "page_free", "interrupts_on", "interrupts_off", "tb app"])

lesson("Команды tb и настройки", "Всё, что можно сделать из терминала.",
[
("Команды",
 "Короткие команды для проектов и полные для отдельных файлов.",
'''tb new game                  # новый проект: папка game/ с main.tb
tb start game [слова]        # собрать проект и запустить
tb run файл.tb [слова]       # собрать и запустить файл
tb build файл.tb             # программа рядом с файлом
tb check файл.tb             # только найти ошибки
tb c файл.tb                 # показать получившийся C
tb ll файл.tb                # показать LLVM IR
tb bare файл.tb              # ядро без ОС
tb app файл.tb [--own]       # программа для Omnium
tb lib файл.tb               # библиотека для C
tb version                   # какая версия
--llvm / --c                 # к build и run: через LLVM или через C
''', "none"),
("Переменные окружения",
 "Пишутся перед командой: <code>TB_GPU_INFO=1 tb run game.tb</code>.",
'''TB_CORES=1        # все ядра → одно (сравнить скорость)
TB_GC_STATS=1     # печатать уборки памяти;  TB_GC=0 — без уборки
TB_MEM_CHECK=1    # строгая проверка памяти в danger
TB_GPU=0          # рисовать процессором;  TB_GPU_INFO=1 — чем рисуем
TB_FPS_FREE=1     # не ждать экран (замеры)
TB_SOUND=0        # без звука;  TB_SOUND_FILE=out.wav — звук в файл
TB_LIB=папка      # где искать библиотеки для use
TB_UI_FONT=шрифт.ttf   # шрифт для lib/ui
''', "none"),
],
["VS Code: расширение Tachbit подсвечивает код и подчёркивает ошибки при сохранении.",
 "Полный справочник — <code>docs/guide.md</code> в репозитории."],
"Создайте проект <code>tb new hello</code>, запустите его через <code>tb start hello</code>, потом посмотрите <code>tb c hello/main.tb</code>: сравните, во что превратились ваши 5 строк.",
["tb new", "tb start", "tb run", "tb build", "tb check", "tb c", "tb ll", "tb bare", "tb app", "tb lib", "tb version"])
