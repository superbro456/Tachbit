C = {}
C[1] = '''// день 1: программа = данные + шаги
func main()
    coins = 0            // состояние: что программа помнит
    lives = 3
    coins += 10          // шаг меняет состояние
    lives -= 1
    print("монеты:", coins, " жизни:", lives)
'''
C[2] = '''// день 2: решения и «охранник»
func can_buy(money: int, price: int, age: int) gives ok: bool
    ok = false
    if age < 18
        return           // охранник: плохой случай уходит сразу
    if money < price
        return
    ok = true

func main()
    print(can_buy(500, 120, 20))
    print(can_buy(100, 120, 20))
    print(can_buy(500, 120, 15))
'''
C[3] = '''// день 3: повторение и «накопитель»
func main()
    scores = [12, 40, 7, 33, 25]
    total = 0
    best = 0
    many = 0
    each s in scores
        total += s               // сумма
        best = max(best, s)      // лучший
        if s >= 25
            many += 1            // счётчик по условию
    print("сумма", total, " лучший", best, " от 25 и выше:", many)
'''
C[4] = '''// день 4: фильтр, преобразование, свёртка
func main()
    prices = [120, 900, 45, 300, 1500]
    cheap = list int
    each p in prices
        if p < 500
            cheap.add(p)                 // фильтр: оставить нужные
    with_tax = list int
    each p in cheap
        with_tax.add(p + p * 12 / 100)   // преобразование: каждый по правилу
    sum = 0
    each p in with_tax
        sum += p                         // свёртка: много в одно
    print(cheap, with_tax, sum)
'''
C[5] = '''// день 5: модель — свой тип для вещи из жизни
obj Player
    name: text
    hp: int
    items: list text

func main()
    p = Player(name: "Нази", hp: 100)
    p.items.add("меч")
    p.items.add("зелье")
    p.hp -= 30
    print(p.name, "здоровье", p.hp, "вещи", p.items)
'''
C[6] = '''// день 6: функции — вход, выход, без сюрпризов
func area(w: int, h: int)
    return w * h

func price_of(w: int, h: int, per_meter: int)
    return area(w, h) * per_meter

func main()
    print("ковёр 3×4 стоит", price_of(3, 4, 250))
'''
C[7] = '''// день 7: ошибка — это тоже ответ
func parse_age(s: text)
    n = try to_int(s)
    if n < 0 or n > 150
        fail "возраст вне границ: " + n
    return n

func main()
    a = parse_age("25") or -1
    print("возраст", a)
    b = parse_age("300") else err
        print("не вышло:", err)
    c = parse_age("abc") or -1
    print("запасное значение:", c)
'''
C[8] = '''// день 8: справочник — найти по ключу сразу
func main()
    stock = dict text int
    stock["чай"] = 10
    stock["кофе"] = 4
    stock["чай"] -= 3
    if stock.has("сок")
        print("сок есть")
    else
        print("сока нет")
    each name in stock
        print(name, stock[name])
'''
C[9] = '''// день 9: сохранение — загрузить, изменить, записать
func main()
    path = "notes.txt"
    old = read_file(path) or ""
    lines = list text
    if old != ""
        lines = split(old, "\\n")
    lines.add("заметка " + (len(lines) + 1))
    write_file(path, join(lines, "\\n")) else err
        print("не сохранил:", err)
        return
    print("в файле заметок:", len(lines))
'''
C[10] = '''// день 10: слои — данные, логика, вывод отдельно
// файл shop_logic.tb рядом:  modpub func total(prices: list int) ...
obj Order
    items: list int

func total(o: Order)                     // логика: только считает
    sum = 0
    each p in o.items
        sum += p
    return sum

func print_order(o: Order)                      // вывод: только показывает
    print("товаров", len(o.items), "на", total(o), "сом")

func main()
    o = Order()                          // данные
    o.items.add(120)
    o.items.add(300)
    print_order(o)
'''
C[11] = '''// день 11: цикл событий — ждём, разбираем, отвечаем
func main()
    count_ok = 0
    repeat while true
        line = input() else err
            stop                          // ввод закончился
        cmd = trim(line)
        if cmd == "выход"
            stop
        else if cmd == "привет"
            print("и тебе привет")
            count_ok += 1
        else
            print("не знаю команду:", cmd)
    print("понял команд:", count_ok)
'''
C[12] = '''// день 12: состояние отдельно, рисование отдельно
use ui

obj Model
    clicks: int

func main()
    try window_open(420, 240, "Счётчик")
    u = ui.start()
    m = Model()
    repeat while window_alive()
        ui.frame(u)
        // 1) событие меняет состояние
        if ui.button(u, 40, 120, 160, 44, "Нажми")
            m.clicks += 1
        if ui.button(u, 220, 120, 160, 44, "Сброс")
            m.clicks = 0
        // 2) рисование только читает состояние
        ui.big_text(u, 40, 40, "нажато: " + m.clicks, 3, rgb(240, 240, 240))
        ui.end_frame(u)
'''
C[13] = '''// день 13: конечный автомат — в каком режиме и куда можно перейти
enum Door
    closed
    open
    locked

func next(d: Door, action: text) gives n: Door
    n = d
    match d
        closed
            if action == "открыть"
                n = Door.open
            else if action == "запереть"
                n = Door.locked
        open
            if action == "закрыть"
                n = Door.closed
        locked
            if action == "отпереть"
                n = Door.closed

func main()
    d = Door.closed
    each a in ["открыть", "запереть", "закрыть", "запереть", "открыть", "отпереть"]
        d = next(d, a)
        print(a, "→", d)
'''
C[14] = '''// день 14: команды и отмена
obj Cmd
    kind: text      // "add" или "del"
    value: text

func do_cmd(doc: list text, c: Cmd)
    if c.kind == "add"
        doc.add(c.value)
    else
        last = doc.pop()

func undo(doc: list text, c: Cmd)
    if c.kind == "add"
        last = doc.pop()
    else
        doc.add(c.value)

func main()
    doc = list text
    done = list Cmd
    each w in ["раз", "два", "три"]
        c = Cmd(kind: "add", value: w)
        do_cmd(doc, c)
        done.add(c)
    print("было:", doc)
    undo(doc, done.pop())                 // Ctrl+Z
    undo(doc, done.pop())
    print("после двух отмен:", doc)
'''
C[15] = '''// день 15: игровой цикл — ввод, обновить, нарисовать
func main()
    try window_open(640, 360, "Игровой цикл")
    set_fps(60)
    x = 320.0
    speed = 200.0                       // точек в секунду
    repeat while window_alive()
        dt = frame_ms() / 1000          // 1) сколько прошло
        if key_down("left")             // 2) ввод
            x -= speed * dt
        if key_down("right")
            x += speed * dt
        x = max(20.0, min(620.0, x))    // 3) обновить и не выпустить за край
        clear_window(rgb(20, 24, 36))   // 4) нарисовать всё заново
        draw_circle(round(x), 180, 20, rgb(255, 190, 60))
'''
C[16] = '''// день 16: позиция, скорость, ускорение
func main()
    y = 0.0          // высота мяча
    vy = 12.0        // скорость вверх
    g = -9.8         // тяжесть меняет скорость
    dt = 0.1
    t = 0.0
    repeat while y >= 0.0
        vy += g * dt         // ускорение меняет скорость
        y += vy * dt         // скорость меняет позицию
        t += dt
    print("мяч упал через", round(t * 10) / 10.0, "с")
'''
C[17] = '''// день 17: столкновения — две коробки пересекаются?
obj Box
    x: dec
    y: dec
    w: dec
    h: dec

func hit(a: Box, b: Box) gives yes: bool
    yes = a.x < b.x + b.w and b.x < a.x + a.w and a.y < b.y + b.h and b.y < a.y + a.h

func main()
    hero = Box(x: 10, y: 10, w: 20, h: 30)
    wall = Box(x: 25, y: 0, w: 10, h: 100)
    coin = Box(x: 80, y: 15, w: 8, h: 8)
    print("герой и стена:", hit(hero, wall))
    print("герой и монета:", hit(hero, coin))
'''
C[18] = '''// день 18: много сущностей — список, обновить всех, убрать мёртвых
obj Enemy
    x: dec
    hp: int

func main()
    enemies = list Enemy
    count i from 1 to 5
        enemies.add(Enemy(x: i * 10.0, hp: i))
    // выстрел по всем: каждый теряет 2 жизни
    each e in enemies
        e.hp -= 2
    // убрать мёртвых: собрать новый список живых
    alive = list Enemy
    each e in enemies
        if e.hp > 0
            alive.add(e)
    enemies = alive
    print("осталось врагов:", len(enemies))
'''
C[19] = '''// день 19: экраны игры — тот же автомат, что и дверь
enum Screen
    menu
    play
    pause
    over

func main()
    s = Screen.menu
    lives = 1
    each key in ["enter", "p", "p", "удар", "enter"]
        match s
            menu
                if key == "enter"
                    s = Screen.play
            play
                if key == "p"
                    s = Screen.pause
                else if key == "удар"
                    lives -= 1
                    if lives == 0
                        s = Screen.over
            pause
                if key == "p"
                    s = Screen.play
            over
                if key == "enter"
                    s = Screen.menu
        print(key, "→", s)
'''
C[20] = '''// день 20: плавность — от «было» к «стало» за время
use anim

func main()
    from_x = 0.0
    to_x = 100.0
    count step from 0 to 5
        t = step / 5.0                          // 0 … 1 — доля пути
        a = anim.lerp(from_x, to_x, t)          // ровно
        b = anim.lerp(from_x, to_x, anim.ease("out", t))   // быстро, потом мягко
        print("t =", t, " ровно", round(a), " мягко", round(b))
'''
C[21] = '''// день 21: события — кто-то сообщил, другие отреагировали
obj Event
    kind: text
    power: int

func main()
    events = list Event                 // очередь событий кадра
    events.add(Event(kind: "удар", power: 7))
    events.add(Event(kind: "монета", power: 1))
    events.add(Event(kind: "удар", power: 2))
    score = 0
    each e in events                    // каждый слушатель смотрит своё
        if e.kind == "удар" and e.power > 5
            print("звук: громкий удар")
        if e.kind == "монета"
            score += 10
            print("звук: дзынь")
    print("очки:", score)
'''
C[22] = '''// день 22: простой ИИ — смотри, реши, действуй
func decide(my_x: dec, ball_x: dec, dist_goal: dec) gives act: text
    if abs(ball_x - my_x) > 1.0
        act = "бежать к мячу"           // далеко — догнать
    else if dist_goal < 10.0
        act = "бить по воротам"         // близко к воротам — бить
    else
        act = "вести мяч"               // иначе вести дальше

func main()
    print(decide(0.0, 5.0, 20.0))
    print(decide(4.5, 5.0, 20.0))
    print(decide(4.5, 5.0, 6.0))
'''
C[23] = '''// день 23: ОС — службы и сообщения вместо общих переменных
func clock_service(asks: channel text, answers: channel text)
    each q in asks                      // служба ждёт вопрос
        if q == "время"
            answers.send(date_now())
        else
            answers.send("не знаю: " + q)

func main()
    asks = channel text
    answers = channel text
    w = background clock_service(asks, answers)
    asks.send("время")
    print("ответ службы:", answers.receive())
    asks.close()
'''
C[24] = '''// день 24: память — кто владеет и когда отдать
func main()
    temp                                    // всё, что создано тут, уйдёт в конце блока
        words = split("раз два три", " ")
        print("слов:", len(words))
    danger                                  // руками: взял — отдай
        buf = new_mem(64) else err
            print(err)
            return
        write32(buf, 42)
        print("в памяти:", read32(buf))
        free_mem(buf)
'''
C[25] = '''// день 25: параллельно — каждый своё, общее под замком
shared done = int 0

func work(a: int, b: int)
    sum = 0
    count i from a to b
        sum += i
    lock done
        done += 1                         // общее меняем только под замком
    return sum

func main()
    a = background work(1, 500000)
    b = background work(500001, 1000000)
    print("сумма:", a.wait + b.wait, " работ закончено:", done)
'''
C[26] = '''// день 26: производитель и потребитель
func cook(orders: channel int, ready: channel text)
    each n in orders
        ready.send("блюдо №" + n)
    ready.close()

func main()
    orders = channel int
    ready = channel text
    k = background cook(orders, ready)
    count i from 1 to 3
        orders.send(i)                     // производитель кладёт
    orders.close()
    each dish in ready                     // потребитель забирает
        print(dish)
'''
C[27] = '''// день 27: кольцевой буфер — как драйвер копит нажатия
obj Ring
    data: list int
    head: int      // откуда читать
    tail: int      // куда писать
    used: int

func put(r: Ring, x: int) gives ok: bool
    ok = r.used < len(r.data)
    if not ok
        return                             // полно: новое теряем, старое не портим
    r.data[r.tail] = x
    r.tail = (r.tail + 1) % len(r.data)
    r.used += 1

func take(r: Ring) gives x: int
    x = r.data[r.head]
    r.head = (r.head + 1) % len(r.data)
    r.used -= 1

func main()
    r = Ring(data: [0, 0, 0, 0])
    each key in [65, 66, 67, 68, 69]       // 5 нажатий в буфер на 4
        if not put(r, key)
            print("буфер полон, потеряно:", char(key))
    repeat while r.used > 0
        print("прочитал:", char(take(r)))
'''
C[28] = '''// день 28: планировщик — всем по очереди понемногу
obj Task
    name: text
    left: int      // сколько ещё работы

func main()
    queue = list Task
    queue.add(Task(name: "музыка", left: 3))
    queue.add(Task(name: "браузер", left: 5))
    queue.add(Task(name: "часы", left: 1))
    slice = 2                               // квант: сколько даём за раз
    tick = 0
    repeat while len(queue) > 0
        t = queue[0]
        rest = list Task
        count i from 1 to len(queue) - 1
            rest.add(queue[i])
        work = min(slice, t.left)
        t.left -= work
        tick += work
        print("такт", tick, ":", t.name, "сделал", work)
        if t.left > 0
            rest.add(t)                     // не закончил — в конец очереди
        else
            print("   ", t.name, "готов")
        queue = rest
'''
C[29] = '''// день 29: права — ключ проверяют на входе
obj Key
    owner: text
    can: list text

func allowed(k: Key, action: text) gives ok: bool
    ok = false
    each c in k.can
        if c == action
            ok = true

func open_camera(k: Key)
    if not allowed(k, "камера")
        fail k.owner + ": нет ключа на камеру"
    print(k.owner, "снимает")

func main()
    chat = Key(owner: "чат", can: ["сеть", "камера"])
    game = Key(owner: "игра", can: ["звук"])
    try open_camera(chat)
    open_camera(game) else err
        print("отказано:", err)
'''
C[30] = '''// день 30: общий скелет — состояние, события, цикл, слои
enum Mode
    work
    done

obj App
    mode: Mode
    todo: list text

func handle(a: App, ev: text)            // события меняют состояние
    if ev == "готово" and len(a.todo) > 0
        last = a.todo.pop()
    if len(a.todo) == 0
        a.mode = Mode.done

func draw(a: App)                        // вывод только читает
    print("режим:", a.mode, " дел:", len(a.todo))

func main()
    a = App(mode: Mode.work, todo: ["код", "тест", "отправить"])
    each ev in ["готово", "готово", "готово"]   // цикл событий
        handle(a, ev)
        draw(a)
'''
import os
os.makedirs('code', exist_ok=True)
for k, v in C.items():
    open(f'code/day{k:02d}.tb', 'w').write(v)
