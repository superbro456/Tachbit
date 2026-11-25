# Уроки 1–14: основа языка.
# part = (заголовок, текст html, код, режим) ; режим: run | check | none | in:<ввод>
L = []


def lesson(title, goal, parts, rules, task, words):
    L.append(dict(title=title, goal=goal, parts=parts, rules=rules, task=task, words=words))


lesson("Первая программа", "Понять, из чего состоит файл .tb и как его запустить.",
[
("Файл и запуск",
 "Программа на Tachbit — это текстовый файл с расширением <code>.tb</code>. Работа начинается с функции <code>main</code>. Всё, что относится к функции, пишется ниже с отступом в 4 пробела. Отступ здесь не украшение, а часть языка: он показывает, где блок начинается и где кончается. Фигурных скобок, <code>then</code> и <code>end</code> в Tachbit нет.",
'''// всё после двух косых черт — комментарий, компьютер его не читает
func main()
    print("Привет, Tachbit!")
    print("2 + 3 =", 2 + 3)       // print пишет через пробел и переводит строку
    show("без ")                 // show — то же, но без перевода строки
    show("перевода")
    print("")
''', "run"),
("Команды терминала",
 "<code>tb run файл.tb</code> собирает программу и сразу запускает. <code>tb check файл.tb</code> только ищет ошибки, ничего не запуская. <code>tb build файл.tb</code> кладёт готовую программу рядом с файлом. Полный список команд — в последнем уроке.",
'''tb run hello.tb        # собрать и запустить
tb check hello.tb      # только проверить
tb build hello.tb      # сделать программу hello рядом с файлом
''', "none"),
],
["Одна программа — одна <code>main</code> в главном файле.", "Отступ — ровно 4 пробела на уровень. Табы лучше не смешивать с пробелами.",
 "Ошибки компилятор объясняет по-русски и с номером строки ещё до запуска."],
"Напишите программу, которая печатает ваше имя, город и год рождения тремя строками, а четвёртой строкой — сколько вам лет в 2026 году (посчитайте вычитанием).",
["func", "main", "print", "show", "//"])

lesson("Переменные и типы", "Научиться хранить данные: числа, дроби, текст и да/нет.",
[
("Четыре основных типа",
 "Переменная — это имя для значения. Тип Tachbit узнаёт сам по первому значению. Типов четыре: <code>int</code> (целое число), <code>dec</code> (дробь), <code>text</code> (текст) и <code>bool</code> (<code>true</code> или <code>false</code>). Тип переменной потом не меняется: если в <code>x</code> лежит число, текст туда положить нельзя.",
'''func main()
    age = 20                  // int
    price = 9.5               // dec
    name = "Алия"             // text
    vip = true                // bool
    print(name, age, price, vip)

    total = int 0             // тип можно написать явно
    rate = dec 0              // дробь, пока 0
    note = text               // пустой текст ""
    const SHOP = "Нектар"     // const — нельзя менять
    print(total, rate, note, SHOP)
''', "run"),
("Арифметика",
 "Плюс, минус, умножение, деление и остаток <code>%</code>. У целых деление отбрасывает дробную часть: <code>7 / 2</code> даёт 3. Нужна дробь — пусть одно из чисел будет <code>dec</code>. Короткие записи <code>+=</code>, <code>-=</code>, <code>*=</code>, <code>/=</code> меняют переменную на месте.",
'''func main()
    print(7 / 2, 7 % 2, 7.0 / 2)
    coins = 10
    coins += 5            // coins = coins + 5
    coins *= 2
    coins -= 1
    print(coins)
    print(2 + 3 * 4, (2 + 3) * 4)    // сначала * и /, потом + и -
''', "run"),
],
["Имя переменной начинается с буквы; занятые слова (<code>if</code>, <code>int</code>, <code>bool</code>…) брать нельзя.",
 "Слишком большое целое (больше 9 223 372 036 854 775 807) — ошибка с номером строки, а не тихая порча числа.",
 "Имя внутри блока не может повторять имя снаружи: так нельзя случайно спрятать переменную."],
"Заведите переменные: цена товара, количество и скидка в процентах. Посчитайте сумму без скидки, размер скидки и итог. Итог посчитайте и в <code>int</code>, и в <code>dec</code> и сравните.",
["int", "dec", "text", "bool", "true", "false", "const", "=", "+= -= *= /=", "%"])

