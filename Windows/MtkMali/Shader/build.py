#!/usr/bin/env python3
"""Build/run the CPU-only DXBC -> SPIR-V -> Mali G57 compiler bridge."""
import hashlib, json, os, re, shlex, shutil, subprocess, sys
from datetime import datetime, timezone
from pathlib import Path
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
PROJECT=ROOT.parent
MESA=PROJECT/"mesa"
BUILD=MESA/"build-t70pro-shader"
VKD3D=PROJECT/"vkd3d"
VBUILD=VKD3D/"build-t70pro-shader"
OUT=ROOT/"Build/MaliShader"
GENXML=MESA/"src/panfrost/genxml"
FRAMEBUFFER_MESA_SOURCES=(
    MESA/"src/panfrost/lib/pan_fb_nir.c",
    MESA/"src/panfrost/lib/pan_fb.h",
    MESA/"src/panfrost/lib/pan_blend.h",
    MESA/"src/panfrost/lib/pan_desc.h",
    MESA/"src/panfrost/lib/pan_image.h",
    MESA/"src/panfrost/compiler/pan_nir.h",
    MESA/"src/panfrost/compiler/pan_compiler.h",
    GENXML/"gen_macros.h",
    GENXML/"gen_pack.py",
    GENXML/"common.xml",
    GENXML/"v9.xml",
)
TARGETS=[
    "src/panfrost/compiler/libpanfrost_compiler.a",
    "src/panfrost/compiler/bifrost/libpanfrost_bifrost.a",
    "src/panfrost/compiler/midgard/libpanfrost_midgard.a",
    "src/panfrost/model/libpanfrost_model_lib.a",
    "src/compiler/spirv/libvtn.a","src/compiler/nir/libnir.a",
    "src/compiler/libcompiler.a","src/util/libmesa_util.a",
    "src/c11/impl/libmesa_util_c11.a","src/util/blake3/libblake3.a",
    "src/panfrost/compiler/bifrost/valhall/libpanfrost_valhall_disasm.a",
    "src/panfrost/compiler/bifrost/libpanfrost_bifrost_disasm.a",
    "src/panfrost/compiler/midgard/libpanfrost_midgard_disasm.a",
]
def run(args, **kw):
    return subprocess.run([str(a) for a in args],check=True,**kw)
def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()
def artifact(path):
    return {"path":str(path),"bytes":path.stat().st_size,"sha256":digest(path)}
def source_fingerprints():
    paths=[p for p in HERE.iterdir() if p.suffix in (".c",".h",".cpp",".py")]
    paths += [HERE.parent/name for name in ("Commands.c","Commands.h","Execution.h","MaliJm.h")]
    paths += list(FRAMEBUFFER_MESA_SOURCES)
    return {str(p.relative_to(PROJECT)):digest(p) for p in sorted(paths)}
def tracked_changes(repo):
    names=run(["git","-C",repo,"diff","--name-only","HEAD"],capture_output=True,text=True).stdout.splitlines()
    return {name:digest(repo/name) for name in names}
def provenance():
    commits={}
    for name,path,expected in (
        ("mesa",MESA,"58f2c55854bd48b18df756edb11f187af9cd2fde"),
        ("vkd3d",VKD3D,"fb7f9ff3c1f8d972ee6f1b8c6a92caf6c8ba1e10")):
        revision=run(["git","-C",path,"rev-parse","HEAD"],capture_output=True,text=True).stdout.strip()
        if revision!=expected:raise RuntimeError(name+" source revision differs from qualified input")
        commits[name]=revision
    generated=VKD3D/"build-t70pro-shader/generated-inputs.json"
    gd=json.loads(generated.read_text())
    if any(digest(VKD3D/name)!=value for name,value in gd["release_generated_input_sha256"].items()):
        raise RuntimeError("vkd3d generated release input changed")
    return {"commits":commits,"mesa_tracked_change_sha256":tracked_changes(MESA),
            "vkd3d_tracked_change_sha256":tracked_changes(VKD3D),
            "vkd3d_generated_release_inputs":artifact(generated)}
def compiler_flags(build):
    commands=json.loads((build/"compile_commands.json").read_text())
    entry=next(c for c in commands if c["file"].endswith("/pan_compiler.c"))
    flags=shlex.split(entry["command"])
    for option in ("-o","-c","-MF","-MQ"):
        while option in flags:
            i=flags.index(option);del flags[i:i+2]
    return [f for f in flags if f!="-MD"]
def generate_framebuffer_headers(destination,env):
    generated=Path(destination)
    (generated/"genxml").mkdir(parents=True,exist_ok=True)
    for name in ("common","v9"):
        with (generated/"genxml"/(name+"_pack.h")).open("wb") as output:
            run([sys.executable,GENXML/"gen_pack.py",GENXML/(name+".xml")],
                cwd=GENXML,env=env,stdout=output)
    return generated
