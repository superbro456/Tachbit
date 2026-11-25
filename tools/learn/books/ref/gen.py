import html, json, re, sys
sys.path.insert(0,'.')
from data import S
out=json.load(open('out.json'))
KW=set("func return gives if else each in count from to repeat times while skip stop const shared obj link int dec text bool and or not true false use modpub modpri temp together background danger lock unlock channel enum match by list dict fail try on all cores vec columns move timer keyboard interrupt".split())
def hl(code):
    res=[]
    for line in code.split('\n'):
        parts=re.split(r'("(?:[^"\\]|\\.)*"|//.*$)', line)
        o=''
        for p in parts:
            if p.startswith('//'): o+='<span class="c">'+html.escape(p)+'</span>'
            elif p.startswith('"') and len(p)>1: o+='<span class="s">'+html.escape(p)+'</span>'
            else:
                o+=re.sub(r'\b([A-Za-z_][A-Za-z0-9_]*|0x[0-9A-Fa-f]+|\d+(?:\.\d+)?)\b', lambda m: ('<span class="k">%s</span>'%m.group(1)) if m.group(1) in KW else (('<span class="n">%s</span>'%m.group(1)) if m.group(1)[0].isdigit() else m.group(1)), html.escape(p))
        res.append(o)
    return '\n'.join(res)
h=[]
toc=[]
k=0
for si,s in enumerate(S,1):
    toc.append(f'<li><a href="#s{si}">{si}. {html.escape(s["title"])}</a></li>')
    h.append(f'<section><h2 id="s{si}"><span class="num">{si}</span>{html.escape(s["title"])}</h2>')
    if s['intro']: h.append(f'<p class="intro">{s["intro"]}</p>')
    for it in s['items']:
        h.append('<div class="item">')
        if it['name']: h.append(f'<h3>{html.escape(it["name"])}</h3>')
        if it['text']: h.append(f'<p>{it["text"]}</p>')
        if it['table']:
            rows=''.join(f'<tr><td><code>{html.escape(a)}</code></td><td>{html.escape(b)}</td></tr>' for a,b in it['table'])
            h.append(f'<table>{rows}</table>')
        if it['code']:
            k+=1
            h.append(f'<pre class="code">{hl(it["code"])}</pre>')
            if str(k) in out:
                h.append(f'<div class="outl">Вывод</div><pre class="out">{html.escape(out[str(k)].rstrip())}</pre>')
        h.append('</div>')
    h.append('</section>')
logo=open('/home/user/tachbit-repo/assets/tachbit-logo-light.svg').read()
logo=re.sub(r'<\?xml[^>]*>','',logo)
page=f'''<!doctype html><html lang="ru"><head><meta charset="utf-8"><title>Справочник Tachbit</title>
<style>
@page{{size:A4;margin:16mm 15mm 16mm 15mm}}
body{{font-family:"DejaVu Sans",Arial,sans-serif;font-size:10.5pt;line-height:1.5;color:#1b1f1e;margin:0}}
.cover{{height:250mm;display:flex;flex-direction:column;justify-content:center;align-items:center;text-align:center;page-break-after:always}}
.cover svg{{width:90mm;height:auto}}
.cover h1{{font-size:28pt;color:#0d4f45;margin:14mm 0 3mm}}
.cover .sub{{color:#555;font-size:12pt}}
.tocp{{page-break-after:always}}
.tocp h2{{border:none}}
.toc{{columns:2;list-style:none;padding:0;font-size:11pt}}
.toc li{{margin:3pt 0}}.toc a{{color:#0d4f45;text-decoration:none}}
h2{{font-size:16pt;color:#0d4f45;border-bottom:2px solid #cfe3df;padding-bottom:3pt;margin:0 0 8pt}}
section{{margin-top:16pt}} h2{{break-after:avoid}} h3{{break-after:avoid}}
.num{{display:inline-block;background:#0d4f45;color:#fff;border-radius:6px;min-width:22px;padding:0 5px;text-align:center;margin-right:8px;font-size:12pt}}
h3{{font-size:12pt;margin:12pt 0 3pt;color:#14342f}}
.intro{{background:#eef6f4;border-left:4px solid #2fd1bf;padding:6pt 10pt}}
.item{{break-inside:avoid}}
pre{{font-family:"DejaVu Sans Mono",Consolas,monospace;font-size:9.3pt;white-space:pre-wrap;border-radius:6px;padding:7pt 10pt;margin:4pt 0}}
pre.code{{background:#f4f7f6;border:1px solid #dde7e4}}
pre.out{{background:#14201e;color:#d8f3ee;border:none}}
.outl{{font-size:8.5pt;color:#6a7b77;margin-top:5pt;text-transform:uppercase;letter-spacing:.06em}}
.k{{color:#0b6e99;font-weight:bold}}.s{{color:#a3501c}}.c{{color:#7b8a86;font-style:italic}}.n{{color:#7c3aed}}
code{{font-family:"DejaVu Sans Mono",Consolas,monospace;background:#f1f5f4;padding:0 3px;border-radius:3px;font-size:9.3pt}}
table{{border-collapse:collapse;width:100%;margin:4pt 0}}
td{{border:1px solid #dde7e4;padding:4pt 6pt;vertical-align:top}}
td:first-child{{width:42%}}
td code{{background:none;padding:0}}
tr:nth-child(even) td{{background:#fafcfb}}
</style></head><body>
<div class="cover">{logo}<h1>Справочник языка</h1><div class="sub">Все команды от переменных и типов до ядра без ОС<br>Tachbit 1.3 · ноябрь 2026</div>
<p style="color:#666;margin-top:12mm;max-width:130mm;font-size:10pt">Каждый пример с блоком «Вывод» собран и запущен этой версией компилятора — вывод настоящий.</p></div>
<div class="tocp"><h2>Содержание</h2><ul class="toc">{"".join(toc)}</ul></div>
{"".join(h)}
</body></html>'''
open('ref.html','w').write(page)
