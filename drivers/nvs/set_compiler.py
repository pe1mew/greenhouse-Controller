Import("env")
import os

codeblocks_paths = [
    r"C:\Program Files\CodeBlocks\MinGW\bin",
    r"C:\Program Files (x86)\CodeBlocks\MinGW\bin",
    r"C:\CodeBlocks\MinGW\bin",
]

mingw_bin_path = None
for path in codeblocks_paths:
    if os.path.exists(os.path.join(path, "g++.exe")):
        mingw_bin_path = path
        break

if mingw_bin_path:
    print(f"Found Code::Blocks MinGW at: {mingw_bin_path}")
    env.PrependENVPath("PATH", mingw_bin_path)
else:
    print("ERROR: Code::Blocks MinGW installation not found!")
    Exit(1)