lesson("Сравнения и решения", "Научить программу выбирать, что делать.",
[
("if, else if, else",
 "<code>if</code> проверяет условие. Если оно верно, выполняется блок под ним. <code>else if</code> — следующая проверка, если первая не прошла. <code>else</code> — всё остальное. Сравнения: <code>==</code> равно, <code>!=</code> не равно, <code>&lt; &gt; &lt;= &gt;=</code>.",
'''func grade(score: int)
    if score >= 90
        return "отлично"
    else if score >= 70
        return "хорошо"
    else if score >= 50
        return "сойдёт"
    else
        return "мало"

func main()
    print(grade(95), grade(72), grade(10))
''', "run"),
("and, or, not",
 "Условия соединяются словами: <code>and</code> — оба верны, <code>or</code> — хотя бы одно, <code>not</code> — наоборот. Скобки помогают читать сложные условия.",
'''func main()
    age = 20
    money = 300
    banned = false
    if age >= 18 and money >= 100 and not banned
        print("можно купить")
    if money < 50 or banned
        print("нельзя")
    else
        print("денег хватает и не в бане")
    can = age >= 18 and not banned       // условие можно сохранить в bool
    print(can)
''', "run"),
],
["После <code>if</code> не нужны ни скобки, ни двоеточие, ни <code>then</code>.", "<code>=</code> кладёт значение, <code>==</code> сравнивает.",
 "Текст тоже сравнивается: <code>\"a\" &lt; \"b\"</code>, <code>name == \"Алия\"</code>."],
"Напишите функцию <code>ticket(age: int)</code>, которая возвращает цену билета: до 7 лет — 0, до 18 — 100, от 65 — 80, остальным — 200. Проверьте пять возрастов.",
["if", "else if", "else", "and", "or", "not", "== != < > <= >="])

lesson("Повторение: циклы", "Повторять действия, не копируя код.",
[
("Четыре вида циклов",
 "<code>repeat N times</code> — ровно N раз. <code>repeat while условие</code> — пока условие верно. <code>count i from A to B</code> — счётчик от A до B, оба конца входят. <code>each x in список</code> — по каждому элементу.",
'''func main()
    repeat 3 times
        show("ура ")
    print("")

    n = 1
    repeat while n < 100
        n *= 3
    print("первая степень 3 после 100:", n)

    count i from 1 to 5
        show(i * i, " ")
    print("")

    each fruit in ["яблоко", "груша", "слива"]
        print("-", fruit)
''', "run"),
("skip и stop",
 "<code>skip</code> — сразу к следующему шагу цикла. <code>stop</code> — выйти из цикла совсем.",
'''func main()
    count i from 1 to 20
        if i % 2 == 0
            skip              // чётные пропускаем
        if i > 9
            stop              // дальше 9 не идём
        show(i, " ")
    print("")
''', "run"),
],
["<code>count</code> умеет считать и вниз: <code>count i from 5 to 1</code>.", "<code>each</code> работает со списками, текстом (по буквам), <code>dict</code> (по ключам) и каналами.",
 "Бесконечный цикл: <code>repeat while true</code>, выход из него — <code>stop</code> или <code>return</code>."],
"Выведите таблицу умножения на 7 (от 7×1 до 7×10). Потом найдите первое число больше 1000, которое делится и на 7, и на 13.",
["repeat … times", "repeat while", "count … from … to", "each … in", "skip", "stop"])

