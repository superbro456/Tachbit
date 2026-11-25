# Файлы .tbs — разметка и стили окна

В файле `.tbs` написано, **что** стоит в окне и **как выглядит**, а **что делает** программа — в `.tb`.
Читает `.tbs` библиотека `lib/view.tb` (`use view`), рисует `lib/ui.tb`. Сам язык для этого не менялся.

Вещь пишется как `obj`: вид, имя и место в скобках. Ниже, с отступом, идут свойства «ключ: значение».

```
btn save (A1:B2)
    text: Сохранить
    color: white
    backcolor: #3ad6c1
    corner.radius: 12
    corner.color: accent
```

## Где стоит вещь — в скобках

| Скобки | Что |
|---|---|
| `(B2)`, `(A1:C2)` | клетки сетки, как в Excel: столбцы `A B C ...` (до `Z`), строки `1 2 3 ...` |
| `(40, 120)` | точки от левого и верхнего края; размер — по содержимому |
| `(40, 120, 200, 36)` | точки и размер: x, y, ширина, высота |
| `(-20, -20)` | минус — от правого и нижнего края (кнопка в углу остаётся в углу, когда окно тянут) |
| `(50%, 10, 40%, 36)` | проценты — доля окна |
| `()` или без скобок | вещь сама встаёт в следующую свободную клетку: слева направо, сверху вниз |

## Сетка экрана

```
screen main 4x7          // экран main: 4 столбца, 7 строк
    cols: 200 * * 120    // ширина столбцов: число точек или * (делят остаток поровну); не написали — все *
    rows: 56 40 * * 52   // высота строк так же
    space: roomy         // расстояния: tight, normal (так по умолчанию), roomy
```

Если нужно точно: `gap: 12` (между клетками) и `pad: 20` (от края окна).

**Окно растягивают.** Клетки `*` растут и сжимаются вместе с окном, а места по точкам считаются заново в каждом кадре.
Если окно стало меньше, чем точные размеры `cols` и `rows` вместе, они сжимаются в одну долю, и за край ничего не уходит.
Длинный текст в узкой клетке кончается на «..». Вещь, которой совсем негде встать, просто не рисуется.

## Вещи

| Вид | Что | Свои свойства |
|---|---|---|
| `title` | крупный заголовок | `text` |
| `heading` | заголовок раздела | `text` |
| `subtitle` | подзаголовок | `text` |
| `text` | обычный текст | `text` |
| `note` | бледная пометка | `text` |
| `caps` | маленькая подпись ЗАГЛАВНЫМИ над полем | `text` |
| `code` | код моноширинным шрифтом | `text` |
| `chip` | цветная метка | `text` |
| `btn имя` | кнопка | `text`, `look: main / second / soft / ghost / danger` |
| `link имя` | текст-ссылка | `text` |
| `icon имя` | круглая кнопка-значок | `text: ×` |
| `field имя` | поле ввода | `hint` (подсказка), `value` (что написано сначала) |
| `area имя` | многострочное поле | `value` |
| `switch имя` | переключатель-«пилюля» | `text`, `on: yes` |
| `check имя` | галочка | `text`, `on: yes` |
| `slider имя` | ползунок | `min`, `max`, `value` |
| `pick имя` | выбор из списка | `items: Один \| Два \| Три`, `value: 1` (выбран второй) |
| `tabs имя` | вкладки | `items`, `value` |
| `card` | карточка-фон: вещи в её клетках отходят от краёв | `padding` |
| `line` | черта | |
| `image` | картинка | `src`, `fit` |

Имя нужно вещам, на которые нажимают или в которые пишут: по нему программа узнаёт нажатие и текст.

## Общие свойства (как в CSS)

Можно писать наши имена или имена из CSS: справа — то же самое по-CSS-ному. Размеры — в точках (`12` или `12px`) или в процентах.

