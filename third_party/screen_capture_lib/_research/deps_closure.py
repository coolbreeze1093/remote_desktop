# deps_closure.py — WebRTC desktop_capture (Win/WGC) 头文件依赖闭包分析
# 只读脚本：仅读取 webrtc 树，全部输出写入本目录 (_research/)。
# 根集合 = 任务A 的 59 个 .cc（desktop_capture 53 + primitives 5 + differ_sse2 1）
#          + 这些 target 声明的仅头文件成员（BUILD.gn win 配置，手工转录）。
# include 解析顺序模拟 clang-cl 引号包含语义：包含者所在目录 → -I../.. → -Igen
#   → -Ibuildtools/third_party/libc++ → -Ithird_party/abseil-cpp
#   → -Ithird_party/libyuv/include → (cflags_cc) -Ithird_party/libc++/src/include
import os, re, json, sys

SRC = r"E:\Github\newwebrtc\webrtc\src"
GEN = os.path.join(SRC, r"out-Release\Windows-x64\gen")
OUT = os.path.dirname(os.path.abspath(__file__))

DC = "modules/desktop_capture"

ROOTS_CC = [
 # --- desktop_capture target (53, from desktop_capture.ninja build lines) ---
 "modules/desktop_capture/blank_detector_desktop_capturer_wrapper.cc",
 "modules/desktop_capture/cropped_desktop_frame.cc",
 "modules/desktop_capture/cropping_window_capturer.cc",
 "modules/desktop_capture/desktop_and_cursor_composer.cc",
 "modules/desktop_capture/desktop_capture_metrics_helper.cc",
 "modules/desktop_capture/desktop_capture_options.cc",
 "modules/desktop_capture/desktop_capturer.cc",
 "modules/desktop_capture/desktop_capturer_differ_wrapper.cc",
 "modules/desktop_capture/desktop_capturer_wrapper.cc",
 "modules/desktop_capture/desktop_frame_generator.cc",
 "modules/desktop_capture/desktop_frame_rotation.cc",
 "modules/desktop_capture/differ_block.cc",
 "modules/desktop_capture/fake_desktop_capturer.cc",
 "modules/desktop_capture/fallback_desktop_capturer_wrapper.cc",
 "modules/desktop_capture/full_screen_application_handler.cc",
 "modules/desktop_capture/full_screen_window_detector.cc",
 "modules/desktop_capture/mouse_cursor.cc",
 "modules/desktop_capture/resolution_tracker.cc",
 "modules/desktop_capture/rgba_color.cc",
 "modules/desktop_capture/screen_capturer_helper.cc",
 "modules/desktop_capture/window_finder.cc",
 "modules/desktop_capture/cropping_window_capturer_win.cc",
 "modules/desktop_capture/desktop_frame_win.cc",
 "modules/desktop_capture/mouse_cursor_monitor_win.cc",
 "modules/desktop_capture/screen_capturer_win.cc",
 "modules/desktop_capture/win/cursor.cc",
 "modules/desktop_capture/win/d3d_device.cc",
 "modules/desktop_capture/win/desktop.cc",
 "modules/desktop_capture/win/desktop_capture_utils.cc",
 "modules/desktop_capture/win/display_configuration_monitor.cc",
 "modules/desktop_capture/win/dxgi_adapter_duplicator.cc",
 "modules/desktop_capture/win/dxgi_context.cc",
 "modules/desktop_capture/win/dxgi_duplicator_controller.cc",
 "modules/desktop_capture/win/dxgi_frame.cc",
 "modules/desktop_capture/win/dxgi_output_duplicator.cc",
 "modules/desktop_capture/win/dxgi_texture.cc",
 "modules/desktop_capture/win/dxgi_texture_mapping.cc",
 "modules/desktop_capture/win/dxgi_texture_staging.cc",
 "modules/desktop_capture/win/full_screen_win_application_handler.cc",
 "modules/desktop_capture/win/scoped_thread_desktop.cc",
 "modules/desktop_capture/win/screen_capture_utils.cc",
 "modules/desktop_capture/win/screen_capturer_win_directx.cc",
 "modules/desktop_capture/win/screen_capturer_win_gdi.cc",
 "modules/desktop_capture/win/screen_capturer_win_magnifier.cc",
 "modules/desktop_capture/win/selected_window_context.cc",
 "modules/desktop_capture/win/window_capture_utils.cc",
 "modules/desktop_capture/win/window_capturer_win_gdi.cc",
 "modules/desktop_capture/window_capturer_win.cc",
 "modules/desktop_capture/window_finder_win.cc",
 "modules/desktop_capture/win/wgc_capture_session.cc",
 "modules/desktop_capture/win/wgc_capture_source.cc",
 "modules/desktop_capture/win/wgc_capturer_win.cc",
 "modules/desktop_capture/win/wgc_desktop_frame.cc",
 # --- primitives target (5) ---
 "modules/desktop_capture/desktop_frame.cc",
 "modules/desktop_capture/desktop_geometry.cc",
 "modules/desktop_capture/desktop_region.cc",
 "modules/desktop_capture/shared_desktop_frame.cc",
 "modules/desktop_capture/shared_memory.cc",
 # --- desktop_capture_differ_sse2 target (1) ---
 "modules/desktop_capture/differ_vector_sse2.cc",
]

