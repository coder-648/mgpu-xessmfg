"""Syntax-check changed production files using existing dependency settings.

No object, application binary, package or deployment is produced. Run this from
a Visual Studio developer shell; BUILD_ROOT must contain the existing Core tlog.
"""
from pathlib import Path
import ctypes
import subprocess
import sys

repo = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve()
shell = ctypes.windll.shell32
shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]
obj = build / "obj"
if not (obj / "Magpie.Core").exists():
    obj = obj / "x64/Release"
log = obj / "Magpie.Core/Magpie.Core.tlog/CL.command.1.tlog"
text = "\n".join(line for line in log.read_text(encoding="utf-16").splitlines() if not line.startswith("^"))
count = ctypes.c_int()
pointer = shell.CommandLineToArgvW("tool " + text, ctypes.byref(count))
args = [pointer[i] for i in range(1, count.value)]
ctypes.windll.kernel32.LocalFree(pointer)
includes = list(dict.fromkeys(arg for arg in args if arg.lower().startswith("/i")))
defines = list(dict.fromkeys(args[i+1] for i, arg in enumerate(args[:-1]) if arg.lower() == "/d"))
base = ["cl.exe", "/nologo", "/Zs", "/Y-", "/std:c++20", "/EHsc", "/utf-8", "/MT", "/W4", "/WX",
        "/permissive-", "/bigobj", "/Zc:__cplusplus", "/volatile:iso",
        "/I" + str(repo / "src/Magpie.Core"), "/I" + str(repo / "src/Magpie.Core/include"),
        "/I" + str(obj / "Magpie.Core"), "/I" + str(obj / "Magpie.Core/Generated Files")]
base += includes + [arg for define in defines for arg in ("/D", define)]
files = sys.argv[2:] or ["Renderer.cpp", "EffectDrawer.cpp", "FrameSourceBase.cpp", "FrameTrace.cpp",
         "DLSSNRFilter.cpp", "DLSSNRTemporal.cpp", "DLSSSRUpscaler.cpp", "FSR2Upscaler.cpp",
         "FSR2ZeroMVUpscaler.cpp", "FSR3Upscaler.cpp", "FSR3ZeroMVUpscaler.cpp",
         "XeSSUpscaler.cpp", "XeSSZeroMVUpscaler.cpp"]
for file in files:
    print("Production syntax: " + file, flush=True)
    result = subprocess.run(base + [str(repo / "src/Magpie.Core" / file)], cwd=repo / "src/Magpie.Core")
    if result.returncode:
        sys.exit(result.returncode)
print("All changed production translation units passed MSVC /Zs with existing SDK definitions.")
