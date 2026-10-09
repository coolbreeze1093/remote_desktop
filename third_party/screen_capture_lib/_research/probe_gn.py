bg = open(r"E:\Github\newwebrtc\webrtc\src\modules\desktop_capture\BUILD.gn", encoding="utf-8").read()
lines = bg.splitlines(True)
for i in range(459, 466):
    print(i + 1, repr(lines[i]))
start = bg.index('rtc_library("desktop_capture")')
end = bg.index("if (is_mac)", start)
print("blk span chars", start, end)
probe = bg.find("if (is_win)", start, end)
print("find if (is_win) in blk:", probe)
