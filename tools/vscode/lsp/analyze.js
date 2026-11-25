// Помощник Tachbit: разбирает файлы .tb и отвечает, что где объявлено.
// Один и тот же код работает в расширении VS Code (extension.js) и в отдельном сервере LSP (server.js)
// для других редакторов. Без зависимостей: обычный JavaScript.
const fs = require('fs');
const path = require('path');
const os = require('os');

const BUILTINS = require('./builtins.json');

const KEYWORDS = [
    'func', 'gives', 'return', 'if', 'else', 'each', 'in', 'count', 'from', 'to', 'repeat', 'times', 'while',
    'skip', 'stop', 'fail', 'try', 'match', 'obj', 'enum', 'shape', 'const', 'use', 'modpub', 'modpri', 'link',
    'and', 'or', 'not', 'true', 'false', 'on', 'all', 'cores', 'shared', 'temp', 'together', 'background',
    'danger', 'lock', 'unlock', 'move', 'by', 'columns', 'vec', 'gpu', 'at', 'big', 'err',
];
const TYPES = ['int', 'dec', 'text', 'bool', 'list', 'dict', 'channel', 'u8', 'u16', 'u32', 'u64',
    'i8', 'i16', 'i32', 'i64', 'f32', 'bits', 'bytes'];
const KEYWORD_DOCS = {
    func: 'функция: `func имя(a: int) gives r: int`',
    gives: 'что функция отдаёт: `gives r: int` (можно несколько: `gives x: int, y: int`)',
    each: 'по каждому: `each x in список`, `each k, v in словарь`',
    count: 'счёт: `count i from 0 to 9` (оба края включительно)',
    repeat: '`repeat 3 times` или `repeat while условие`',
    skip: 'перейти к следующему кругу цикла',
    stop: 'выйти из цикла',
    fail: 'закончить функцию ошибкой: `fail "текст"`',
    try: 'позвать функцию, которая может упасть: при ошибке она поднимется выше',
    match: 'разбор вариантов enum, чисел или текстов',
    obj: 'объект: `obj Имя` и ниже с отступом поля `x: int`',
    enum: 'варианты: `enum Status` и ниже с отступом `new`, `paid`',
    use: 'подключить модуль: `use ui` (ищется рядом, потом в $TB_LIB и ~/.tachbit/lib)',
    modpub: 'видно из других файлов: `modpub func ...`',
    modpri: 'видно только в этом файле',
    link: 'ярлык на объект: `link Node`',
    danger: 'блок, где можно работать с памятью руками',
    background: 'работа в фоне: `a = background f(x)`, потом `a.wait`',
};

// --- строки: убираем комментарии и содержимое текстов, длина строки та же ---
function clean(line) {
    let out = '';
    let inStr = false;
    for (let i = 0; i < line.length; i++) {
        const ch = line[i];
        if (inStr) {
            if (ch === '\\') { out += '  '; i++; continue; }
            if (ch === '"') { inStr = false; out += '"'; continue; }
            out += ' ';
            continue;
        }
        if (ch === '"') { inStr = true; out += '"'; continue; }
        if (ch === '/' && line[i + 1] === '/') { out += ' '.repeat(line.length - i); break; }
        out += ch;
    }
    return out;
}

function indentOf(line) { return line.length - line.trimStart().length; }

// «a: int, b: list text, c: link Node» → [{name, type}]
function splitParams(s) {
    const res = [];
    let depth = 0, cur = '';
    for (const ch of s) {
        if (ch === '(' || ch === '[') depth++;
        if (ch === ')' || ch === ']') depth--;
        if (ch === ',' && depth === 0) { res.push(cur); cur = ''; continue; }
        cur += ch;
    }
    if (cur.trim()) res.push(cur);
    return res.map(p => {
        const m = p.trim().match(/^([A-Za-z_]\w*)\s*(?::\s*(.*))?$/);
        return m ? { name: m[1], type: (m[2] || '').trim() } : null;
    }).filter(Boolean);
}

