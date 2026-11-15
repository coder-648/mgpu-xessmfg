"""Check changed production translation units without product build/output.

Pass an existing Release object directory containing Magpie.Core and Magpie
CL.command.1.tlog files. These supply dependency paths and feature defines;
current checkout headers take precedence over the old build's source paths.
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
for project, sources in {
    "Magpie.Core": ["DirectXHelper.cpp", "DeviceResources.cpp"],
    "Magpie": ["AdaptersService.cpp"],
}.items():
    raw = " ".join(line for line in
                   (build / project / f"{project}.tlog/CL.command.1.tlog").read_text(encoding="utf-16").splitlines()
                   if not line.startswith("^"))
    count = ctypes.c_int()
    pointer = shell.CommandLineToArgvW("tool " + raw, ctypes.byref(count))
    try:
        args = [pointer[i] for i in range(1, count.value)]
    finally:
        ctypes.windll.kernel32.LocalFree(pointer)
    includes = list(dict.fromkeys(arg for arg in args if arg.lower().startswith("/i")))
    definitions = list(dict.fromkeys(args[i+1] for i, arg in enumerate(args) if arg.lower() == "/d"))
    current = [repo / "src/Magpie.Core/include", repo / "src/Magpie.Core", repo / "src/Shared",
               build / project / "Generated Files", build / project]
    for source in sources:
        command = ["cl.exe", "/nologo", "/Zs", "/std:c++20", "/EHsc", "/utf-8",
                   "/W4", "/WX", "/bigobj", "/MT", "/Y-"]
        command += ["/I" + str(path) for path in current] + includes
        command += [item for value in definitions for item in ("/D", value)]
        command.append(str(repo / f"src/{project}/{source}"))
        print(f"Checking current {project}/{source}", flush=True)
        subprocess.run(command, cwd=repo / f"src/{project}", check=True)
print("All 3 current production translation units passed MSVC /Zs /W4 /WX; no product was built.")
