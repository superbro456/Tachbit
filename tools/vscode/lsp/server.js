#!/usr/bin/env node
// Сервер LSP для Tachbit: тот же помощник, что в VS Code, но для любого редактора
// (Zed, Neovim, Helix, Sublime, JetBrains через плагин LSP). Говорит по stdin/stdout.
// Запуск: node tools/vscode/lsp/server.js   (или команда tb-lsp, её ставит tools/install.sh)
const cp = require('child_process');
const path = require('path');
const url = require('url');
const A = require('./analyze.js');

const docs = new Map(); // uri → текст

function send(msg) {
    const body = JSON.stringify(Object.assign({ jsonrpc: '2.0' }, msg));
    process.stdout.write('Content-Length: ' + Buffer.byteLength(body, 'utf8') + '\r\n\r\n' + body);
}
function reply(id, result) { send({ id, result }); }
function notify(method, params) { send({ method, params }); }

function fileOf(uri) { try { return url.fileURLToPath(uri); } catch (_) { return uri; } }
function uriOf(file) { return url.pathToFileURL(file).href; }
function infoOf(uri) { return A.parse(docs.get(uri) || '', fileOf(uri)); }

const KIND = { func: 3, field: 5, var: 6, module: 9, keyword: 14, type: 7, builtin: 3, obj: 7, enum: 20, const: 21 };
const SYM = { func: 12, obj: 23, field: 8, enum: 10, const: 14 };

function range(line, s, e) { return { start: { line, character: s }, end: { line, character: e } }; }

function hover(p) {
    const info = infoOf(p.textDocument.uri);
    const r = A.resolve(info, p.position.line, p.position.character);
    if (!r) return null;
    return { contents: { kind: 'markdown', value: '```tachbit\n' + r.sig + '\n```' + (r.doc ? '\n\n' + r.doc : '') } };
}

function definition(p) {
    const info = infoOf(p.textDocument.uri);
    const r = A.resolve(info, p.position.line, p.position.character);
    if (!r || !r.file) return null;
    return { uri: uriOf(r.file), range: range(r.line, Math.max(0, r.col), Math.max(0, r.col) + r.name.length) };
}

function completion(p) {
    const info = infoOf(p.textDocument.uri);
    return A.complete(info, p.position.line, p.position.character).map(c => ({
        label: c.label, kind: KIND[c.kind] || 1, detail: c.detail,
        documentation: c.doc ? { kind: 'markdown', value: c.doc } : undefined,
        insertText: c.snippet || c.label, insertTextFormat: c.snippet ? 2 : 1,
    }));
}

function signatureHelp(p) {
    const info = infoOf(p.textDocument.uri);
    const s = A.signature(info, p.position.line, p.position.character);
    if (!s) return null;
    return { signatures: [{ label: s.label, documentation: s.doc, parameters: s.params.map(x => ({ label: x })) }], activeSignature: 0, activeParameter: s.active };
}

function refs(p) {
    const info = infoOf(p.textDocument.uri);
    return A.references(info, p.position.line, p.position.character).map(r => ({ uri: p.textDocument.uri, range: range(r.line, r.start, r.end) }));
}

function rename(p) {
    const info = infoOf(p.textDocument.uri);
    const edits = A.references(info, p.position.line, p.position.character).map(r => ({ range: range(r.line, r.start, r.end), newText: p.newName }));
    return { changes: { [p.textDocument.uri]: edits } };
}

function symbols(p) {
    const conv = s => ({
        name: s.name, detail: s.detail, kind: SYM[s.kind] || 13,
        range: range(s.line, 0, 0x7fff), selectionRange: range(s.line, Math.max(0, s.col), Math.max(0, s.col) + s.name.length),
        children: (s.children || []).map(conv),
    });
    return A.symbols(infoOf(p.textDocument.uri)).map(s => {
        const r = conv(s);
        r.range = range(s.line, 0, 0);
        r.range.end = { line: s.end, character: 0x7fff };
        return r;
    });
}