# 仅头文件成员（target sources 里声明、无对应 .cc；来源 BUILD.gn 转录）
ROOTS_H = [
 "modules/desktop_capture/delegated_source_list_controller.h",
 "modules/desktop_capture/mouse_cursor_monitor.h",
 "modules/desktop_capture/screen_capture_frame_queue.h",
 "modules/desktop_capture/desktop_capture_types.h",
 "modules/desktop_capture/win/scoped_gdi_object.h",
 "modules/desktop_capture/win/scoped_gdi_object.h~dup",  # placeholder removed below
]
ROOTS_H = [r for r in ROOTS_H if not r.endswith("~dup")]

INC_RE = re.compile(r'^\s*#\s*include\s*(["<])([^">]+)[">]', re.M)

def exists(p):
    return os.path.isfile(p)

class Closure:
    def __init__(self):
        self.nodes = {}   # relpath(orig) -> dict(kind, via, resolved_root)
        self.edges = {}   # relpath -> set(relpath of includes resolved)
        self.unresolved = {}  # (includer, inc) -> None

    def normkey(self, p):
        return os.path.normpath(p).replace("/", "\\").lower()

    def add(self, path, kind, via):
        k = self.normkey(path)
        if k not in self.nodes:
            self.nodes[k] = {"path": os.path.normpath(path).replace("\\", "/"),
                             "kind": kind, "via": via}
            return True
        return False

    def resolve(self, inc, includer_abs):
        cands = []
        d = os.path.dirname(includer_abs)
        cands.append((os.path.join(d, inc), "dir-relative"))
        cands.append((os.path.join(SRC, inc), "-I../.."))
        cands.append((os.path.join(GEN, inc), "-Igen"))
        cands.append((os.path.join(SRC, r"buildtools\third_party\libc++", inc), "libcxx-shim"))
        cands.append((os.path.join(SRC, "third_party", "abseil-cpp", inc), "abseil"))
        cands.append((os.path.join(SRC, "third_party", "libyuv", "include", inc), "libyuv"))
        cands.append((os.path.join(SRC, "third_party", "libc++", "src", "include", inc), "libcxx-src"))
        for p, root in cands:
            if exists(p):
                return os.path.normpath(p), root
        return None, None

    def classify(self, abspath, via):
        rel = os.path.relpath(abspath, SRC).replace("\\", "/")
        if abspath.lower().startswith(GEN.lower()):
            return "gen"
        if via == "libcxx-shim":
            return "libcxx-shim"
        if via == "libcxx-src":
            return "libcxx-src"
        if via == "abseil":
            return "abseil"
        if via == "libyuv":
            return "libyuv"
        return "webrtc"

    def walk(self, abspath):
        stack = [abspath]
        while stack:
            cur = stack.pop()
            ckey = self.normkey(cur)
            try:
                text = open(cur, encoding="utf-8", errors="replace").read()
            except OSError as e:
                print("READ FAIL", cur, e, file=sys.stderr)
                continue
            for m in INC_RE.finditer(text):
                if m.group(1) == "<":
                    continue  # 系统头
                inc = m.group(2)
                rp, root = self.resolve(inc, cur)
                if rp is None:
                    self.unresolved[(os.path.relpath(cur, SRC).replace("\\", "/"), inc)] = True
                    continue
                kind = self.classify(rp, root)
                nk = self.normkey(rp)
                new = nk not in self.nodes
                self.edges.setdefault(ckey, set()).add(nk)
                if new:
                    self.nodes[nk] = {"path": os.path.relpath(rp, SRC).replace("\\", "/"),
                                      "kind": kind, "via": root,
                                      "included_from": os.path.relpath(cur, SRC).replace("\\", "/")}
                    if kind in ("webrtc", "abseil", "libyuv", "libcxx-shim", "libcxx-src", "gen"):
                        stack.append(rp)

