# reverse_includes.py — 闭包内每个 absl/libyuv 头被哪些闭包文件引用（只读）
import os, re

SRC = r"E:\Github\newwebrtc\webrtc\src"
OUT = os.path.dirname(os.path.abspath(__file__))
INC_RE = re.compile(r'^\s*#\s*include\s*([\"<])([^\">]+)[\">]', re.M)

# 从闭包清单取全部文件
files = []
for line in open(os.path.join(OUT, "closure_all_files.txt"), encoding="utf-8"):
    kind, via, path = line.rstrip("\n").split("\t", 2)
    files.append((kind, path))

# 只对 webrtc 树内的 include 做引用统计（absl 内部互引单独标 kind）
inc_index = {}   # include arg -> set of includer paths
for kind, path in files:
    p = os.path.join(SRC, path)
    try:
        t = open(p, encoding="utf-8", errors="replace").read()
    except OSError:
        continue
    for m in INC_RE.finditer(t):
        arg = m.group(2)
        inc_index.setdefault(arg, set()).add(path)

absl = [path for kind, path in files if kind == "abseil"]
lyuv = [path for kind, path in files if kind == "libyuv"]

with open(os.path.join(OUT, "absl_reverse_includes.txt"), "w", encoding="utf-8") as f:
    for h in sorted(absl):
        # 把绝对 include 参数转成相对路径关键字
        arg = h.replace("third_party/abseil-cpp/", "")
        incs = inc_index.get(arg, set())
        f.write(f"{h}\n")
        if incs:
            for i in sorted(incs):
                f.write(f"    <- {i}\n")
        else:
            f.write("    <- (no direct includer found in closure)\n")

with open(os.path.join(OUT, "libyuv_reverse_includes.txt"), "w", encoding="utf-8") as f:
    for h in sorted(lyuv):
        arg = h.replace("third_party/libyuv/include/", "")
        incs = inc_index.get(arg, set())
        f.write(f"{h}\n")
        for i in sorted(incs):
            f.write(f"    <- {i}\n")
print("done")