// комментарии над строкой (и в конце строки) — это описание
function docAbove(lines, n) {
    const res = [];
    for (let i = n - 1; i >= 0; i--) {
        const t = lines[i].trim();
        if (!t.startsWith('//')) break;
        res.unshift(t.replace(/^\/\/\s?/, ''));
    }
    const tail = lines[n].match(/\/\/\s?(.*)$/);
    if (tail && clean(lines[n]).trim() !== '' && !lines[n].trim().startsWith('//')) {
        // хвостовой комментарий только если // стоит вне текста
        const c = clean(lines[n]);
        const at = lines[n].indexOf('//', c.trimEnd().length);
        if (at >= 0) res.push(lines[n].slice(at + 2).trim());
    }
    return res.join('\n');
}

// --- разбор одного файла ---
function parse(text, file) {
    const lines = text.split(/\r?\n/);
    const info = { file, lines, funcs: [], objs: [], enums: [], consts: [], uses: [] };
    let curFunc = null, curObj = null, curEnum = null;
    for (let n = 0; n < lines.length; n++) {
        const raw = lines[n];
        const line = clean(raw);
        const t = line.trim();
        if (t === '') continue;
        const ind = indentOf(line);
        if (ind === 0) {
            if (curFunc) curFunc.end = prevCode(lines, n);
            curFunc = curObj = curEnum = null;
            let m;
            if ((m = t.match(/^use\s+([A-Za-z_]\w*)\s*$/))) {
                info.uses.push({ name: m[1], line: n, col: raw.indexOf(m[1], 3) });
            } else if ((m = t.match(/^(modpub\s+|modpri\s+|gpu\s+)?func\s+([A-Za-z_]\w*)\s*\((.*)\)\s*(?:gives\s+(.*))?$/))) {
                const gives = m[4] ? splitParams(m[4]) : [];
                curFunc = {
                    name: m[2], params: splitParams(m[3]), gives, givesText: (m[4] || '').trim(),
                    pub: (m[1] || '').trim(), line: n, col: raw.indexOf(m[2], raw.indexOf('func') + 4),
                    end: n, doc: docAbove(lines, n), file,
                };
                info.funcs.push(curFunc);
            } else if ((m = t.match(/^(obj|shape)\s+([A-Za-z_]\w*)(.*)$/))) {
                curObj = { name: m[2], kind: m[1], extra: m[3].trim(), fields: [], line: n, col: raw.indexOf(m[2]), doc: docAbove(lines, n), file };
                info.objs.push(curObj);
            } else if ((m = t.match(/^enum\s+([A-Za-z_]\w*)/))) {
                curEnum = { name: m[1], values: [], line: n, col: raw.indexOf(m[1]), doc: docAbove(lines, n), file };
                info.enums.push(curEnum);
            } else if ((m = t.match(/^const\s+([A-Za-z_]\w*)\s*=\s*(.*)$/))) {
                info.consts.push({ name: m[1], value: m[2].trim(), line: n, col: raw.indexOf(m[1]), doc: docAbove(lines, n), file });
            }
            continue;
        }
        if (curObj) {
            const m = t.match(/^([A-Za-z_]\w*)\s*:\s*(.+)$/);
            if (m) curObj.fields.push({ name: m[1], type: m[2].trim(), line: n, col: raw.indexOf(m[1]), doc: docAbove(lines, n), owner: curObj.name, file });
        } else if (curEnum) {
            for (const v of t.split(',')) {
                const w = v.trim();
                if (/^[A-Za-z_]\w*$/.test(w)) curEnum.values.push({ name: w, line: n, col: raw.indexOf(w), owner: curEnum.name, file });
            }
        }
    }
    if (curFunc) curFunc.end = prevCode(lines, lines.length);
    return info;
}

function prevCode(lines, n) {
    for (let i = n - 1; i >= 0; i--) if (clean(lines[i]).trim() !== '') return i;
    return n;
}