def host_dependencies(env):
    if not (BUILD/"build.ninja").exists():
        run(["meson","setup",BUILD,"-Dgallium-drivers=[]","-Dvulkan-drivers=[]","-Dplatforms=[]",
             "-Dglx=disabled","-Degl=disabled","-Dgbm=disabled","-Dopengl=false",
             "-Dgles1=disabled","-Dgles2=disabled","-Dllvm=disabled","-Dspirv-tools=disabled",
             "-Dbuild-tests=false","-Dtools=panfrost-compiler","-Dpanfrost-rust=false",
             "-Dpanfrost-disassemble=true","-Dbuildtype=release","-Db_ndebug=false"],cwd=MESA,env=env)
    run(["ninja","-C",BUILD,"-j6",*TARGETS],env=env)
    VBUILD.mkdir(exist_ok=True)
    inc=VBUILD/"include/vulkan";inc.mkdir(parents=True,exist_ok=True)
    for name in ("spirv.h","GLSL.std.450.h"):
        link=inc/name
        if not link.exists():link.symlink_to(MESA/"src/compiler/spirv"/name)
    venv=env.copy()
    venv.update({
        "BISON":str(PROJECT/"tools/host-bison/bison/3.8.2/bin/bison"),
        "BISON_PKGDATADIR":str(PROJECT/"tools/host-bison/bison/3.8.2/share/bison"),
        "CPPFLAGS":"-I"+str(VBUILD/"include")+" -I"+str(MESA/"include"),
        "CFLAGS":"-O2 -fstack-protector-strong",
        "SONAME_LIBVULKAN":"libvulkan.dylib","am__fastdepOBJC_TRUE":"#","am__fastdepOBJC_FALSE":"",
    })
    if not (VBUILD/"Makefile").exists():
        run([VKD3D/"configure","--disable-maintainer-mode","--disable-tests","--disable-demos",
             "--disable-static","--without-xcb","--without-opengl","--without-metal","--without-ncurses",
             "--without-spirv-tools","--disable-dependency-tracking"],cwd=VBUILD,env=venv)
    run(["gmake","-j6","include/private/spirv_grammar.h","include/private/vkd3d_version.h"],cwd=VBUILD,env=venv)
    run(["gmake","-j6","libvkd3d-shader.la"],cwd=VBUILD,env=venv)