lesson("Текст", "Склеивать, резать, искать и менять текст.",
[
("Склейка и буквы",
 "Текст склеивается через <code>+</code>, числа при этом сами становятся текстом. <code>len</code> считает буквы (а не байты), <code>s[0]</code> — первая буква, <code>cut(s, от, до)</code> — кусок, буквы считаются с нуля.",
'''func main()
    name = "Алия"
    hello = "Привет, " + name + "! Тебе " + 20 + " лет."
    print(hello)
    print(len(name), name[0], name[len(name) - 1])
    print(cut("Tachbit", 0, 3))         // буквы с 0-й по 3-ю
    each ch in "кот"
        show("[", ch, "]")
    print("")
''', "run"),
("Поиск и замена",
 "Для работы с текстом есть готовые функции. Все они возвращают новый текст и не меняют старый.",
'''func main()
    s = "  Мама мыла раму  "
    t = trim(s)                         // убрать пробелы по краям
    print("[" + t + "]")
    print(upper(t), lower(t))
    print(find(t, "мыла"), find(t, "кот"))    // номер буквы или -1
    print(replace(t, "раму", "окно"))
    print(starts_with(t, "Мама"), ends_with(t, "!"))
    parts = split("a,b,c", ",")
    print(parts, join(parts, " + "))
''', "run"),
("Числа и коды букв",
 "<code>to_int</code> и <code>to_dec</code> превращают текст в число. Они могут не справиться (в тексте не число), поэтому их зовут с запасным значением через <code>or</code> (подробнее в уроке 12). <code>code</code> даёт номер буквы, <code>char</code> — букву по номеру.",
'''func main()
    a = to_int("42") or 0
    b = to_int("сорок") or -1
    d = to_dec("2.5") or 0.0
    print(a + 1, b, d * 2)
    print(code("A"), char(66), code("п"), char(1087))
''', "run"),
],
["<code>len</code>, <code>s[i]</code>, <code>cut</code>, <code>find</code> считают буквы: <code>len(\"привет\")</code> = 6, смайлик — одна буква.",
 "Байты нужны редко (файлы, сеть): <code>byte_len</code>, <code>byte_at</code>, <code>byte_char</code>.",
 "<code>s += x</code> дописывает в конец быстро, без лишних копий."],
"Пользователь ввёл <code>\"  иванов иван  \"</code>. Сделайте из этого <code>\"Иванов И.\"</code>: уберите пробелы, разрежьте по пробелу, сделайте первую букву заглавной, от имени оставьте одну букву.",
["+", "len", "s[i]", "cut", "find", "replace", "split", "join", "upper", "lower", "trim", "starts_with", "ends_with", "to_int", "to_dec", "code", "char", "byte_len", "byte_at", "byte_char"])

lesson("Списки", "Хранить много значений под одним именем.",
[
("Создать, взять, изменить",
 "Список пишется в квадратных скобках. Номера идут с нуля. Пустой список создаётся с типом: <code>list int</code>, <code>list text</code>. Номер за краем списка останавливает программу с понятной ошибкой.",
'''func main()
    nums = [3, 8, 1]
    print(nums[0], len(nums))
    nums[0] = 5
    nums.add(7)                     // в конец
    last = nums.pop()               // взять и убрать последний
    print(nums, last)

    names = list text               // пустой список текста
    names.add("Вали")
    names.add("Алия")
    sort(names)
    print(names)

    grid = list list int            // список списков
    grid.add([1, 2])
    grid.add([3, 4])
    print(grid[1][0])
    nums.clear()
    print(len(nums))
''', "run"),
("Пройти и собрать итог",
 "Чаще всего по списку идут через <code>each</code>, а нужен номер — через <code>count</code>.",
'''func main()
    scores = [12, 40, 7, 33]
    total = 0
    best = scores[0]
    each s in scores
        total += s
        best = max(best, s)
    print("сумма", total, "лучший", best)
    count i from 0 to len(scores) - 1
        show(i, ":", scores[i], "  ")
    print("")
''', "run"),
],
["Список передаётся в функцию как ярлык: функция меняет тот же список, а не копию.",
 "<code>sort</code> сортирует числа и текст на месте.", "<code>.clear()</code> делает список пустым и сразу отдаёт его память."],
"Есть список оценок. Посчитайте среднее (в <code>dec</code>), сколько оценок выше среднего, и создайте новый список только из оценок не ниже 50.",
["[ ]", "list T", "list list T", ".add", ".pop", ".clear", "len", "sort", "x[i]"])

