#!/usr/bin/env python3
# Шейдеры видеокарты: compiler/gpu/tb.vert, tb.frag (Vulkan, GLSL) и tb.metal (Mac).
# Поменял шейдер — запусти: python3 tools/gpu_shaders.py  (нужен glslangValidator)
# Он переводит GLSL в SPIR-V и кладёт всё в compiler/runtime.h между метками @@...@@,
# потом запусти python3 tools/embed_runtime.py.
import os, struct, subprocess, sys, tempfile
root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
gpu = os.path.join(root, "compiler", "gpu")
rt = os.path.join(root, "compiler", "runtime.h")

def spirv(name, var):
    out = tempfile.mktemp(suffix=".spv")
    subprocess.check_call(["glslangValidator", "-V", "-o", out, os.path.join(gpu, name)], stdout=subprocess.DEVNULL)
    data = open(out, "rb").read()
    os.remove(out)
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    lines = ["TB_API const uint32_t %s[%d] = {" % (var, len(words))]
    for i in range(0, len(words), 8):
        lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
    lines.append("};")
    return lines

def msl():
    lines = ["TB_API const char tb_msl[] ="]
    for l in open(os.path.join(gpu, "tb.metal")).read().splitlines():
        lines.append('    "' + l.replace("\\", "\\\\").replace('"', '\\"') + '\\n"')
    lines[-1] += ";"
    return lines

def put(text, tag, body):
    a, b = "// @@%s-BEGIN@@" % tag, "// @@%s-END@@" % tag
    i, j = text.index(a), text.index(b)
    return text[:i + len(a)] + "\n" + "\n".join(body) + "\n" + text[j:]

text = open(rt).read()
text = put(text, "SPIRV", spirv("tb.vert", "tb_spv_vert") + spirv("tb.frag", "tb_spv_frag"))
text = put(text, "MSL", msl())
open(rt, "w").write(text)
print("шейдеры обновлены в compiler/runtime.h")