def main():
    cl = Closure()
    missing_roots = []
    for r in ROOTS_CC + ROOTS_H:
        p = os.path.join(SRC, r)
        if not exists(p):
            missing_roots.append(r)
            continue
        cl.add(p, "root-cc" if r.endswith(".cc") else "root-h", "root")
    for r in missing_roots:
        print("MISSING ROOT:", r, file=sys.stderr)

    for r in ROOTS_CC + ROOTS_H:
        p = os.path.join(SRC, r)
        if exists(p):
            cl.walk(p)

    # ---- outputs ----
    nodes = sorted(cl.nodes.values(), key=lambda n: n["path"])
    with open(os.path.join(OUT, "closure_all_files.txt"), "w", encoding="utf-8") as f:
        for n in nodes:
            f.write(f"{n['kind']}\t{n['via']}\t{n['path']}\n")

    # 按顶层目录分组统计
    groups = {}
    for n in nodes:
        top = "/".join(n["path"].split("/")[:3]) if n["path"].startswith("third_party/") else n["path"].split("/")[0]
        if n["kind"] == "gen":
            top = "gen/"
        if n["kind"] in ("libcxx-shim",):
            top = "buildtools/third_party/libc++"
        if n["kind"] == "libcxx-src":
            top = "third_party/libc++/src/include"
        groups.setdefault(top, []).append(n)
    with open(os.path.join(OUT, "closure_by_dir.txt"), "w", encoding="utf-8") as f:
        for g in sorted(groups):
            f.write(f"### {g}  ({len(groups[g])} files)\n")
            for n in sorted(groups[g], key=lambda x: x["path"]):
                f.write(f"  {n['path']}\n")

    # webrtc 树内文件（manifest 用）：root + webrtc kind
    vendored = [n["path"] for n in nodes if n["kind"] in ("root-cc", "root-h", "webrtc")]
    with open(os.path.join(OUT, "closure_webrtc_vendored.txt"), "w", encoding="utf-8") as f:
        for p in sorted(set(vendored)):
            f.write(p + "\n")

    # 候选运行时符号文件：rtc_base / api / system_wrappers 闭包头的同名 .cc
    cand = []
    for n in nodes:
        p = n["path"]
        if n["kind"] not in ("root-cc", "root-h", "webrtc"):
            continue
        if p.startswith(("rtc_base/", "api/", "system_wrappers/")) and p.endswith(".h"):
            cc = p[:-2] + ".cc"
            if exists(os.path.join(SRC, cc)):
                cand.append((p, cc))
    with open(os.path.join(OUT, "candidate_runtime_symbols.txt"), "w", encoding="utf-8") as f:
        for h, cc in sorted(cand):
            f.write(f"{h} -> {cc}\n")

    # 每个 .cc 根的 absl 直接引用（区分 .cc 直引 / .h 直引）
    absl_direct_cc, absl_direct_h = [], []
    for r in ROOTS_CC:
        p = os.path.join(SRC, r)
        for m in INC_RE.finditer(open(p, encoding="utf-8", errors="replace").read()):
            if m.group(1) == '"' and m.group(2).startswith("absl/"):
                absl_direct_cc.append((r, m.group(2)))
    for n in nodes:
        p = n["path"]
        if p.startswith("modules/desktop_capture/") and p.endswith(".h"):
            for m in INC_RE.finditer(open(os.path.join(SRC, p), encoding="utf-8", errors="replace").read()):
                if m.group(1) == '"' and m.group(2).startswith("absl/"):
                    absl_direct_h.append((p, m.group(2)))
    with open(os.path.join(OUT, "absl_direct_usage.txt"), "w", encoding="utf-8") as f:
        f.write("== .cc 直接 include 的 absl 头 ==\n")
        for a, b in sorted(set(absl_direct_cc)):
            f.write(f"{a}: {b}\n")
        f.write("\n== modules/desktop_capture 头文件直接 include 的 absl 头 ==\n")
        for a, b in sorted(set(absl_direct_h)):
            f.write(f"{a}: {b}\n")

    # libyuv:: 符号使用（闭包内 webrtc 树文件）
    libyuv_used = {}
    for n in nodes:
        if n["kind"] not in ("root-cc", "root-h", "webrtc"):
            continue
        p = os.path.join(SRC, n["path"])
        try:
            t = open(p, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        hits = set(re.findall(r"libyuv::(\w+)", t))
        if hits:
            libyuv_used[n["path"]] = sorted(hits)
    with open(os.path.join(OUT, "libyuv_symbol_usage.txt"), "w", encoding="utf-8") as f:
        for k in sorted(libyuv_used):
            f.write(f"{k}: {', '.join(libyuv_used[k])}\n")

    # 重模块警示
    warn_dirs = ("media/", "call/", "audio/", "video/", "pc/", "p2p/", "sdk/",
                 "api/video", "api/audio", "common_video", "modules/audio", "modules/video")
    warns = [n["path"] for n in nodes
             if n["kind"] in ("root-cc", "root-h", "webrtc") and n["path"].startswith(warn_dirs)]
    with open(os.path.join(OUT, "heavy_module_warnings.txt"), "w", encoding="utf-8") as f:
        if warns:
            for w in sorted(set(warns)):
                f.write(w + "\n")
        else:
            f.write("(none) 闭包中没有 media/call/audio/video/pc/p2p/sdk 相关文件\n")

    # 未解析的引号 include
    with open(os.path.join(OUT, "unresolved_includes.txt"), "w", encoding="utf-8") as f:
        for (inc_from, inc) in sorted(cl.unresolved):
            f.write(f"{inc_from}: {inc}\n")

    # 摘要
    kinds = {}
    for n in nodes:
        kinds[n["kind"]] = kinds.get(n["kind"], 0) + 1
    summary = {
        "roots_cc": len(ROOTS_CC),
        "roots_h": len(ROOTS_H),
        "missing_roots": missing_roots,
        "nodes_total": len(nodes),
        "nodes_by_kind": kinds,
        "unresolved_count": len(cl.unresolved),
        "gen_headers": [n["path"] for n in nodes if n["kind"] == "gen"],
        "libcxx_shim": [n["path"] for n in nodes if n["kind"] == "libcxx-shim"],
        "libcxx_src": [n["path"] for n in nodes if n["kind"] == "libcxx-src"],
        "abseil_headers": sorted({n["path"] for n in nodes if n["kind"] == "abseil"}),
        "libyuv_headers": sorted({n["path"] for n in nodes if n["kind"] == "libyuv"}),
        "candidate_runtime_pairs": len(cand),
        "heavy_warnings": sorted(set(warns)),
    }
    with open(os.path.join(OUT, "closure_summary.json"), "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, ensure_ascii=False)
    print(json.dumps(summary, indent=2, ensure_ascii=False))

if __name__ == "__main__":
    main()