lesson("Словари: dict", "Находить значение по ключу, как в телефонной книге.",
[
("Ключ и значение",
 "<code>dict</code> хранит пары «ключ → значение». Ключ — <code>text</code> или <code>int</code>. Пустой словарь: <code>dict text int</code> (ключ текст, значение число).",
'''func main()
    phones = {"Алия": "555-12", "Вали": "555-34"}
    print(phones["Алия"])
    phones["Тимур"] = "555-56"          // добавить или заменить
    print(phones.has("Вали"), phones.has("Олег"))
    phones.remove("Вали")
    print(len(phones), phones.keys())
    each name in phones                 // по ключам, в порядке добавления
        print(name, "→", phones[name])
''', "run"),
("Счётчик слов",
 "Самый частый приём: посчитать, сколько раз встретилось каждое слово.",
'''func main()
    text_in = "кот пёс кот мышь кот пёс"
    counts = dict text int
    each w in split(text_in, " ")
        if not counts.has(w)
            counts[w] = 0
        counts[w] += 1
    print(counts)
    print(counts.values())
''', "run"),
],
["Взять ключ, которого нет, — ошибка; сначала проверьте <code>.has(k)</code>.", "Убирать ключи во время <code>each</code> по этому же словарю нельзя: компилятор не даст.",
 "Порядок ключей при печати и <code>each</code> — как добавляли."],
"Сделайте словарь «товар → цена» на 5 товаров и список покупок. Посчитайте сумму покупок; если товара нет в словаре, напечатайте «нет такого: …» и пропустите его.",
["{ }", "dict K V", "d[k]", ".has", ".remove", ".keys", ".values", ".clear", "len", "each k in d"])

lesson("Функции", "Разбить программу на кусочки с именами.",
[
("Параметры и return",
 "У каждого параметра пишется тип. <code>return</code> отдаёт ответ и сразу выходит из функции. Тип ответа Tachbit узнаёт сам. При вызове можно назвать параметры по имени: тогда порядок не важен и код читается лучше.",
'''func area(w: int, h: int)
    return w * h

func greet(name: text, n: int)
    repeat n times
        print("привет,", name)

func main()
    print(area(3, 4))
    print(area(h: 10, w: 2))         // по именам — в любом порядке
    greet("Алия", 2)
''', "run"),
("Несколько ответов: gives",
 "Если ответов несколько, их называют после <code>gives</code>. Внутри функции они — обычные переменные. Принять их можно сразу в несколько имён, в ячейки списка или в поля obj (<code>p.x, p.y = f()</code>).",
'''func divide(a: int, b: int) gives whole: int, rest: int
    whole = a / b
    rest = a % b

func main()
    q, r = divide(17, 5)
    print("17 = 5 *", q, "+", r)
    res = [0, 0]
    res[0], res[1] = divide(9, 4)     // ответы сразу в ячейки списка
    print(res)
''', "run"),
("Функция зовёт сама себя",
 "Это называется рекурсия. Нужна остановка, иначе функция будет звать себя вечно; Tachbit тогда скажет «стек переполнен».",
'''func fact(n: int)
    if n <= 1
        return 1
    return n * fact(n - 1)

func main()
    print(fact(10))
''', "run"),
],
["Один ответ — <code>return</code>; несколько — <code>gives</code>; не получилось — <code>fail</code> (урок 12).",
 "Списки и obj приходят в функцию как ярлыки: изменения видны снаружи. Числа и текст копируются.",
 "Порядок функций в файле не важен: <code>main</code> может стоять и первой."],
"Напишите <code>min_max(xs: list int) gives lo: int, hi: int</code> и <code>is_prime(n: int)</code>. Выведите все простые числа до 100 и самое маленькое и большое из них.",
["func", "return", "gives", "имя: тип", "вызов по имени"])