| Свойство | CSS | Что |
|---|---|---|
| `text` | текст тега | надпись |
| `color` | `color` | цвет текста |
| `backcolor` | `background`, `background-color` | цвет фона |
| `corner.radius` | `border-radius` | скругление краёв |
| `corner.color` | `border-color` | цвет рамки |
| `corner.width` | `border-width` | толщина рамки (0 — без рамки) |
| `width`, `height` | `width`, `height` | своя ширина и высота (точки или % места) |
| `min.width`, `max.width`, `min.height`, `max.height` | `min-width` ... | не меньше и не больше |
| `scale` | `transform: scale` | вся вещь крупнее или мельче: `1.5` или `150%` |
| `margin` | `margin` | отступ снаружи: `8` или `8 16` (сверху-снизу, слева-справа) |
| `padding` | `padding` | отступ внутри (текст от краёв; у `card` — вещи от краёв карточки) |
| `size` | `font-size` | размер букв |
| `font` | `font-weight`, `font-family` | `bold`, `regular`, `mono` |
| `align` | `text-align` | `left`, `center`, `right` |
| `spacing` | `letter-spacing` | разрядка между буквами |
| `line.height` | `line-height` | высота строки: `1.5` или `150%` |
| `transform` | `text-transform` | `upper`, `lower`, `capitalize` |
| `decoration` | `text-decoration` | `underline`, `line-through` |
| `wrap` | `white-space` | `yes` — длинный текст переносится по словам (абзац) |
| `opacity` | `opacity` | прозрачность: `0.5` или `50%` |
| `shadow` | `box-shadow` | тень: `none`, `small`, `medium`, `big` |
| `visible` | `display: none`, `visibility: hidden` | `no` — вещь спрятана |
| `layer` | `z-index` | слой: больше — выше |
| `cursor` | `cursor` | курсор над вещью: `hand`, `text`, `move` |
| `tip` | `title` | подсказка, если задержать мышь |
| `disabled` | `disabled` | `yes` — кнопку не нажать (серая) |
| `hover.color`, `hover.backcolor`, `hover.corner.color` ... | `:hover { ... }` | любое свойство с `hover.` — когда мышь над вещью |
| `look` | — | вид кнопки: `main`, `second`, `soft`, `ghost`, `danger` |
| `accent` | `accent-color` | цветное у поля, ползунка, переключателя |
| `style` | `class` | взять свойства из стиля |

Цвета: имена темы — `accent ink dim red panel bg raised field line faint white black` (они сами меняются в светлой теме),
имена из CSS — `green blue yellow orange purple pink gray transparent`, и `#ff8800`, `#f80`, `rgb(255, 136, 0)`.

Картинка: `image (A1:B3)` и `src: cat.png` (путь от программы или от папки файла .tbs). Вписывается в место целиком;
`fit: fill` — растянуть на всё место.

## Стили

Это как классы в CSS: стиль пишется один раз, а вещь берёт его через `style:`. Её собственные свойства важнее свойств стиля.

```
style price
    color: accent
    size: 22
    align: right

screen main 2x2
    text total (B2)
        text: Итого: 0 сом
        style: price
```

## Логика в .tb

```
use ui
use view

func main()
    window_sharp()
    try window_open(820, 520, "Окно из .tbs")
    u = ui.start()
    v = view.file("app.tbs")
    repeat while window_alive()
        ui.frame(u)
        clear_window(u.bg)
        view.screen(u, v, "main")
        if view.pressed(v, "save")
            print(view.text_of(v, "name"))
        ui.end_frame(u)
```

| Слово | Что |
|---|---|
| `view.file("app.tbs")` | прочитать файл (`view.from_text(текст)` — из готового текста) |
| `view.screen(u, v, "main")` | нарисовать экран (экранов в файле может быть несколько) |
| `view.pressed(v, "save")` | кнопку нажали в этом кадре |
| `view.text_of(v, "name")` / `view.put_text(v, "name", "...")` | текст поля |
| `view.is_on(v, "id")` / `view.put_on(v, "id", true)` | переключатель, галочка |
| `view.number_of(v, "id")` / `view.put_number(v, "id", 5)` | ползунок, номер в списке, вкладка |

Поменяли `.tbs` и сохранили — окно перерисуется само (файл перечитывается каждые 30 кадров), а то, что ввели в поля, остаётся.
Ошибку в `.tbs` окно покажет красным, с номером строки, а программа не падает.

Пример: `examples/apps/tbs_demo` (`tb run examples/apps/tbs_demo/main.tb`).