def main():
    if "--locked" not in sys.argv:
        run([ROOT/"Resources/Scripts/with-build-lock","T70Pro",
             "Compile and execute DXBC to G57 shader pipeline","--",sys.executable,__file__,"--locked"])
        return
    if shutil.disk_usage(PROJECT).free<40*1024**3:raise RuntimeError("Less than 40 GiB free")
    OUT.mkdir(parents=True,exist_ok=True)
    before=source_fingerprints(); origin=provenance()
    env=os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"]="1";env["PYTHONPATH"]=str(PROJECT/".host-python")
    host_dependencies(env)
    generated_headers=generate_framebuffer_headers(OUT/"mesa-generated",env)
    flags=compiler_flags(BUILD);compiler=flags.pop(0)
    flags+=["-I"+str(HERE),"-I"+str(MESA/"src/panfrost"),"-I"+str(MESA/"src/panfrost/model"),"-I"+str(MESA/"src/gallium/include"),
            "-I"+str(VKD3D/"include"),"-fstack-protector-strong",
            "-fsanitize=address,undefined","-fno-sanitize-recover=all"]
    objects=[]
    sources=(
        ("ShaderCompiler.c",HERE/"ShaderCompiler.c",()),
        ("ShaderCompilerTests.c",HERE/"ShaderCompilerTests.c",()),
        ("ShaderAbiTests.cpp",HERE/"ShaderAbiTests.cpp",()),
        ("Commands.c",HERE.parent/"Commands.c",()),
        ("FramebufferPreload.c",HERE/"FramebufferPreload.c",("-DPAN_ARCH=9",)),
        ("pan_fb_nir.c",MESA/"src/panfrost/lib/pan_fb_nir.c",
         ("-DPAN_ARCH=9","-DPAN_FB_NIR_SHADER_ONLY","-ffunction-sections","-fdata-sections")),
    )
    for name,source,extra_flags in sources:
        obj=OUT/(Path(name).name+".o")
        source_flags=flags if source.suffix!=".cpp" else [f for f in flags if not f.startswith("-std=")]+["-std=c++17"]
        source_flags=[*source_flags,"-I"+str(generated_headers),*extra_flags]
        if name=="ShaderCompiler.c":source_flags=[*source_flags,"-DT70_SHADER_TESTS"]
        run([compiler,*source_flags,"-c",source,"-o",obj],cwd=BUILD,env=env)
        objects.append(obj)
    libraries=sorted(BUILD.rglob("*.a"))
    executable=OUT/"MaliShaderTests"
    san=["-fsanitize=address,undefined","-fno-sanitize-recover=all"]
    run(["c++",*san,*objects,*libraries,"-Wl,-dead_strip","-L"+str(VBUILD/".libs"),"-lvkd3d-shader",
         "-Wl,-rpath,"+str(VBUILD/".libs"),"-lz","-lzstd","-L/opt/homebrew/lib",
         "-framework","CoreFoundation","-framework","IOKit","-lcompression","-lpthread","-o",executable],env=env)
    env["DYLD_LIBRARY_PATH"]=str(VBUILD/".libs")
    result=run([executable],cwd=OUT,env=env,capture_output=True,text=True)
    print(result.stdout,end="")
    match=re.search(r"shader cases: (\d+); threaded compilations: (\d+); checks: (\d+)",result.stdout)
    if not match:raise RuntimeError("Shader test summary missing")
    pipelines=re.search(r"linked pipelines: (\d+); register moves: (\d+); component moves: (\d+)",result.stdout)
    if not pipelines:raise RuntimeError("Pipeline linkage summary missing")
    threaded_pipelines=re.search(r"threaded pipelines: (\d+)",result.stdout)
    if not threaded_pipelines:raise RuntimeError("Threaded pipeline summary missing")
    tokens=re.search(r"DDI token shaders: (\d+); token pipelines: (\d+)",result.stdout)
    if not tokens:raise RuntimeError("DDI token test summary missing")
    preload=re.search(r"framebuffer preload variants: (\d+); concurrent compiles: (\d+)",result.stdout)
    if not preload:raise RuntimeError("Framebuffer preload test summary missing")
    token_threads=re.search(r"DDI threaded pipelines: (\d+)",result.stdout)
    if not token_threads:raise RuntimeError("Threaded DDI token summary missing")
    layout=OUT/"MaliShaderLayoutTests"
    run([compiler,*flags,"-DT70_LAYOUT_NATIVE",HERE/"ShaderBitfieldTests.c","-o",layout],cwd=BUILD,env=env)
    lr=run([layout],env=env,capture_output=True,text=True);print(lr.stdout,end="")
    lm=re.search(r"shader layouts: (\d+) checks passed",lr.stdout)
    if not lm:raise RuntimeError("Layout test summary missing")
    if source_fingerprints()!=before or provenance()!=origin:
        raise RuntimeError("Shader source inputs changed during verification")
    report={"schema":1,"at":datetime.now(timezone.utc).isoformat(),"status":"passed",
        "shader_program_descriptors":"Real compiled metadata and entry offsets fed into the actual arch-9 CPU descriptor encoder; SM4/5 SPIR-V denorm execution mode compared with exported flags. Synthetic GPU VA; no GPU execution.",
        "scope":"Actual host CPU compilation from HLSL to DXBC, SPIR-V and arch-9 Valhall; bridge/test C and C++ objects use ASan/UBSan. Mesa/vkd3d dependency libraries are not sanitizer-instrumented.",
        "DDI_native_threaded_pipeline_compilations":int(token_threads[1]),
        "DDI_token_shader_cases":int(tokens[1]),"DDI_token_pipeline_cases":int(tokens[2]),
        "framebuffer_preload_variants":int(preload[1]),
        "framebuffer_preload_concurrent_compilations":int(preload[2]),
        "native_shader_cases":int(match[1]),"native_threaded_compilations":int(match[2]),
        "native_assertions":int(match[3]),"native_layout_assertions":int(lm[1]),
        "linked_pipeline_cases":int(pipelines[1]),"linked_register_moves":int(pipelines[2]),
        "linked_component_moves":int(pipelines[3]),
        "native_threaded_pipeline_compilations":int(threaded_pipelines[1]),
        "linked_values_scope":"Host-only hook observes actual scalar NIR constant stores before code generation; verifies semantic component values and destination lanes. Reference PS machine code/layout match; VS instruction schedules may differ.",
        "gpu_target":"G57 arch-9 0x90930000 selected offline; runtime GPU ID not read",
        "source_sha256":before,"provenance":origin,
        "artifacts":{"tests":artifact(executable),"layouts":artifact(layout),
                     "vkd3d_shader":artifact(VBUILD/".libs/libvkd3d-shader.1.dylib")},
        "linked_mesa_libraries":{str(p.relative_to(MESA)):digest(p) for p in libraries},
        "test_output":result.stdout,"layout_output":lr.stdout,
        "actual_windows_execution":False,"gpu_execution":False,"DDI_shader_registration":False}
    (PROJECT/"analysis/mali-shader-verification.json").write_text(json.dumps(report,indent=2)+"\n")
if __name__=="__main__":
    try:main()
    except subprocess.CalledProcessError as error:
        if error.stdout:print(error.stdout,end="",file=sys.stderr)
        if error.stderr:print(error.stderr,end="",file=sys.stderr)
        sys.exit(error.returncode)