lesson("Свои типы: obj", "Собрать несколько полей в одну вещь.",
[
("Описать и создать",
 "<code>obj</code> описывает свой тип: какие у него качества и каких они типов. Создают его по имени типа, качества называют по имени. Пропущенные качества будут 0, пустым текстом или пустым списком.",
'''obj User
    name: text
    age: int
    tags: list text

func main()
    u = User(name: "Алия", age: 20)
    u.age += 1
    u.tags.add("админ")
    print(u.name, u.age, u.tags)
    print(u)                          // obj печатается целиком

    team = list User
    team.add(u)
    team.add(User(name: "Вали", age: 17))
    each p in team
        if p.age >= 18
            print(p.name, "взрослый")
''', "run"),
("obj — это ярлык",
 "Переменная с obj указывает на сам объект. Если отдать его в функцию или положить во второе имя, меняется тот же объект.",
'''obj Hero
    hp: int

func hit(h: Hero, dmg: int)
    h.hp -= dmg

func main()
    a = Hero(hp: 100)
    b = a                 // b и a — один и тот же герой
    hit(b, 30)
    print(a.hp)
''', "run"),
],
["Тип объекта пишется с большой буквы (так принято).", "obj можно класть в списки и dict, а качество может быть списком или другим obj.",
 "Пустой (не созданный) объект при обращении останавливает программу с понятной ошибкой."],
"Сделайте <code>obj Item</code> (название, цена, количество) и <code>obj Cart</code> со списком товаров. Напишите функции <code>add(cart, item)</code> и <code>total(cart)</code>.",
["obj", "Тип(поле: …)", "x.поле", "list Тип"])

lesson("Ярлыки: link", "Менять чужую переменную через другое имя.",
[
("link на качество или элемент",
 "<code>link</code> — это ярлык: другое имя для того же места в памяти. Изменили ярлык — изменилось само место. Удобно, когда до значения длинный путь.",
'''obj Hero
    hp: int
    gold: int

func main()
    h = Hero(hp: 100, gold: 5)
    life: link h.hp
    life -= 30
    life += 5
    print(h.hp)

    xs = [1, 2, 3]
    first: link xs[0]
    first = 99
    print(xs)
''', "run"),
("link в параметре",
 "Если функция должна поменять число вызывающего, параметр делают ярлыком.",
'''func heal(link hp, amount: int)
    hp += amount
    if hp > 100
        hp = 100

func main()
    hp = 70
    heal(hp, 50)
    print(hp)
''', "run"),
],
["Пока жив ярлык на элемент списка, сам список нельзя растить (<code>.add</code>): память списка могла бы переехать. Компилятор это проверяет.",
 "Ярлык на один элемент <code>list bool</code> взять нельзя: там биты, у бита нет адреса."],
"Напишите <code>swap(link a, link b)</code>, которая меняет два числа местами, и проверьте на двух переменных и на двух элементах списка.",
["link", "имя: link место", "func f(link x, …)"])

