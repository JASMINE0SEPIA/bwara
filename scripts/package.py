"""Build a BwARA Patchbay ZIP against the installed Bitwig 6.1.7."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BML = ROOT / ".deps" / "BML-0.36.0-dev.14-windows"
FRAMEWORKS = ("action-menu-composition-framework", "notifications-framework", "panel-management-framework")


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bitwig", type=Path, default=Path("C:/Program Files/Bitwig Studio"))
    parser.add_argument("--patchbay", type=Path, default=DEFAULT_BML)
    parser.add_argument("--host", type=Path, default=ROOT / "build/bwara/bin/Release/bwara-host.exe")
    args = parser.parse_args()

    original_jar = args.bitwig / "bin" / "bitwig.jar"
    libs = args.bitwig / "bin" / "libs.jar"
    kit = args.patchbay / "modkit" / "bml-modkit.jar"
    api = args.patchbay / "modkit" / "bml-api.jar"
    loader = args.host.with_name("bwara-engine-loader.exe")
    engine = args.host.with_name("bwara-engine.dll")
    frameworks = [args.patchbay / "mods" / framework / "helper.jar" for framework in FRAMEWORKS]
    for path in (original_jar, libs, kit, api, *frameworks, args.host, loader, engine):
        if not path.is_file():
            parser.error(f"missing {path}")
    build = ROOT / "build"
    build.mkdir(exist_ok=True)
    dist = ROOT / "dist"
    dist.mkdir(exist_ok=True)
    mod = ROOT / "mod"
    manifest = json.loads((mod / "mod.json").read_text(encoding="utf-8-sig"))
    build_name = f"bwara-{manifest['mod']['version']}-windows-x64.zip"

    with tempfile.TemporaryDirectory(prefix="pack-", dir=build) as temporary:
        scratch = Path(temporary)
        compile_jar = scratch / "bitwig-compile.jar"
        # 放宽访问权限只发生在编译用副本，安装目录里的 Bitwig JAR 不动。
        run("java", "-jar", kit, "compile-jar", original_jar, compile_jar)
        classes = scratch / "classes"
        classes.mkdir()
        cp = ";".join(str(p) for p in (compile_jar, libs, api, *frameworks))
        run("javac", "--release", "21", "-proc:none", "-encoding", "UTF-8", "-implicit:none",
            "-cp", cp, "-d", classes, *sorted((mod / "src").rglob("*.java")))
        hooks_json = scratch / "hooks.json"
        run("java", "-jar", kit, "finish", classes, hooks_json, original_jar, libs, *frameworks)
        found = json.loads(hooks_json.read_text(encoding="utf-8"))
        opens = {f"open-{n}": target for n, target in enumerate(found["access"])}
        targets = dict(opens)
        for hook in found["hooks"]:
            helper_class = hook.pop("helper_class")
            if helper_class != manifest["helper"]["class"]:
                parser.error(f"hook {hook['id']} belongs to {helper_class}, but the manifest declares a different helper")
            target = f"{hook['id']}-target"
            targets[target] = hook["target"]
            hook["target"] = target
            if "match" in hook:
                match = hook["match"]
                call = f"{hook['id']}-call"
                targets[call] = match["target"]
                hook["match"] = {"opcode": match["opcode"], "target": call, "occurrence": match["occurrence"]}
        manifest["targets"] = targets
        manifest["opens"] = list(opens)
        manifest["hooks"] = found["hooks"]
        helper = scratch / "helper.jar"
        run("jar", "--create", "--file", helper, "-C", classes, ".")
        bundle = scratch / build_name
        with zipfile.ZipFile(bundle, "w", compression=zipfile.ZIP_DEFLATED) as out:
            out.writestr("bwara/mod.json", json.dumps(manifest, indent=2) + "\n")
            out.write(helper, "bwara/helper.jar")
            out.write(args.host, "bwara/bwara-host.exe")
            out.write(loader, "bwara/bwara-engine-loader.exe")
            out.write(engine, "bwara/bwara-engine.dll")
            out.write(ROOT / "docs/use.md", "bwara/docs/use.md")
            out.write(mod / "actions.properties", "bwara/actions.properties")
            out.write(mod / "panels.properties", "bwara/panels.properties")
            out.write(mod / "panel.svg", "bwara/panel.svg")
            out.writestr("bwara/takes/", "")
            for name, relative in (
                ("ARA-LICENSE.txt", ".deps/ARA_SDK/ARA_Examples/LICENSE.txt"),
                ("ARA-NOTICE.txt", ".deps/ARA_SDK/ARA_Examples/NOTICE.txt"),
                ("VST3-LICENSE.txt", ".deps/vst3sdk/LICENSE.txt"),
                ("ICST-LICENSE.txt", ".deps/ARA_SDK/ARA_Examples/3rdParty/ICST_AudioFile/LICENSE.txt"),
                ("cpp-base64-LICENSE.txt", ".deps/ARA_SDK/ARA_Examples/3rdParty/cpp-base64/LICENSE"),
                ("pugixml-LICENSE.txt", ".deps/ARA_SDK/ARA_Examples/3rdParty/pugixml/LICENSE.md"),
                ("MinHook-LICENSE.txt", ".deps/minhook/LICENSE.txt"),
            ):
                license_path = ROOT / relative
                out.write(license_path, f"bwara/licenses/{name}")
        # 所有内容齐了才交付 ZIP；失败时保留上一份完整包。
        bundle.replace(dist / build_name)
    print(dist / build_name)


if __name__ == "__main__":
    main()

