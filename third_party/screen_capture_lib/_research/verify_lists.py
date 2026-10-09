# verify_lists.py — ninja 清单 vs BUILD.gn 声明 交叉验证（只读）
import re

nj_path = r"E:\Github\newwebrtc\webrtc\src\out-Release\Windows-x64\obj\modules\desktop_capture\desktop_capture.ninja"
gn_path = r"E:\Github\newwebrtc\webrtc\src\modules\desktop_capture\BUILD.gn"

nj = open(nj_path, encoding="utf-8", errors="replace").read()
ninja = set()
for m in re.finditer(r"(?m)^build obj/modules/desktop_capture/desktop_capture/(\S+)\.obj: cxx (\S+\.cc)", nj):
    ninja.add(m.group(2).replace("../../", ""))

bg = open(gn_path, encoding="utf-8").read()
start = bg.index('rtc_library("desktop_capture")')
end = bg.index("if (is_mac)", start)
blk = bg[start:end]
gn = set(m.group(1) for m in re.finditer(r'"([A-Za-z0-9_/]+\.cc)"', blk))

print("ninja .cc count:", len(ninja))
print("gn .cc count (desktop_capture block, all platforms):", len(gn))
print("in ninja not in gn:", sorted(ninja - gn))
only_gn = sorted(gn - ninja)
print("in gn not in ninja:", only_gn)
