#!/usr/bin/env python3
# Шейдеры видеокарты: compiler/gpu/tb.vert, tb.frag, tb3.vert (Vulkan, GLSL) и tb.metal, tb3.metal (Mac).
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

def msl(name="tb.metal", var="tb_msl"):
    lines = ["TB_API const char %s[] =" % var]
    for l in open(os.path.join(gpu, name)).read().splitlines():
        lines.append('    "' + l.replace("\\", "\\\\").replace('"', '\\"') + '\\n"')
    lines[-1] += ";"
    return lines

def put(text, tag, body):
    a, b = "// @@%s-BEGIN@@" % tag, "// @@%s-END@@" % tag
    i, j = text.index(a), text.index(b)
    return text[:i + len(a)] + "\n" + "\n".join(body) + "\n" + text[j:]

text = open(rt).read()
text = put(text, "SPIRV", spirv("tb.vert", "tb_spv_vert") + spirv("tb.frag", "tb_spv_frag") + spirv("tb3.vert", "tb_spv_vert3"))
text = put(text, "MSL", msl() + msl("tb3.metal", "tb_msl3"))
open(rt, "w").write(text)
print("шейдеры обновлены в compiler/runtime.h")
