#!/usr/bin/env python3
"""Cross-build the real DXBC/Mesa G57 shader compiler for Windows ARM64.
Uses the existing source checkouts and Zig toolchain; does not install/run on a device.
"""
import json, os, shlex, shutil, struct, subprocess, sys
from datetime import datetime, timezone
import build as common
from pathlib import Path
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[2]
PROJECT=ROOT.parent
MESA=PROJECT/"mesa"
VKD3D=PROJECT/"vkd3d"
MBUILD=MESA/"build-t70pro-shader-windows"
VBUILD=VKD3D/"build-t70pro-shader-windows"
OUT=ROOT/"Build/MaliShader/windows"
ZIG=Path(os.environ.get("T70_MALI_ZIG","/private/tmp/t70-mali-python/ziglang/zig"))
def run(args, **kw):
    subprocess.run([str(a) for a in args],check=True,**kw)

def inspect_pe(path):
    b=path.read_bytes(); pe=struct.unpack_from("<I",b,60)[0]; opt=pe+24
    if b[:2]!=b"MZ" or b[pe:pe+4]!=b"PE\0\0":raise RuntimeError("Not a PE: "+str(path))
    machine,count=struct.unpack_from("<HH",b,pe+4)
    optsize,characteristics=struct.unpack_from("<HH",b,pe+20)
    if machine!=0xaa64 or struct.unpack_from("<H",b,opt)[0]!=0x20b or not characteristics&0x2000:
        raise RuntimeError("Not a Windows ARM64 DLL: "+str(path))
    dc=struct.unpack_from("<H",b,opt+70)[0]
    if dc&0x160!=0x160:raise RuntimeError("DLL lacks ASLR, high-entropy VA or NX")
    sections=[]
    for i in range(count):
        o=opt+optsize+40*i
        vs,rva,rs,raw=struct.unpack_from("<IIII",b,o+8);flags=struct.unpack_from("<I",b,o+36)[0]
        if flags&0xa0000000==0xa0000000:raise RuntimeError("Writable executable section")
        if raw+rs>len(b):raise RuntimeError("Truncated DLL section")
        sections.append((rva,vs,rs,raw))
    def offset(rva):
        for start,vs,rs,raw in sections:
            if start<=rva<start+min(vs,rs):return raw+rva-start
        raise RuntimeError("RVA is not file-backed: "+hex(rva))
    def string(rva):
        o=offset(rva);end=b.index(0,o)
        return b[o:end].decode("ascii")
    erva,esize=struct.unpack_from("<II",b,opt+112)
    d=offset(erva);n=struct.unpack_from("<I",b,d+24)[0]
    names=offset(struct.unpack_from("<I",b,d+32)[0])
    exports=[string(struct.unpack_from("<I",b,names+4*i)[0]) for i in range(n)]
    irva,isize=struct.unpack_from("<II",b,opt+120);imports={}
    if irva:
        pos=offset(irva)
        while any(b[pos:pos+20]):
            orig,_,_,name,thunk=struct.unpack_from("<IIIII",b,pos);pos+=20
            dll=string(name).lower();fns=[];a=offset(orig or thunk)
            while (value:=struct.unpack_from("<Q",b,a)[0]):
                a+=8;fns.append("#"+str(value&65535) if value>>63 else string(value+2))
            imports[dll]=fns
    return {"machine":"ARM64","format":"PE32+ DLL","exports":exports,"imports":imports,
            "ASLR":True,"NX":True,"high_entropy_VA":True,"writable_executable_sections":False}