// --- переменные внутри функции ---
// [{name, type, line, col}] — первое появление каждого имени до строки upTo
function localsOf(info, fn, upTo) {
    const vars = new Map();
    const add = (name, type, line, col) => {
        if (KEYWORDS.includes(name) || TYPES.includes(name)) return;
        const old = vars.get(name);
        if (!old) vars.set(name, { name, type: type || '', line, col });
        else if (!old.type && type) old.type = type;
    };
    for (const p of fn.params) add(p.name, p.type, fn.line, info.lines[fn.line].indexOf(p.name, fn.col + fn.name.length));
    for (const g of fn.gives) add(g.name, g.type, fn.line, info.lines[fn.line].lastIndexOf(g.name));
    const last = Math.min(fn.end, upTo === undefined ? fn.end : upTo);
    for (let n = fn.line + 1; n <= last; n++) {
        const raw = info.lines[n];
        const line = clean(raw);
        let m;
        if ((m = line.match(/^\s*each\s+([A-Za-z_]\w*)(?:\s*,\s*([A-Za-z_]\w*))?\s+in\s+(.+?)(\s+on\s+all\s+cores|\s+by\s+\d+)?\s*$/))) {
            const src = m[3].trim();
            const st = guessType(info, fn, src, n, vars);
            let el = '';
            if (st.startsWith('list ')) el = st.slice(5).trim();
            else if (st === 'text') el = 'text';
            else if (st.startsWith('channel ')) el = st.slice(8).trim();
            if (m[2]) {
                add(m[1], st.startsWith('dict') ? dictKey(st) : 'int', n, raw.indexOf(m[1]));
                add(m[2], st.startsWith('dict') ? dictVal(st) : el, n, raw.indexOf(m[2], raw.indexOf(m[1]) + m[1].length));
            } else {
                add(m[1], el, n, raw.indexOf(m[1]));
            }
            continue;
        }
        if ((m = line.match(/^\s*count\s+([A-Za-z_]\w*)\s+from/))) { add(m[1], 'int', n, raw.indexOf(m[1])); continue; }
        if ((m = line.match(/^\s*const\s+([A-Za-z_]\w*)\s*=\s*(.*)$/))) { add(m[1], guessType(info, fn, m[2], n, vars), n, raw.indexOf(m[1])); continue; }
        // a = ..., a, b = ..., a: int = ...  (но не ==, +=, a.b = ...)
        if ((m = line.match(/^\s*([A-Za-z_]\w*(?:\s*,\s*[A-Za-z_]\w*)*)\s*(?::\s*([^=]+?))?\s*=(?!=)\s*(.*)$/))) {
            const names = m[1].split(',').map(s => s.trim());
            const rhs = m[3];
            let types = [];
            if (m[2]) types = [m[2].trim()];
            else if (names.length === 1) types = [guessType(info, fn, rhs, n, vars)];
            else types = givesTypesOf(info, rhs);
            let from = 0;
            names.forEach((nm, i) => {
                const col = raw.indexOf(nm, from);
                from = col + nm.length;
                add(nm, types[i] || '', n, col);
            });
            continue;
        }
        // x = f() else err  → err: text
        if ((m = line.match(/\belse\s+([A-Za-z_]\w*)\s*$/))) add(m[1], 'text', n, raw.lastIndexOf(m[1]));
    }
    return [...vars.values()];
}

function dictKey(t) { const p = t.split(/\s+/); return p[1] || ''; }
function dictVal(t) { const p = t.split(/\s+/); return p.slice(2).join(' '); }

