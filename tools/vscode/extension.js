// Расширение Tachbit для VS Code: запуск (F5 / Cmd+Enter), проверка при сохранении,
// ошибки подчёркиваются красным прямо в коде, и помощник языка: подсказки при наборе,
// описание при наведении, переход к объявлению (F12), параметры функции, список функций файла,
// все места имени (Shift+F12) и переименование (F2). Помощник — в lsp/analyze.js,
// тот же код работает как сервер LSP для других редакторов (lsp/server.js).
// Без зависимостей: обычный JavaScript.
const vscode = require('vscode');
const A = require('./lsp/analyze.js');
const cp = require('child_process');
const path = require('path');
const fs = require('fs');

function tbPath() {
    return A.tbPath(vscode.workspace.getConfiguration('tachbit').get('path'));
}

function quote(s) { return "'" + s.replace(/'/g, "'\\''") + "'"; }

let terminal = null;
function getTerminal() {
    if (terminal && terminal.exitStatus === undefined) return terminal;
    terminal = vscode.window.createTerminal({ name: 'Tachbit' });
    return terminal;
}

async function runInTerminal(mode) {
    const ed = vscode.window.activeTextEditor;
    if (!ed || ed.document.languageId !== 'tachbit') {
        vscode.window.showWarningMessage('Открой файл .tb');
        return;
    }
    await ed.document.save();
    const file = ed.document.fileName;
    const t = getTerminal();
    t.show(true);
    t.sendText('cd ' + quote(path.dirname(file)) + ' && ' + quote(tbPath()) + ' ' + mode + ' ' + quote(path.basename(file)));
}

const parseErrors = A.parseErrors;

const diags = vscode.languages.createDiagnosticCollection('tachbit');

function check(doc) {
    if (doc.languageId !== 'tachbit' || doc.isUntitled) return;
    const dir = path.dirname(doc.fileName);
    const env = Object.assign({}, process.env, { NO_COLOR: '1' });
    cp.execFile(tbPath(), ['check', path.basename(doc.fileName)], { cwd: dir, env, timeout: 20000 }, (err, stdout, stderr) => {
        diags.clear();
        if (err && err.code === 'ENOENT') {
            vscode.window.showErrorMessage('Не найдена команда tb. Запусти tools/install.sh или укажи tachbit.path в настройках.');
            return;
        }
        const byFile = new Map();
        for (const e of parseErrors((stdout || '') + '\n' + (stderr || ''), dir)) {
            const uri = vscode.Uri.file(e.file);
            let text = '';
            try { text = fs.readFileSync(e.file, 'utf8').split('\n')[e.line - 1] || ''; } catch (_) {}
            const start = text.length - text.trimStart().length;
            const range = new vscode.Range(e.line - 1, start, e.line - 1, Math.max(start + 1, text.length));
            const d = new vscode.Diagnostic(range, e.msg, vscode.DiagnosticSeverity.Error);
            d.source = 'Tachbit';
            if (!byFile.has(e.file)) byFile.set(e.file, { uri, list: [] });
            byFile.get(e.file).list.push(d);
        }
        for (const { uri, list } of byFile.values()) diags.set(uri, list);
    });
}

// --- помощник языка ---
function infoOf(doc) { return A.parse(doc.getText(), doc.isUntitled ? '' : doc.fileName); }

const CK = vscode.CompletionItemKind;
const KIND = { func: CK.Function, field: CK.Field, var: CK.Variable, module: CK.Module, keyword: CK.Keyword,
    type: CK.TypeParameter, builtin: CK.Function, obj: CK.Class, enum: CK.EnumMember, const: CK.Constant };
const SK = vscode.SymbolKind;
const SYM = { func: SK.Function, obj: SK.Class, field: SK.Field, enum: SK.Enum, const: SK.Constant };

function md(r) {
    const m = new vscode.MarkdownString();
    m.appendCodeblock(r.sig, 'tachbit');
    if (r.doc) m.appendMarkdown('\n' + r.doc.replace(/\n/g, '  \n'));
    return m;
}

const helper = {
    provideHover(doc, pos) {
        const r = A.resolve(infoOf(doc), pos.line, pos.character);
        return r ? new vscode.Hover(md(r)) : null;
    },
    provideDefinition(doc, pos) {
        const r = A.resolve(infoOf(doc), pos.line, pos.character);
        if (!r || !r.file) return null;
        return new vscode.Location(vscode.Uri.file(r.file), new vscode.Position(r.line, Math.max(0, r.col)));
    },
    provideCompletionItems(doc, pos) {
        return A.complete(infoOf(doc), pos.line, pos.character).map((c, i) => {
            const it = new vscode.CompletionItem(c.label, KIND[c.kind] || CK.Text);
            it.detail = c.detail;
            if (c.doc) it.documentation = new vscode.MarkdownString(c.doc);
            if (c.snippet) it.insertText = new vscode.SnippetString(c.snippet);
            // свои имена выше встроенных слов
            const order = { var: 0, field: 0, func: 1, const: 1, enum: 1, obj: 2, module: 2, builtin: 3, keyword: 4, type: 4 };
            it.sortText = String(order[c.kind] !== undefined ? order[c.kind] : 5) + String(i).padStart(4, '0');
            return it;
        });
    },
    provideSignatureHelp(doc, pos) {
        const s = A.signature(infoOf(doc), pos.line, pos.character);
        if (!s) return null;
        const sig = new vscode.SignatureInformation(s.label, s.doc || undefined);
        sig.parameters = s.params.map(p => new vscode.ParameterInformation(p));
        const h = new vscode.SignatureHelp();
        h.signatures = [sig];
        h.activeSignature = 0;
        h.activeParameter = s.active;
        return h;
    },
    provideReferences(doc, pos) {
        return A.references(infoOf(doc), pos.line, pos.character).map(r =>
            new vscode.Location(doc.uri, new vscode.Range(r.line, r.start, r.line, r.end)));
    },
    prepareRename(doc, pos) {
        const info = infoOf(doc);
        const r = A.resolve(info, pos.line, pos.character);
        if (!r || r.kind === 'builtin' || r.kind === 'keyword') throw new Error('Это слово языка, его не переименовать');
        if (r.file && r.file !== info.file) throw new Error('Оно объявлено в другом файле (' + require('path').basename(r.file) + '): переименуй там');
        const w = A.wordAt(doc.lineAt(pos.line).text, pos.character);
        return new vscode.Range(pos.line, w.start, pos.line, w.end);
    },
    provideRenameEdits(doc, pos, newName) {
        const e = new vscode.WorkspaceEdit();
        for (const r of A.references(infoOf(doc), pos.line, pos.character))
            e.replace(doc.uri, new vscode.Range(r.line, r.start, r.line, r.end), newName);
        return e;
    },
    provideDocumentSymbols(doc) {
        const conv = s => {
            const last = Math.min(doc.lineCount - 1, Math.max(s.line, s.end));
            const full = new vscode.Range(s.line, 0, last, doc.lineAt(last).text.length);
            const sel = new vscode.Range(s.line, Math.max(0, s.col), s.line, Math.max(0, s.col) + s.name.length);
            const d = new vscode.DocumentSymbol(s.name, s.detail || '', SYM[s.kind] || SK.Variable, full, sel);
            d.children = (s.children || []).map(conv);
            return d;
        };
        return A.symbols(infoOf(doc)).map(conv);
    },
};

// .tb когда-то могли открыть «как Python» (files.associations) — тогда помощник Tachbit молчит
async function fixAssociation() {
    const cfg = vscode.workspace.getConfiguration('files');
    const assoc = cfg.get('associations') || {};
    const bad = Object.keys(assoc).filter(k => /\.tb$/.test(k) && assoc[k] !== 'tachbit');
    if (!bad.length) return;
    const yes = 'Да, открывать как Tachbit';
    const pick = await vscode.window.showInformationMessage(
        'Файлы .tb сейчас открываются как «' + assoc[bad[0]] + '», поэтому подсказки идут не от Tachbit. Исправить?', yes, 'Нет');
    if (pick !== yes) return;
    for (const target of [vscode.ConfigurationTarget.Global, vscode.ConfigurationTarget.Workspace]) {
        const ins = cfg.inspect('associations');
        const cur = target === vscode.ConfigurationTarget.Global ? ins.globalValue : ins.workspaceValue;
        if (!cur) continue;
        const next = Object.assign({}, cur);
        for (const k of bad) if (k in next) next[k] = 'tachbit';
        try { await cfg.update('associations', next, target); } catch (_) {}
    }
    for (const ed of vscode.window.visibleTextEditors)
        if (ed.document.fileName.endsWith('.tb') && ed.document.languageId !== 'tachbit')
            vscode.languages.setTextDocumentLanguage(ed.document, 'tachbit');
}

function activate(context) {
    const sel = { language: 'tachbit' };
    context.subscriptions.push(
        vscode.languages.registerHoverProvider(sel, helper),
        vscode.languages.registerDefinitionProvider(sel, helper),
        vscode.languages.registerCompletionItemProvider(sel, helper, '.'),
        vscode.languages.registerSignatureHelpProvider(sel, helper, '(', ','),
        vscode.languages.registerReferenceProvider(sel, helper),
        vscode.languages.registerRenameProvider(sel, helper),
        vscode.languages.registerDocumentSymbolProvider(sel, helper)
    );
    fixAssociation();
    context.subscriptions.push(
        diags,
        vscode.commands.registerCommand('tachbit.run', () => runInTerminal('run')),
        // зелёная кнопка «отладка» (F5, Touch Bar, Run and Debug): просто запускаем файл
        vscode.debug.registerDebugConfigurationProvider('tachbit', {
            resolveDebugConfiguration() { runInTerminal('run'); return undefined; }
        }),
        vscode.commands.registerCommand('tachbit.check', () => {
            const ed = vscode.window.activeTextEditor;
            if (ed) ed.document.save().then(() => check(ed.document));
        }),
        vscode.commands.registerCommand('tachbit.showC', () => runInTerminal('c')),
        vscode.workspace.onDidSaveTextDocument(doc => {
            if (vscode.workspace.getConfiguration('tachbit').get('checkOnSave')) check(doc);
        }),
        vscode.workspace.onDidOpenTextDocument(doc => check(doc)),
        vscode.window.onDidCloseTerminal(t => { if (t === terminal) terminal = null; })
    );
    vscode.workspace.textDocuments.forEach(check);
}

function deactivate() {}

module.exports = { activate, deactivate };
