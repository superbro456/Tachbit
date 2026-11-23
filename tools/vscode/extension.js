// Расширение Tachbit для VS Code: запуск (F5 / Cmd+Enter), проверка при сохранении,
// ошибки подчёркиваются красным прямо в коде. Без зависимостей: обычный JavaScript.
const vscode = require('vscode');
const cp = require('child_process');
const path = require('path');
const fs = require('fs');
const os = require('os');

function tbPath() {
    const set = vscode.workspace.getConfiguration('tachbit').get('path');
    if (set) return set;
    const home = path.join(os.homedir(), '.tachbit', 'bin', 'tb');
    return fs.existsSync(home) ? home : 'tb';
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

// строка ошибки компилятора: «файл.tb, строка 7: сообщение» или «файл.tb: сообщение»
function parseErrors(out, dir) {
    const res = [];
    for (const line of out.split('\n')) {
        let m = line.match(/^(.+?\.tb), строка (\d+): (.*)$/);
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

function activate(context) {
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