lesson("Варианты: enum и match", "Описать вещь, которая бывает в одном из нескольких состояний.",
[
("enum",
 "<code>enum</code> перечисляет все варианты. Значение пишется как <code>Тип.вариант</code>. <code>print</code> печатает имя варианта.",
'''enum Status
    new
    paid
    sent

func main()
    s = Status.new
    print(s)
    s = Status.paid
    if s == Status.paid
        print("оплачен")
''', "run"),
("match",
 "<code>match</code> разбирает значение по вариантам: у каждого свой блок. Несколько вариантов можно перечислить через запятую. Если вариант забыт, компилятор назовёт его по имени. <code>match</code> работает и с числами, и с текстом; там нужен <code>else</code>.",
'''enum Status
    new
    paid
    sent

func label(s: Status)
    match s
        new
            return "новый"
        paid, sent
            return "в работе"
    return ""

func day_name(n: int)
    match n
        6, 7
            return "выходной"
        else
            return "будний"

func main()
    print(label(Status.new), label(Status.sent))
    print(day_name(3), day_name(7))
''', "run"),
],
["<code>match</code> по <code>enum</code> должен разобрать все варианты или иметь <code>else</code>.", "<code>enum</code> можно класть в obj, списки и посылать по каналам."],
"Сделайте <code>enum Light</code> (red, yellow, green) и функцию <code>next(l: Light)</code>, которая возвращает следующий цвет светофора. Прогоните светофор 6 шагов.",
["enum", "Тип.вариант", "match", "else"])

lesson("Ошибки: fail, try, or, else err", "Сообщать о неудаче и решать, что делать с ней.",
[
("fail и три способа поймать",
 "<code>fail \"причина\"</code> заканчивает функцию ошибкой. Такую функцию нельзя вызвать просто так: компилятор попросит решить, что делать при ошибке. <code>or значение</code> — подставить запасное. <code>else err</code> — свой блок, в <code>err</code> лежит текст причины. <code>try</code> — передать ошибку выше (в <code>main</code> программа остановится и напечатает причину).",
'''func parse_age(s: text)
    n = to_int(s) or -1
    if n < 0
        fail "не число: " + s
    if n > 150
        fail "слишком много: " + s
    return n

func main()
    a = parse_age("20") or 0
    b = parse_age("сто") or 0            // запасное значение
    print(a, b)
    c = parse_age("200") else err
        print("ошибка:", err)
        c = 0
    print(c)
''', "run"),
("try внутри функции",
 "<code>try</code> полезен, когда функция сама не знает, что делать с ошибкой, и отдаёт её тому, кто её вызвал.",
'''func parse_age(s: text)
    n = to_int(s) or -1
    if n < 0
        fail "не число: " + s
    return n

func sum_ages(a: text, b: text)
    x = try parse_age(a)       // сломалось — sum_ages тоже ломается
    y = try parse_age(b)
    return x + y

func main()
    print(sum_ages("20", "30") or -1)
    r = sum_ages("20", "abc") else err
        print("не вышло:", err)
        return
    print(r)
''', "run"),
],
["Сломаться могут: <code>read_file</code>, <code>write_file</code>, <code>input</code>, <code>to_int</code>, <code>to_dec</code>, <code>window_open</code>, <code>load_image</code>, сеть и ваши функции с <code>fail</code> или <code>try</code>.",
 "<code>try</code> можно и без присваивания: <code>try write_file(\"a.txt\", s)</code>.",
 "Деление на ноль, номер за краем списка, переполнение — не <code>fail</code>, а авария: программа останавливается с номером строки."],
"Напишите <code>withdraw(balance: int, amount: int)</code>: ошибка, если сумма ≤ 0 или больше баланса, иначе новый баланс. Вызовите её тремя способами: с <code>or</code>, с <code>else err</code> и из другой функции через <code>try</code>.",
["fail", "try", "or", "else err", "err"])