def layout_test(flags,env):
    obj=OUT/"ShaderBitfieldTests.obj"
    run([*flags,"-I"+str(HERE),"-fno-stack-protector","-c",HERE/"ShaderBitfieldTests.c","-o",obj],
        cwd=MBUILD,env=env)
    pe=OUT/"MaliShaderLayoutTests.exe"
    run([ZIG,"lld-link","/nologo","/threads:6","/noimplib","/machine:arm64",
         "/subsystem:native","/entry:DriverEntry","/nodefaultlib",
         "/out:"+str(pe),obj],env=env)
    penv=env.copy()
    penv["PYTHONPATH"]=os.pathsep.join(filter(None,[
        os.environ.get("T70_MALI_PYTHONPATH","/private/tmp/t70-mali-python"),
        str(PROJECT/".venv/lib/python3.14/site-packages"),env.get("PYTHONPATH","")]))
    run([sys.executable,HERE.parent/"test-wdk.py",pe,"T70ShaderLayoutAssertions"],env=penv)
    report=json.loads(pe.with_suffix(".json").read_text())
    report["scope"]="Actual Windows ARM64 compiler layout instructions executed under Unicorn. Tests 33 words against explicit bit-position oracles for Bifrost/Valhall texture flags and the 64-bit compiler index key. No OS/GPU services are called, and neither shader DLL is executed."
    report["command_preparation"]="not applicable"
    pe.with_suffix(".json").write_text(json.dumps(report,indent=2)+"\n")
    return report,pe

