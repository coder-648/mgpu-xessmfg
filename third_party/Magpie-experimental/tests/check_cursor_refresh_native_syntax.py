"""MIDL/projection generation and MSVC /Zs using existing dependencies.

Run from a Visual Studio developer shell. Does not link an application, create
a Release package or touch deployment. Output is metadata/headers and logs.
"""
from pathlib import Path
import ctypes
import subprocess
import sys

repo = Path(__file__).resolve().parents[1]
build, output = (Path(arg).resolve() for arg in sys.argv[1:3])
output.mkdir(parents=True, exist_ok=True)
obj = build / "obj/Magpie"
if not obj.exists():
    obj = build / "obj/x64/Release/Magpie"
shell = ctypes.windll.shell32
shell.CommandLineToArgvW.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
shell.CommandLineToArgvW.restype = ctypes.POINTER(ctypes.c_wchar_p)
ctypes.windll.kernel32.LocalFree.argtypes = [ctypes.c_void_p]

def parse(text):
    count = ctypes.c_int()
    ptr = shell.CommandLineToArgvW("tool " + text.replace("\n", " "), ctypes.byref(count))
    result = [ptr[i] for i in range(1, count.value)]
    ctypes.windll.kernel32.LocalFree(ptr)
    return result

def run(args, cwd):
    result = subprocess.run(args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = result.stdout.decode("utf-8", errors="replace")
    with (output / "native-tools.log").open("a", encoding="utf-8") as stream:
        stream.write(log)
    if result.returncode:
        print(log[-5000:], flush=True)
        sys.exit(result.returncode)

midl_log = (obj / "Magpie.tlog/midl.command.1.tlog").read_text(encoding="utf-16").splitlines()
args = parse(next(line for line in midl_log if not line.startswith("^") and "APP.IDL" in line.upper()))
args = args[:next(i for i, arg in enumerate(args) if arg.startswith("@")) + 1]
args[args.index("/winmd") + 1] = str(output / "App.winmd")
args[-1] = "@" + str(obj / "Magpie.vcxproj.midlrt.rsp")
args.append(str(repo / "src/Magpie/App.idl"))
print("Production IDL: App.idl including ProfileViewModel", flush=True)
run(["midl.exe"] + args, repo / "src/Magpie")
if not (output / "App.winmd").exists():
    raise RuntimeError("MIDL reported success without producing the current App.winmd; inspect native-tools.log")

generated = output / "Generated Files"
args = parse((obj / "Magpie.vcxproj.cppwinrt_comp.rsp").read_text(encoding="utf-8-sig"))
args[args.index("-in") + 1] = str(output / "App.winmd")
args[args.index("-comp") + 1] = str(generated / "sources")
args[args.index("-out") + 1] = str(generated)
args += ["-in", str(obj / "Unmerged/XamlMetaDataProvider.winmd")]
print("Generate current application projection headers", flush=True)
cppwinrt = repo / "packages/Microsoft.Windows.CppWinRT.3.0.260520.1/bin/cppwinrt.exe"
if not cppwinrt.exists():
    cppwinrt = repo.parents[1] / "source/packages/Microsoft.Windows.CppWinRT.3.0.260520.1/bin/cppwinrt.exe"
run([str(cppwinrt)]
    + args, output)

lines = (obj / "Magpie.tlog/CL.command.1.tlog").read_text(encoding="utf-16").splitlines()
args = parse("\n".join(line for line in lines if not line.startswith("^")))
includes = list(dict.fromkeys(arg for arg in args if arg.lower().startswith("/i")))
defines = list(dict.fromkeys(args[i + 1] for i, arg in enumerate(args[:-1]) if arg.lower() == "/d"))
# /W3 matches the existing application project, whose historical Profile/SDK
# headers have /W4 conversion warnings. All core units use /W4 /WX separately.
base = ["cl.exe", "/nologo", "/Zs", "/Y-", "/std:c++20", "/EHsc", "/utf-8", "/MT",
        "/W3", "/WX", "/permissive-", "/bigobj", "/Zc:__cplusplus", "/volatile:iso",
        "/I" + str(generated), "/I" + str(generated / "sources"),
        "/I" + str(output / "xaml"),
        "/I" + str(obj / "Generated Files"),
        "/I" + str(repo / "src/Magpie.Core/include"), "/I" + str(repo / "src/Magpie")]
base += includes + [arg for define in defines for arg in ("/D", define)]
for file in sys.argv[3:] or ["AppSettings.cpp", "ProfileViewModel.cpp", "HomeViewModel.cpp", "ScalingService.cpp"]:
    print("Production application syntax: " + file, flush=True)
    run(base + [str(repo / "src/Magpie" / file)], repo / "src/Magpie")
print("Current IDL, application projections and changed application translation units passed.")
