# verify_lists3.py — 把 if(is_win)/if(rtc_enable_win_wgc) 条件 sources 一并纳入验证（只读）
import re

nj = open(r"E:\Github\newwebrtc\webrtc\src\out-Release\Windows-x64\obj\modules\desktop_capture\desktop_capture.ninja", encoding="utf-8", errors="replace").read()
ninja = set(m.group(1) for m in re.finditer(r"(?m)^build obj/modules/desktop_capture/desktop_capture/\S+\.obj: cxx \.\./\.\./(\S+\.cc)", nj))

bg = open(r"E:\Github\newwebrtc\webrtc\src\modules\desktop_capture\BUILD.gn", encoding="utf-8").read()
start = bg.index('rtc_library("desktop_capture")')
# 目标内部有内层 if (is_mac)；目标是终止于外层 "if (is_mac) {\n  rtc_library("desktop_capture_objc")"
end = bg.index('if (is_mac) {\n  rtc_library("desktop_capture_objc")', start)
blk = bg[start:end]

# 取 if (is_win) 段（BUILD.gn:462 起）到 absl_deps（:532）为止
w = blk.rindex("if (is_win) {")
w2 = blk.index("absl_deps = [", w)
winblk = blk[w:w2]
# WGC 段
g = blk.index("if (rtc_enable_win_wgc)")
g2 = blk.index("}\n}", g)
wgcblk = blk[g:g2]

gn = set("modules/desktop_capture/" + m.group(1) for m in re.finditer(r'"([A-Za-z0-9_/]+\.cc)"', blk))
gnwin = set("modules/desktop_capture/" + m.group(1) for m in re.finditer(r'"([A-Za-z0-9_/]+\.cc)"', winblk))
gnwgc = set("modules/desktop_capture/" + m.group(1) for m in re.finditer(r'"([A-Za-z0-9_/]+\.cc)"', wgcblk))

print("base:", len(gn), "win-conditional:", len(gnwin), "wgc-conditional:", len(gnwgc))
print("union:", len(gn | gnwin | gnwgc), " ninja:", len(ninja))
print("union - ninja:", sorted((gn | gnwin | gnwgc) - ninja))
print("ninja - union:", sorted(ninja - (gn | gnwin | gnwgc)))
print("WGC files:", sorted(gnwgc))
