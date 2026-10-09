# verify_lists2.py — 统一路径口径后的交叉验证（只读）
import re

nj = open(r"E:\Github\newwebrtc\webrtc\src\out-Release\Windows-x64\obj\modules\desktop_capture\desktop_capture.ninja", encoding="utf-8", errors="replace").read()
ninja = set(m.group(1) for m in re.finditer(r"(?m)^build obj/modules/desktop_capture/desktop_capture/\S+\.obj: cxx \.\./\.\./(\S+\.cc)", nj))

bg = open(r"E:\Github\newwebrtc\webrtc\src\modules\desktop_capture\BUILD.gn", encoding="utf-8").read()
start = bg.index('rtc_library("desktop_capture")')
end = bg.index("if (is_mac)", start)
blk = bg[start:end]
gn = set("modules/desktop_capture/" + m.group(1) for m in re.finditer(r'"([A-Za-z0-9_/]+\.cc)"', blk))

print("ninja", len(ninja), "gn-block", len(gn))
print("ninja-gn:", sorted(ninja - gn))
print("gn-ninja:", sorted(gn - ninja))
print("wgc in gn:", sorted(x for x in gn if "wgc" in x))
print("wgc in ninja:", sorted(x for x in ninja if "wgc" in x))
