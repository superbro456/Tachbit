#!/usr/bin/env python3
# Переводы сообщений: compiler/lang/en.tsv → compiler/lang_en.tb (для компилятора)
# и таблица tb_lang_tab в compiler/runtime.h (для готовых программ).
# После этого: python3 tools/embed_runtime.py и ./tools/build_tb.sh
import os, re, sys

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
src = os.path.join(root, 'compiler', 'lang', 'en.tsv')

pairs = []
seen = set()
for line in open(src, encoding='utf-8'):
    line = line.rstrip('\n')
    if not line or line.startswith('#'):
        continue
    if '\t' not in line:
        sys.exit('en.tsv: нет TAB в строке: ' + line)
    ru, en = line.split('\t', 1)
    if ru in seen or ru == en or not ru.strip():
        continue
    seen.add(ru)
    pairs.append((ru, en))

def plain_len(s):
    return len(s.replace('\\"', '"'))

# длинные куски первыми: «нет такого файла картинки» раньше, чем «нет такого»
pairs.sort(key=lambda p: -plain_len(p[0]))

def q(s):
    return '"' + s + '"'

# --- для компилятора (Tachbit)
tb = ['// Сделано tools/embed_lang.py из compiler/lang/en.tsv. Не правь руками.',
      '// Пары: русский кусок, английский кусок; длинные сначала.',
      'modpub func en_pairs()',
      '    p = list text']
for ru, en in pairs:
    tb.append('    p.add(' + q(ru) + ')')
    tb.append('    p.add(' + q(en) + ')')
tb.append('    return p')
open(os.path.join(root, 'compiler', 'lang_en.tb'), 'w', encoding='utf-8').write('\n'.join(tb) + '\n')

# --- для готовых программ (C, внутри runtime.h)
rt_path = os.path.join(root, 'compiler', 'runtime.h')
rt = open(rt_path, encoding='utf-8').read()
begin = '// ---- TB_LANG_TAB (сделано tools/embed_lang.py, не правь руками) ----\n'
end = '// ---- TB_LANG_TAB конец ----\n'
body = ['static const char *const tb_lang_tab[] = {']
for ru, en in pairs:
    body.append('    ' + q(ru) + ', ' + q(en) + ',')
body.append('    0, 0 };')
block = begin + '\n'.join(body) + '\n' + end
if begin in rt:
    a = rt.index(begin)
    b = rt.index(end) + len(end)
    rt = rt[:a] + block + rt[b:]
else:
    sys.exit('runtime.h: нет метки TB_LANG_TAB')
open(rt_path, 'w', encoding='utf-8').write(rt)
print('переводов:', len(pairs))
