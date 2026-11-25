// Собирает builtins.json для помощника: какие встроенные слова есть (из compiler/check.tb и emit.tb)
// и что каждое значит (строки таблиц из docs/guide.md, где слово стоит как `имя(...)`).
// Запуск из корня репозитория: node tools/vscode/lsp/make_builtins.js
const fs = require('fs');
const path = require('path');
const root = path.resolve(__dirname, '..', '..', '..');
const check = fs.readFileSync(path.join(root, 'compiler/check.tb'), 'utf8');
const emit = fs.readFileSync(path.join(root, 'compiler/emit.tb'), 'utf8');
const guide = fs.readFileSync(path.join(root, 'docs/guide.md'), 'utf8');

const names = new Set();
const m1 = check.match(/each b in \[([^\]]*)\]/);
if (m1) for (const w of m1[1].match(/"([a-z_0-9]+)"/g)) names.add(w.slice(1, -1));
const m2 = emit.match(/c_names = \[([^\]]*)\]/);
if (m2) {
    const all = m2[1].match(/"([^"]*)"/g).map(s => s.slice(1, -1));
    for (let i = 0; i + 2 < all.length; i += 3) names.add(all[i]);
}
for (const w of ['wait_ms', 'uptime_ms', 'read_key', 'yield', 'interrupts_on', 'interrupts_off', 'page_new', 'asm']) names.add(w);

// md → простой текст
function plain(s) {
    return s.replace(/`([^`]*)`/g, '$1').replace(/\*\*([^*]*)\*\*/g, '$1').replace(/\s+/g, ' ').trim();
}

const rows = guide.split('\n').filter(l => l.startsWith('|') && !/^\|\s*-/.test(l));
const out = {};
for (const name of [...names].sort()) {
    const re = new RegExp('`(?:[a-z_]+\\s*=\\s*)?(?:try\\s+)?' + name + '\\(([^`]*)\\)`');
    let best = null;
    for (const row of rows) {
        const m = row.match(re);
        if (!m) continue;
        const cells = row.split('|').slice(1, -1).map(c => c.trim());
        // описание: ячейка, где стоит слово, плюс заголовок строки
        let doc = cells.map(plain).filter(Boolean).join(' — ');
        if (doc.length > 600) {
            // длинную строку таблицы режем на предложение со словом
            const i = doc.indexOf(name + '(');
            const from = Math.max(0, doc.lastIndexOf('. ', i) + 1);
            let to = doc.indexOf('. ', i + name.length);
            if (to < 0 || to - from > 500) to = Math.min(doc.length, from + 500);
            doc = (from > 0 ? '… ' : '') + doc.slice(from, to + 1).trim() + (to + 1 < doc.length ? ' …' : '');
        }
        best = { sig: name + '(' + m[1] + ')', doc };
        break;
    }
    out[name] = best || { sig: name + '(…)', doc: '' };
}
// ручные описания там, где в справке нет строки или она слишком общая
const extra = JSON.parse(fs.readFileSync(path.join(__dirname, 'builtins_extra.json'), 'utf8'));
for (const k of Object.keys(extra)) { names.add(k); out[k] = { sig: extra[k][0], doc: extra[k][1] }; }
fs.writeFileSync(path.join(__dirname, 'builtins.json'), JSON.stringify(out, null, 1) + '\n');
const empty = Object.keys(out).filter(k => !out[k].doc);
console.log('слов:', Object.keys(out).length, ' без описания:', empty.length, empty.join(' '));
