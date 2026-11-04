"""Package an already-built Windows app with a verified portable runtime.

Run the build first in an MSVC developer environment. The runtime is an
existing approved portable folder with RUNTIME_MANIFEST.json and licenses/.
This checks packaging; it does not certify a CI run or a clean Windows install.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def package(args):
    repo = Path(__file__).resolve().parents[1]
    runtime, stage, output = args.runtime.resolve(), args.stage.resolve(), args.output.resolve()
    if stage.exists() or output.exists():
        raise ValueError("Use a new stage directory and ZIP name; existing deliveries are preserved")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    manifest = json.loads((runtime / "RUNTIME_MANIFEST.json").read_text(encoding="utf-8"))
    for row in manifest:
        if sha256(runtime / row["file"]) != row["sha256"]:
            raise ValueError(f"Runtime hash mismatch: {row['file']}")
    listed = {row["file"].replace("\\", "/").lower() for row in manifest}
    unlisted = [f.relative_to(runtime).as_posix() for f in runtime.rglob("*")
                if f.suffix.lower() == ".dll" and f.relative_to(runtime).as_posix().lower() not in listed]
    if unlisted:
        raise ValueError(f"Runtime DLLs absent from manifest: {unlisted}")
    for name in ["occt", "qt6-main", "opennurbs"]:
        if not any((runtime / "licenses" / name).rglob("*")):
            raise ValueError(f"Missing library license: {name}")
    shutil.copytree(runtime, stage)
    binaries = {}
    for name, folder in {"SketchBRep.exe": "src/app", "sbr_cli.exe": "tools/cli",
                         "sbr_mcp.exe": "tools/mcp", "sbr_phase0.exe": "tools/phase0"}.items():
        source = args.build / folder / name
        shutil.copy2(source, stage / name)
        binaries[name] = sha256(stage / name)
        if binaries[name] != sha256(source):
            raise ValueError(f"Binary copy mismatch: {name}")
    for doc in sorted((repo / "docs").glob("*.md")):
        shutil.copy2(doc, stage / "manual" / doc.name)
    shutil.copytree(repo / "docs" / "validation", stage / "manual" / "validation", dirs_exist_ok=True)
    for doc in ["00_START_HERE.md", "EULA.md", "THIRD_PARTY_NOTICES.md"]:
        shutil.copy2(repo / "packaging" / doc, stage / doc)
    # Use the shipped plugins even on a developer PC with Qt installed.
    (stage / "qt.conf").write_text("[Paths]\nPrefix=.\nPlugins=.\n", encoding="ascii")
    (stage / "LOCAL_BUILD.md").write_text(
        f"# Windows ローカル更新版\n\n実装コミット: {commit}\n\n"
        "ZIPをすべて展開し、SketchBRep.exeを起動してください。\n"
        "変更と操作方法は manual/ の更新記録、検証は manual/validation を参照してください。\n"
        "ローカル検証版です。CI・人による実機操作の確認を示すものではありません。\n"
        "インストーラーは含みません。\n", encoding="utf-8")
    checked = 0
    for binary in sorted(stage.rglob("*")):
        if binary.suffix.lower() not in {".exe", ".dll"}:
            continue
        dump = subprocess.check_output([str(args.dumpbin), "/nologo", "/dependents", str(binary)],
                                       text=True, encoding="utf-8", errors="replace")
        checked += 1
        for line in dump.splitlines():
            match = re.fullmatch(r"\s+(\S+\.dll)\s*", line, re.IGNORECASE)
            if not match:
                continue
            dep = match[1]
            if dep.lower().startswith(("api-ms-win-", "ext-ms-")):
                continue
            bundled = (stage / dep).is_file() or (binary.parent / dep).is_file()
            runtime_dll = dep.lower().startswith(("msvcp", "vcruntime", "concrt", "vcomp"))
            system = (Path(os.environ["WINDIR"]) / "System32" / dep).is_file()
            if not bundled and (runtime_dll or not system):
                raise ValueError(f"Unresolved dependency: {binary.name} -> {dep}")
    record = {"code_commit": commit, "kind": "local-windows-portable",
              "runtime_dlls_verified": len(manifest), "binary_dependencies_checked": checked,
              "executables_sha256": binaries, "ci_verified": False, "human_windows_verified": False}
    (stage / "BUILD_INFO.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for file in sorted(stage.rglob("*")):
            if file.is_file():
                archive.write(file, "SketchBRep/" + file.relative_to(stage).as_posix())
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None:
            raise ValueError("ZIP integrity check failed")
    digest = sha256(output)
    output.with_suffix(".zip.sha256").write_text(f"{digest}  {output.name}\n", encoding="ascii")
    print(json.dumps({**record, "zip": output.name, "bytes": output.stat().st_size, "sha256": digest}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--stage", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--dumpbin", type=Path, required=True)
    package(parser.parse_args())