// какой тип у выражения справа от «=» (если легко понять)
function guessType(info, fn, expr, line, vars) {
    let e = clean(expr).trim().replace(/^(try|move)\s+/, '').replace(/\s+else\s+\w+\s*$/, '').replace(/\s+or\s+.*$/, '');
    if (/^-?\d+$/.test(e) || /^0x[0-9a-f]+$/i.test(e)) return 'int';
    if (/^-?\d+\.\d+$/.test(e)) return 'dec';
    if (/^"/.test(expr.trim())) return 'text';
    if (/^(true|false)$/.test(e)) return 'bool';
    let m;
    if ((m = e.match(/^(list|dict|channel)\b(.*)$/))) return (m[1] + ' ' + m[2].trim()).trim();
    if ((m = e.match(/^\[/))) {
        const inner = expr.trim().slice(1);
        if (inner.startsWith('"')) return 'list text';
        if (/^-?\d+\.\d/.test(inner)) return 'list dec';
        if (/^-?\d/.test(inner)) return 'list int';
        return 'list';
    }
    // Obj(...) — новый объект
    if ((m = e.match(/^([A-Z]\w*)\s*\(/))) {
        if (findObj(info, m[1])) return m[1];
    }
    // f(...) или mod.f(...)
    if ((m = e.match(/^(?:([A-Za-z_]\w*)\.)?([A-Za-z_]\w*)\s*\(.*\)$/))) {
        const f = findFunc(info, m[2], m[1]);
        if (f && f.gives.length) return f.gives[0].type;
        if (!m[1]) {
            const b = BUILTIN_TYPES[m[2]];
            if (b) return b;
        }
    }
    // a.b — поле объекта
    if ((m = e.match(/^([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)+)$/))) {
        const t = typeOfChain(info, fn, m[1], line, vars);
        if (t) return t;
    }
    // просто имя
    if ((m = e.match(/^([A-Za-z_]\w*)$/)) && vars && vars.get(m[1])) return vars.get(m[1]).type;
    // a[i] — элемент списка
    if ((m = e.match(/^([A-Za-z_]\w*)\[[^\]]*\]$/)) && vars && vars.get(m[1])) {
        const t = vars.get(m[1]).type;
        if (t.startsWith('list ')) return t.slice(5).trim();
        if (t === 'text') return 'text';
        if (t.startsWith('dict')) return dictVal(t);
    }
    if (/["]/.test(expr) && /\+/.test(e)) return 'text';
    return '';
}

const BUILTIN_TYPES = {
    len: 'int', cut: 'text', find: 'int', split: 'list text', join: 'text', replace: 'text', upper: 'text', lower: 'text',
    trim: 'text', starts_with: 'bool', ends_with: 'bool', to_int: 'int', to_dec: 'dec', read_file: 'text', input: 'text',
    args: 'list text', env: 'text', list_dir: 'list text', file_exists: 'bool', is_folder: 'bool', rgb: 'int', rgba: 'int',
    window_alive: 'bool', key_down: 'bool', mouse_x: 'int', mouse_y: 'int', mouse_down: 'bool', mouse_wheel: 'int',
    typed_text: 'text', window_w: 'int', window_h: 'int', load_image: 'int', load_font: 'int', text_width: 'int',
    time_ms: 'int', random: 'int', sqrt: 'dec', sin: 'dec', cos: 'dec', round: 'int', char: 'text', code: 'int',
    http_get: 'text', json_get: 'text', json_text: 'text', date_now: 'text', unix_time: 'int', mesh_new: 'int',
    load_sound: 'int', clipboard_get: 'text', window_scale: 'dec', frame_ms: 'int', fps: 'int',
};

// типы из «a, b = f()»
function givesTypesOf(info, rhs) {
    const m = clean(rhs).trim().replace(/^try\s+/, '').match(/^(?:([A-Za-z_]\w*)\.)?([A-Za-z_]\w*)\s*\(/);
    if (!m) return [];
    const f = findFunc(info, m[2], m[1]);
    return f ? f.gives.map(g => g.type) : [];
}

// тип цепочки a.b.c
function typeOfChain(info, fn, chain, line, vars) {
    const parts = chain.split('.');
    let t = '';
    // vars уже даны (идёт разбор переменных) — заново их не собираем, иначе разбор пойдёт по кругу
    if (vars) { if (vars.get(parts[0])) t = vars.get(parts[0]).type; }
    else if (fn) {
        const v = localsOf(info, fn, line).find(x => x.name === parts[0]);
        if (v) t = v.type;
    }
    if (!t) {
        const en = findEnum(info, parts[0]);
        if (en && parts.length === 2) return en.name;
        return '';
    }
    for (let i = 1; i < parts.length; i++) {
        const o = findObj(info, baseType(t));
        if (!o) return '';
        const f = o.fields.find(x => x.name === parts[i]);
        if (!f) return '';
        t = f.type;
    }
    return t;
}

// «link Node» → Node, «list Item» остаётся списком
function baseType(t) { return (t || '').replace(/^link\s+/, '').trim(); }

// --- модули: use ui → путь к ui.tb ---
const cache = new Map();
function load(file) {
    let st;
    try { st = fs.statSync(file); } catch (_) { return null; }
    const c = cache.get(file);
    if (c && c.mtime === st.mtimeMs) return c.info;
    let text;
    try { text = fs.readFileSync(file, 'utf8'); } catch (_) { return null; }
    const info = parse(text, file);
    cache.set(file, { mtime: st.mtimeMs, info });
    return info;
}

function libDirs(file) {
    const dirs = [];
    if (process.env.TB_LIB) dirs.push(process.env.TB_LIB);
    dirs.push(path.join(os.homedir(), '.tachbit', 'lib'));
    // в репозитории Tachbit: ищем папку lib выше по дереву
    let d = path.dirname(file);
    for (let i = 0; i < 8; i++) {
        const lib = path.join(d, 'lib');
        if (fs.existsSync(path.join(lib, 'ui.tb'))) { dirs.push(lib); break; }
        const up = path.dirname(d);
        if (up === d) break;
        d = up;
    }
    return dirs;
}

function modulePath(file, name) {
    const near = path.join(path.dirname(file), name + '.tb');
    if (fs.existsSync(near)) return near;
    for (const d of libDirs(file)) {
        const p = path.join(d, name + '.tb');
        if (fs.existsSync(p)) return p;
    }
    return null;
}

// модуль по имени (только если файл его подключает)
function moduleInfo(info, name) {
    if (!info.uses.some(u => u.name === name)) return null;
    if (!info.file) return null;
    const p = modulePath(info.file, name);
    return p ? load(p) : null;
}

// все файлы, видные из этого: он сам и его модули (и их модули)
function visible(info) {
    const res = [info];
    const seen = new Set([info.file]);
    const stack = [info];
    while (stack.length) {
        const cur = stack.pop();
        for (const u of cur.uses) {
            if (!cur.file) continue;
            const p = modulePath(cur.file, u.name);
            if (!p || seen.has(p)) continue;
            seen.add(p);
            const m = load(p);
            if (m) { res.push(m); stack.push(m); }
        }
    }
    return res;
}

function findFunc(info, name, mod) {
    if (mod) {
        const m = moduleInfo(info, mod);
        return m ? m.funcs.find(f => f.name === name) : null;
    }
    return info.funcs.find(f => f.name === name) || null;
}
function findObj(info, name) {
    for (const i of visible(info)) { const o = i.objs.find(x => x.name === name); if (o) return o; }
    return null;
}
function findEnum(info, name) {
    for (const i of visible(info)) { const o = i.enums.find(x => x.name === name); if (o) return o; }
    return null;
}

function funcAt(info, line) {
    return info.funcs.find(f => line >= f.line && line <= f.end) || null;
}

// слово под курсором и то, что стоит перед ним через точки: «u.mx» → {word: 'mx', chain: 'u'}
function wordAt(line, col) {
    const c = clean(line);
    let s = col, e = col;
    while (s > 0 && /\w/.test(c[s - 1])) s--;
    while (e < c.length && /\w/.test(c[e])) e++;
    if (s === e) return null;
    const word = c.slice(s, e);
    let chain = '';
    let k = s;
    while (k > 0 && c[k - 1] === '.') {
        let b = k - 1;
        while (b > 0 && /\w/.test(c[b - 1])) b--;
        if (b === k - 1) break;
        chain = c.slice(b, k - 1) + (chain ? '.' + chain : '');
        k = b;
    }
    return { word, chain, start: s, end: e };
}

// --- что показать при наведении и куда прыгнуть ---
function funcSig(f, mod) {
    return 'func ' + (mod ? mod + '.' : '') + f.name + '(' + f.params.map(p => p.name + (p.type ? ': ' + p.type : '')).join(', ') + ')' +
        (f.givesText ? ' gives ' + f.givesText : '');
}

function modName(info, file) {
    if (!file || file === info.file) return '';
    return path.basename(file, '.tb');
}

// что это за слово: {kind, name, sig, doc, file, line, col}
function resolve(info, lineNo, col) {
    const w = wordAt(info.lines[lineNo] || '', col);
    if (!w) return null;
    const fn = funcAt(info, lineNo);
    const raw = clean(info.lines[lineNo]);
    const call = raw.slice(w.end).match(/^\s*\(/);
    if (w.chain) {
        const parts = w.chain.split('.');
        // mod.func
        if (parts.length === 1 && info.uses.some(u => u.name === parts[0])) {
            const m = moduleInfo(info, parts[0]);
            if (m) {
                const f = m.funcs.find(x => x.name === w.word);
                if (f) return { kind: 'func', name: f.name, sig: funcSig(f, parts[0]), doc: f.doc, file: m.file, line: f.line, col: f.col, item: f };
                const o = m.objs.find(x => x.name === w.word);
                if (o) return objResult(o);
            }
        }
        // Enum.value
        const en = findEnum(info, parts[parts.length - 1]);
        if (en && parts.length === 1) {
            const v = en.values.find(x => x.name === w.word);
            if (v) return { kind: 'enum', name: v.name, sig: en.name + '.' + v.name, doc: en.doc, file: en.file, line: v.line, col: v.col };
        }
        // obj.field
        const t = typeOfChain(info, fn, w.chain, lineNo);
        const o = findObj(info, baseType(t));
        if (o) {
            const f = o.fields.find(x => x.name === w.word);
            if (f) return { kind: 'field', name: f.name, sig: o.name + '.' + f.name + ': ' + f.type, doc: f.doc, file: f.file, line: f.line, col: f.col, type: f.type };
        }
        const chanWords = { wait: 'дождаться фоновой работы и взять её результат', send: 'положить в канал (не ждёт)', receive: 'взять из канала (ждёт)', close: 'закрыть канал', add: 'добавить в конец списка', size: 'размер shape в байтах' };
        if (chanWords[w.word]) return { kind: 'keyword', name: w.word, sig: '.' + w.word, doc: chanWords[w.word] };
        return null;
    }
    // местная переменная
    if (fn) {
        const v = localsOf(info, fn, lineNo).find(x => x.name === w.word);
        if (v && !call) return { kind: 'var', name: v.name, sig: v.name + (v.type ? ': ' + v.type : ''), doc: '', file: info.file, line: v.line, col: v.col, type: v.type, fn };
    }
    // функция этого файла
    const f = info.funcs.find(x => x.name === w.word);
    if (f) return { kind: 'func', name: f.name, sig: funcSig(f), doc: f.doc, file: f.file, line: f.line, col: f.col, item: f };
    // obj / enum / const из этого файла или модулей
    const o = findObj(info, w.word);
    if (o) return objResult(o);
    const en = findEnum(info, w.word);
    if (en) return { kind: 'enum', name: en.name, sig: 'enum ' + en.name + ': ' + en.values.map(v => v.name).join(', '), doc: en.doc, file: en.file, line: en.line, col: en.col };
    for (const i of visible(info)) {
        const c = i.consts.find(x => x.name === w.word);
        if (c) return { kind: 'const', name: c.name, sig: 'const ' + c.name + ' = ' + c.value, doc: c.doc, file: c.file, line: c.line, col: c.col };
    }
    // модуль
    const u = info.uses.find(x => x.name === w.word);
    if (u) {
        const p = info.file ? modulePath(info.file, u.name) : null;
        return { kind: 'module', name: u.name, sig: 'use ' + u.name, doc: p ? 'файл: ' + p : 'файл модуля не найден', file: p, line: 0, col: 0 };
    }
    if (BUILTINS[w.word]) return { kind: 'builtin', name: w.word, sig: BUILTINS[w.word].sig, doc: BUILTINS[w.word].doc };
    if (KEYWORD_DOCS[w.word]) return { kind: 'keyword', name: w.word, sig: w.word, doc: KEYWORD_DOCS[w.word] };
    return null;
}

function objResult(o) {
    const body = o.fields.map(f => '    ' + f.name + ': ' + f.type).join('\n');
    return { kind: 'obj', name: o.name, sig: o.kind + ' ' + o.name + (o.extra ? ' ' + o.extra : '') + (body ? '\n' + body : ''), doc: o.doc, file: o.file, line: o.line, col: o.col };
}

// --- подсказки при наборе ---
// [{label, kind, detail, doc, insert}]; kind: func, field, var, module, keyword, type, builtin, obj, enum, const
function complete(info, lineNo, col) {
    const before = clean(info.lines[lineNo] || '').slice(0, col);
    const fn = funcAt(info, lineNo);
    // после точки
    const dot = before.match(/([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\.(\w*)$/);
    if (dot) {
        const chain = dot[1];
        const res = [];
        if (!chain.includes('.') && info.uses.some(u => u.name === chain)) {
            const m = moduleInfo(info, chain);
            if (m) {
                for (const f of m.funcs) if (f.pub === 'modpub') res.push({ label: f.name, kind: 'func', detail: funcSig(f, chain), doc: f.doc, snippet: callSnippet(f) });
                for (const c of m.consts) res.push({ label: c.name, kind: 'const', detail: 'const ' + c.name + ' = ' + c.value, doc: c.doc });
            }
            return res;
        }
        const en = !chain.includes('.') ? findEnum(info, chain) : null;
        if (en) return en.values.map(v => ({ label: v.name, kind: 'enum', detail: en.name + '.' + v.name }));
        const t = typeOfChain(info, fn, chain, lineNo);
        const o = findObj(info, baseType(t));
        if (o) {
            for (const f of o.fields) res.push({ label: f.name, kind: 'field', detail: f.name + ': ' + f.type, doc: f.doc });
            if (o.kind === 'shape') res.push({ label: 'size', kind: 'field', detail: 'размер в байтах' });
        } else if (t.startsWith('list')) {
            res.push({ label: 'add', kind: 'func', detail: 'add(x) — добавить в конец', snippet: 'add(${1})' });
        } else if (t.startsWith('channel')) {
            for (const w of ['send', 'receive', 'close']) res.push({ label: w, kind: 'func', detail: '.' + w + '()', snippet: w + (w === 'send' ? '(${1})' : '()') });
        } else if (!t) {
            res.push({ label: 'wait', kind: 'keyword', detail: 'дождаться фоновой работы' });
        }
        return res;
    }
    const res = [];
    // в начале строки на верхнем уровне — заготовки
    if (fn) {
        for (const v of localsOf(info, fn, lineNo)) res.push({ label: v.name, kind: 'var', detail: v.name + (v.type ? ': ' + v.type : '') });
    }
    for (const f of info.funcs) res.push({ label: f.name, kind: 'func', detail: funcSig(f), doc: f.doc, snippet: callSnippet(f) });
    for (const u of info.uses) res.push({ label: u.name, kind: 'module', detail: 'use ' + u.name });
    for (const i of visible(info)) {
        for (const o of i.objs) res.push({ label: o.name, kind: 'obj', detail: o.kind + ' ' + o.name, doc: o.doc });
        for (const e of i.enums) res.push({ label: e.name, kind: 'enum', detail: 'enum ' + e.name });
        for (const c of i.consts) res.push({ label: c.name, kind: 'const', detail: 'const ' + c.name + ' = ' + c.value });
    }
    for (const k of Object.keys(BUILTINS)) res.push({ label: k, kind: 'builtin', detail: BUILTINS[k].sig, doc: BUILTINS[k].doc, snippet: k + '(${1})' });
    for (const k of KEYWORDS) res.push({ label: k, kind: 'keyword', detail: KEYWORD_DOCS[k] || '' });
    for (const t of TYPES) res.push({ label: t, kind: 'type', detail: 'тип' });
    // одно имя — одна подсказка (первая побеждает: переменная важнее встроенного слова)
    const seen = new Set();
    return res.filter(r => (seen.has(r.label) ? false : (seen.add(r.label), true)));
}

function callSnippet(f) {
    return f.name + '(' + f.params.map((p, i) => '${' + (i + 1) + ':' + p.name + '}').join(', ') + ')';
}

// --- подсказка параметров: внутри «f(a, |» ---
function signature(info, lineNo, col) {
    const before = clean(info.lines[lineNo] || '').slice(0, col);
    let depth = 0, comma = 0;
    for (let i = before.length - 1; i >= 0; i--) {
        const ch = before[i];
        if (ch === ')' || ch === ']') depth++;
        else if (ch === '[') { if (depth === 0) return null; depth--; }
        else if (ch === '(') {
            if (depth > 0) { depth--; continue; }
            const m = before.slice(0, i).match(/(?:([A-Za-z_]\w*)\.)?([A-Za-z_]\w*)\s*$/);
            if (!m) return null;
            let label, params, doc;
            const mod = m[1] && info.uses.some(u => u.name === m[1]) ? m[1] : null;
            const f = findFunc(info, m[2], mod);
            if (f) {
                label = funcSig(f, mod);
                params = f.params.map(p => p.name + (p.type ? ': ' + p.type : ''));
                doc = f.doc;
            } else if (!m[1] && BUILTINS[m[2]]) {
                label = BUILTINS[m[2]].sig;
                const inner = label.slice(label.indexOf('(') + 1, label.lastIndexOf(')'));
                params = splitTop(inner);
                doc = BUILTINS[m[2]].doc;
            } else {
                const o = findObj(info, m[2]);
                if (!o) return null;
                label = o.name + '(' + o.fields.map(x => x.name + ': ' + x.type).join(', ') + ')';
                params = o.fields.map(x => x.name + ': ' + x.type);
                doc = o.doc;
            }
            return { label, params, active: Math.min(comma, Math.max(0, params.length - 1)), doc };
        } else if (ch === ',' && depth === 0) comma++;
    }
    return null;
}

function splitTop(s) {
    const res = [];
    let depth = 0, cur = '';
    for (const ch of s) {
        if (ch === '(') depth++;
        if (ch === ')') depth--;
        if (ch === ',' && depth === 0) { res.push(cur.trim()); cur = ''; continue; }
        cur += ch;
    }
    if (cur.trim()) res.push(cur.trim());
    return res;
}

// --- все места, где стоит то же имя (для «найти все» и «переименовать») ---
// для переменной — только внутри её функции; для остального — в этом файле
function references(info, lineNo, col) {
    const r = resolve(info, lineNo, col);
    const w = wordAt(info.lines[lineNo] || '', col);
    if (!w) return [];
    let from = 0, to = info.lines.length - 1;
    if (r && r.kind === 'var' && r.fn) { from = r.fn.line; to = r.fn.end; }
    const res = [];
    const re = new RegExp('\\b' + w.word + '\\b', 'g');
    for (let n = from; n <= to; n++) {
        const c = clean(info.lines[n]);
        let m;
        while ((m = re.exec(c))) {
            const prev = c.slice(0, m.index);
            const dotted = /\.\s*$/.test(prev);
            // поля и функции модулей стоят после точки, переменные — нет
            if (r && r.kind === 'var' && dotted) continue;
            if (r && (r.kind === 'field') && !dotted && !isObjFieldDecl(info, n)) continue;
            res.push({ line: n, start: m.index, end: m.index + w.word.length });
        }
    }
    return res;
}

function isObjFieldDecl(info, n) {
    for (const o of info.objs) if (o.fields.some(f => f.line === n)) return true;
    return false;
}

// --- значки в списке файла ---
function symbols(info) {
    const res = [];
    for (const f of info.funcs) res.push({ name: f.name, kind: 'func', detail: funcSig(f), line: f.line, col: f.col, end: f.end });
    for (const o of info.objs) {
        const last = o.fields.length ? o.fields[o.fields.length - 1].line : o.line;
        res.push({ name: o.name, kind: 'obj', detail: o.kind, line: o.line, col: o.col, end: last,
            children: o.fields.map(f => ({ name: f.name, kind: 'field', detail: f.type, line: f.line, col: f.col, end: f.line })) });
    }
    for (const e of info.enums) {
        const last = e.values.length ? e.values[e.values.length - 1].line : e.line;
        res.push({ name: e.name, kind: 'enum', detail: 'enum', line: e.line, col: e.col, end: last,
            children: e.values.map(v => ({ name: v.name, kind: 'enum', detail: '', line: v.line, col: v.col, end: v.line })) });
    }
    for (const c of info.consts) res.push({ name: c.name, kind: 'const', detail: c.value, line: c.line, col: c.col, end: c.line });
    return res.sort((a, b) => a.line - b.line);
}

// разбор ошибок компилятора: «файл.tb, строка 7: сообщение» или «файл.tb: сообщение»
function parseErrors(out, dir) {
    const res = [];
    for (const line of out.split('\n')) {
        let m = line.match(/^(.+?\.tb), (?:строка|line) (\d+): (.*)$/);
        let file, num = 1, msg;
        if (m) { file = m[1]; num = parseInt(m[2], 10); msg = m[3]; }
        else {
            m = line.match(/^(.+?\.tb): (.*)$/);
            if (!m) continue;
            file = m[1]; msg = m[2];
        }
        res.push({ file: path.resolve(dir, file), line: Math.max(1, num), msg });
    }
    return res;
}

function tbPath(custom) {
    if (custom) return custom;
    const exe = process.platform === 'win32' ? 'tb.exe' : 'tb';
    const home = path.join(os.homedir(), '.tachbit', 'bin', exe);
    return fs.existsSync(home) ? home : 'tb';
}

module.exports = {
    parse, load, resolve, complete, signature, references, symbols, wordAt, parseErrors, tbPath,
    modulePath, KEYWORDS, TYPES, BUILTINS, clean,
};