def main():
    if "--locked" not in sys.argv:
        run([ROOT/"Resources/Scripts/with-build-lock","T70Pro",
             "Cross-build real G57 shader compiler for Windows ARM64","--",
             sys.executable,__file__,"--locked",*sys.argv[1:]])
        return
    if shutil.disk_usage(PROJECT).free<40*1024**3:raise RuntimeError("Less than 40 GiB free")
    before=common.source_fingerprints();origin=common.provenance()
    OUT.mkdir(parents=True,exist_ok=True)
    env=os.environ.copy()
    env["PYTHONDONTWRITEBYTECODE"]="1"
    env["PYTHONPATH"]=str(PROJECT/".host-python")
    env["ZIG_GLOBAL_CACHE_DIR"]=str(OUT.parent/"zig-cache")
    env["ZIG_LOCAL_CACHE_DIR"]=str(OUT.parent/"zig-cache-local")
    cc=[str(ZIG),"cc","-target","aarch64-windows-gnu"]
    cpp=[str(ZIG),"c++","-target","aarch64-windows-gnu"]
    sdk_lib=ROOT/"Windows/Toolchain/microsoft.windows.sdk.cpp.arm64/c/um/arm64"
    cross=OUT/"cross.ini"
    cross_text = (
        "[binaries]\nc = "+repr(cc)+"\ncpp = "+repr(cpp)+
        "\nar = "+repr([str(ZIG),"ar"])+
        "\nstrip = 'true'\npkg-config = 'false'\n"
        "[host_machine]\nsystem = 'windows'\ncpu_family = 'aarch64'\ncpu = 'aarch64'\nendian = 'little'\n"
        "[properties]\nneeds_exe_wrapper = true\n"
        "[built-in options]\nc_args = ['-fstack-protector-strong', '-UNDEBUG']\ncpp_args = ['-fstack-protector-strong', '-UNDEBUG']\n"+
        "c_link_args = "+repr(["-L"+str(sdk_lib)])+"\ncpp_link_args = "+repr(["-L"+str(sdk_lib)])+"\n")
    if not cross.exists() or cross.read_text()!=cross_text:
        cross.write_text(cross_text)
    if not (MBUILD/"build.ninja").exists():
        run(["meson","setup",MBUILD,"--cross-file",cross,
             "-Dgallium-drivers=[]","-Dvulkan-drivers=[]","-Dplatforms=[]",
             "-Dglx=disabled","-Degl=disabled","-Dgbm=disabled","-Dopengl=false",
             "-Dgles1=disabled","-Dgles2=disabled","-Dllvm=disabled",
             "-Dspirv-tools=disabled","-Dbuild-tests=false","-Dtools=panfrost-compiler",
             "-Dpanfrost-rust=false","-Dpanfrost-disassemble=true",
             "-Dshader-cache=disabled","-Dzlib=disabled","-Dzstd=disabled",
             "-Dexpat=disabled","-Dxmlconfig=disabled","-Dbuildtype=release","-Db_ndebug=false"],
             cwd=MESA,env=env)
    options=json.loads((MBUILD/"meson-info/intro-buildoptions.json").read_text())
    requested=["-fstack-protector-strong","-UNDEBUG"]
    if any(o["value"]!=requested for o in options if o["name"] in ("c_args","cpp_args")):
        run(["meson","configure",MBUILD,"-Dc_args="+repr(requested),
             "-Dcpp_args="+repr(requested)],env=env)
    if "--configure-only" in sys.argv:
        return
    targets=common.TARGETS
    run(["ninja","-C",MBUILD,"-j6",*targets],env=env)
    if "--mesa-only" in sys.argv:
        return
    generated_headers=common.generate_framebuffer_headers(OUT/"mesa-generated",env)
    VBUILD.mkdir(exist_ok=True)
    inc=VBUILD/"include/vulkan";inc.mkdir(parents=True,exist_ok=True)
    for name in ("spirv.h","GLSL.std.450.h"):
        link=inc/name;target=MESA/"src/compiler/spirv"/name
        if not link.exists():link.symlink_to(target)
    venv=env.copy()
    venv.update({
        "CC":shlex.join(cc),"AR":shlex.join([str(ZIG),"ar"]),
        "RANLIB":shlex.join([str(ZIG),"ranlib"]),
        "LD":shlex.join([str(ZIG),"ld.lld"]),"DLLTOOL":shlex.join([str(ZIG),"dlltool"]),
        "lt_cv_prog_gnu_ld":"yes","lt_cv_deplibs_check_method":"pass_all",
        "vkd3d_cv_cflags__flto_auto":"no",
        "BISON":str(PROJECT/"tools/host-bison/bison/3.8.2/bin/bison"),
        "BISON_PKGDATADIR":str(PROJECT/"tools/host-bison/bison/3.8.2/share/bison"),
        "CPPFLAGS":"-I"+str(VBUILD/"include")+" -I"+str(MESA/"include"),
        "CFLAGS":"-O2 -fstack-protector-strong -Wno-ignored-attributes -Wno-unknown-attributes",
        "SONAME_LIBVULKAN":"vulkan-1.dll","am__fastdepOBJC_TRUE":"#","am__fastdepOBJC_FALSE":"",
    })
    makefile=VBUILD/"Makefile"
    if not makefile.exists() or "-flto=auto" in makefile.read_text():
        # Retire only this failed Windows configuration's compiled intermediates.
        for pattern in ("*.obj","*.lo","*.la","*.a"):
            for path in VBUILD.rglob(pattern):path.unlink()
        run([VKD3D/"configure","--host=aarch64-w64-mingw32","--with-gnu-ld","--disable-maintainer-mode",
             "--disable-tests","--disable-demos","--disable-shared","--enable-static","--without-xcb",
             "--without-opengl","--without-metal","--without-ncurses","--without-spirv-tools",
             "--disable-dependency-tracking"],cwd=VBUILD,env=venv)
    run(["gmake","-j6","include/private/spirv_grammar.h","include/private/vkd3d_version.h"],
        cwd=VBUILD,env=venv)
    run(["gmake","-j6","libvkd3d-shader.la"],cwd=VBUILD,env=venv)
    objects=sorted((VBUILD/"libs/vkd3d-shader").glob("libvkd3d_shader_la-*.obj"))
    if len(objects)!=19 or any(p.read_bytes()[:2]!=b"\x64\xaa" for p in objects):
        raise RuntimeError("Expected 19 real ARM64 COFF shader objects from pinned vkd3d 2.1")
    run([*cc,"-shared",*objects,VBUILD/".libs/libvkd3d-common.a",
         "-Wl,--out-implib,"+str(OUT/"libvkd3d-shader.dll.a"),
         "-o",OUT/"vkd3d-shader.dll"],env=env)
    commands=json.loads((MBUILD/"compile_commands.json").read_text())
    entry=next(c for c in commands if c["file"].endswith("/pan_compiler.c"))
    flags=shlex.split(entry["command"])
    # Preserve compiler and target arguments selected by Meson.
    for option in ("-o","-c","-MF","-MQ"):
        while option in flags:
            i=flags.index(option);del flags[i:i+2]
    flags=[f for f in flags if f!="-MD"]
    objects=[]
    sources=(
        ("ShaderCompiler.c",HERE/"ShaderCompiler.c",()),
        ("FramebufferPreload.c",HERE/"FramebufferPreload.c",("-DPAN_ARCH=9",)),
        ("pan_fb_nir.c",MESA/"src/panfrost/lib/pan_fb_nir.c",
         ("-DPAN_ARCH=9","-DPAN_FB_NIR_SHADER_ONLY","-ffunction-sections","-fdata-sections")),
    )
    for name,source,extra_flags in sources:
        obj=OUT/(Path(name).stem+".obj")
        source_flags=[*flags,"-I"+str(HERE),"-I"+str(MESA/"src/panfrost"),"-I"+str(MESA/"src/panfrost/model"),
                      "-I"+str(MESA/"src/gallium/include"),"-I"+str(VKD3D/"include"),
                      "-I"+str(generated_headers),*extra_flags]
        if name=="ShaderCompiler.c":source_flags.append("-DT70_SHADER_BUILD_DLL")
        run([*source_flags,"-c",source,"-o",obj],cwd=MBUILD,env=env)
        objects.append(obj)
    libraries=sorted(MBUILD.rglob("*.a"))
    shader_import_library=OUT/"libMtkMaliShader.dll.a"
    run([*cpp,"-shared",*objects,*libraries,"-Wl,--gc-sections",
         "-Wl,--out-implib,"+str(shader_import_library),"-L"+str(OUT),"-lvkd3d-shader",
         "-L"+str(sdk_lib),"-lole32","-luuid","-lwinmm","-lsynchronization","-o",OUT/"MtkMaliShader.dll"],env=env)
    layout,layout_pe=layout_test(flags,env)
    dll=OUT/"MtkMaliShader.dll";dependency=OUT/"vkd3d-shader.dll"
    pe=inspect_pe(dll);dp=inspect_pe(dependency)
    expected={"t70_shader_abi_version","t70_shader_compile","t70_shader_free",
              "t70_shader_compile_pipeline","t70_shader_free_pipeline",
              "t70_shader_compile_tokens","t70_shader_compile_token_pipeline",
              "t70_shader_compile_framebuffer_preload"}
    if set(pe["exports"])!=expected or "vkd3d-shader.dll" not in pe["imports"]:
        raise RuntimeError("Public compiler ABI or dynamic vkd3d dependency differs")
    if (not shader_import_library.is_file() or
        b"t70_shader_compile_framebuffer_preload" not in shader_import_library.read_bytes()):
        raise RuntimeError("Shader compiler import library is missing the preload entrypoint")
    if not {"vkd3d_shader_compile","vkd3d_shader_scan"}<=set(dp["exports"]):
        raise RuntimeError("vkd3d shader exports missing")
    if common.source_fingerprints()!=before or common.provenance()!=origin:
        raise RuntimeError("Source inputs changed during Windows verification")
    report={"schema":1,"at":datetime.now(timezone.utc).isoformat(),"status":"passed",
        "scope":"Real Windows ARM64 DLLs cross-linked and inspected. Layout test PE executed under Unicorn; DLLs/Windows/D3D runtime/GPU were not executed.",
        "source_sha256":before,"provenance":origin,
        "artifacts":{"compiler":common.artifact(dll),"vkd3d_shader":common.artifact(dependency),
                     "compiler_import_library":common.artifact(shader_import_library),
                     "layouts":common.artifact(layout_pe)},
        "PE":{"compiler":pe,"vkd3d_shader":dp},"ARM64_layout_test":layout,
        "linked_mesa_libraries":{str(p.relative_to(MESA)):common.digest(p) for p in libraries},
        "cross_configuration":common.artifact(cross),"vkd3d_COFF_objects":len(objects),
        "automatic_LTO":False,"actual_windows_execution":False,"gpu_execution":False,
        "DDI_shader_registration":False}
    (PROJECT/"analysis/mali-shader-windows.json").write_text(json.dumps(report,indent=2)+"\n")
    print("Windows ARM64 DLL inspection and compiler layout execution passed; no Windows/GPU execution.")
if __name__=="__main__":
    try:main()
    except subprocess.CalledProcessError as error:sys.exit(error.returncode)