let tbCmd = A.tbPath();
function check(uri) {
    const file = fileOf(uri);
    if (!file.endsWith('.tb')) return;
    const dir = path.dirname(file);
    const env = Object.assign({}, process.env, { NO_COLOR: '1' });
    cp.execFile(tbCmd, ['check', path.basename(file)], { cwd: dir, env, timeout: 20000 }, (err, stdout, stderr) => {
        if (err && err.code === 'ENOENT') return;
        const list = [];
        for (const e of A.parseErrors((stdout || '') + '\n' + (stderr || ''), dir)) {
            if (path.resolve(e.file) !== path.resolve(file)) continue;
            const text = (docs.get(uri) || '').split(/\r?\n/)[e.line - 1] || '';
            const s = text.length - text.trimStart().length;
            list.push({ range: range(e.line - 1, s, Math.max(s + 1, text.length)), severity: 1, source: 'Tachbit', message: e.msg });
        }
        notify('textDocument/publishDiagnostics', { uri, diagnostics: list });
    });
}

function handle(msg) {
    const p = msg.params || {};
    try {
        switch (msg.method) {
            case 'initialize':
                if (p.initializationOptions && p.initializationOptions.tbPath) tbCmd = p.initializationOptions.tbPath;
                return reply(msg.id, {
                    capabilities: {
                        textDocumentSync: { openClose: true, change: 1, save: true },
                        hoverProvider: true, definitionProvider: true, referencesProvider: true, renameProvider: true,
                        documentSymbolProvider: true,
                        completionProvider: { triggerCharacters: ['.'] },
                        signatureHelpProvider: { triggerCharacters: ['(', ','] },
                    },
                    serverInfo: { name: 'tachbit-lsp', version: '1.0.0' },
                });
            case 'initialized': return;
            case 'shutdown': return reply(msg.id, null);
            case 'exit': return process.exit(0);
            case 'textDocument/didOpen':
                docs.set(p.textDocument.uri, p.textDocument.text);
                return check(p.textDocument.uri);
            case 'textDocument/didChange':
                if (p.contentChanges.length) docs.set(p.textDocument.uri, p.contentChanges[p.contentChanges.length - 1].text);
                return;
            case 'textDocument/didSave': return check(p.textDocument.uri);
            case 'textDocument/didClose': docs.delete(p.textDocument.uri); return;
            case 'textDocument/hover': return reply(msg.id, hover(p));
            case 'textDocument/definition': return reply(msg.id, definition(p));
            case 'textDocument/completion': return reply(msg.id, completion(p));
            case 'textDocument/signatureHelp': return reply(msg.id, signatureHelp(p));
            case 'textDocument/references': return reply(msg.id, refs(p));
            case 'textDocument/rename': return reply(msg.id, rename(p));
            case 'textDocument/documentSymbol': return reply(msg.id, symbols(p));
            default:
                if (msg.id !== undefined) send({ id: msg.id, error: { code: -32601, message: 'нет такого: ' + msg.method } });
        }
    } catch (e) {
        if (msg.id !== undefined) send({ id: msg.id, error: { code: -32603, message: String(e && e.stack || e) } });
    }
}

// чтение сообщений: «Content-Length: N\r\n\r\n» и тело
let buf = Buffer.alloc(0);
process.stdin.on('data', chunk => {
    buf = Buffer.concat([buf, chunk]);
    for (;;) {
        const head = buf.indexOf('\r\n\r\n');
        if (head < 0) return;
        const m = buf.slice(0, head).toString('ascii').match(/Content-Length:\s*(\d+)/i);
        if (!m) { buf = buf.slice(head + 4); continue; }
        const len = parseInt(m[1], 10);
        if (buf.length < head + 4 + len) return;
        const body = buf.slice(head + 4, head + 4 + len).toString('utf8');
        buf = buf.slice(head + 4 + len);
        let msg;
        try { msg = JSON.parse(body); } catch (_) { continue; }
        handle(msg);
    }
});