lesson("Модули и библиотеки", "Разложить программу по файлам и брать готовое.",
[
("Свой модуль",
 "<code>use money</code> подключает файл <code>money.tb</code> из той же папки. Функции модуля зовут через его имя. <code>modpub</code> делает функцию видимой другим файлам; без него (или с <code>modpri</code>) функция видна только своему файлу.",
'''// файл money.tb
modpub func with_tax(price: int)
    return price + fee(price)

func fee(price: int)          // видна только внутри money.tb
    return price * 12 / 100

// файл main.tb
use money

func main()
    print(money.with_tax(1000))
''', "none"),
("Готовые библиотеки",
 "С Tachbit идут библиотеки в папке <code>lib/</code>: <code>ui</code> (кнопки, поля, списки, окна внутри программы), <code>physics</code> (тела, столкновения, лучи), <code>anim</code> (плавность, ключи, кости). <code>use</code> ищет файл сначала рядом с программой, потом в папке <code>TB_LIB</code>, потом в <code>~/.tachbit/lib</code>.",
'''use anim

func main()
    count i from 0 to 4
        t = i * 0.25
        print(t, anim.ease("inout", t))
''', "lib"),
],
["В модуле не нужна <code>main</code>; <code>tb check</code> модуля проверит его функции.", "Имя модуля = имя файла без <code>.tb</code>.",
 "Новые возможности после заморозки ядра 1.2 появляются именно как библиотеки."],
"Вынесите функции корзины из урока 9 в файл <code>cart.tb</code> и подключите его из <code>main.tb</code>. Сделайте одну функцию <code>modpri</code> и проверьте, что из <code>main</code> её не видно.",
["use", "modpub", "modpri", "модуль.функция", "lib/ui", "lib/physics", "lib/anim"])

lesson("Числа, время и случайность", "Готовые функции для чисел и времени.",
[
("Числа",
 "Округлить, модуль, меньшее и большее, корень. <code>min</code> и <code>max</code> работают с двумя числами. Для игр и графики есть углы и степени: <code>sin cos tan asin acos atan atan2</code> (углы в радианах, <code>pi()</code> = 3.14159…), <code>pow</code> (степень), <code>exp</code>, <code>log</code>, <code>floor</code> (вниз до целого), <code>ceil</code> (вверх).",
'''func main()
    print(round(2.6), round(2.4), abs(-7), min(3, 9), max(3, 9), sqrt(16.0))
    print(floor(2.7), ceil(2.1), pow(2.0, 10.0), round(pi() * 100))
    print(round(sin(pi() / 2)), round(cos(0.0)), round(atan2(1.0, 1.0) * 180 / pi()))
    print(bit_and(12, 10), bit_or(12, 10), bit_xor(12, 10), shift_left(1, 4), 0xFF)
''', "run"),
("Время и случайность",
 "<code>time_ms()</code> — миллисекунды от старта (для замеров), <code>wait_ms</code> — подождать, <code>date_now()</code> — дата и время текстом, <code>unix_time()</code> — секунды с 1970 года. <code>random(a, b)</code> — случайное целое, оба конца входят.",
'''func main()
    t0 = time_ms()
    wait_ms(50)
    spent = time_ms() - t0
    print(spent >= 50)
    d = random(1, 6)
    print(d >= 1 and d <= 6)
    print(len(date_now()), unix_time() > 1700000000)
''', "run"),
],
["<code>round</code> отдаёт <code>int</code>. Дробь из целого: умножьте на <code>1.0</code>.", "<code>0x</code>-числа и битовые функции работают везде, не только в <code>danger</code>."],
"Сделайте игру «угадай число»: программа загадывает <code>random(1, 100)</code>, вы вводите числа (<code>input()</code>, урок 15), она отвечает «больше» или «меньше» и в конце говорит число попыток и сколько секунд прошло.",
["round", "abs", "min", "max", "sqrt", "sin", "cos", "tan", "asin", "acos", "atan", "atan2", "pow", "exp", "log", "floor", "ceil", "pi", "random", "time_ms", "wait_ms", "date_now", "unix_time", "bit_and", "bit_or", "bit_xor", "bit_not", "shift_left", "shift_right", "0x"])
