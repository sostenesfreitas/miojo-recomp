"""A float add to a storage buffer is one OpAtomicFAddEXT where the GPU has it and it is wanted (by default with NVIDIA's
driver only), and the compare-exchange loop elsewhere.

The game's RED/ATOM.ADD.F32 were always a compare-exchange loop (f32_add_cas), and every such loop gives up after
CAS_LOOP_MAX_TRIES attempts (issue 17's ceiling): when more invocations add to one word than that, the ones that give
up lose their add. Where the device has VK_EXT_shader_atomic_float's shaderBufferFloat32AtomicAdd (the profile's
support_float32_atomic_add, vk_main.cpp), the add is now the GPU's own atomic on an f32 view of the buffer.

DefineStorageBuffers' decision, the loop it builds otherwise, and EmitStorageAtomicAddF32 with its helpers are cut out
of the back end into a Sirit fixture: a compute shader that adds 1.0 to a word, made with the device's float add, without
it, and without descriptor aliasing (no f32 view to add through). Each must pass spirv-val; the first must be one
OpAtomicFAddEXT and no compare-exchange, the others the loop and no float-add capability. The device's half (the
extension asked for only when wanted, only that feature on, the profile and the program cache's stamp) and the
global-address fallback are checked in the source. Whether it is wanted (native_fadd_wanted, vk_main.cpp) is compiled
alone against a table: unset, NVIDIA's driver alone (not RADV, AMD's, Intel's or NVK); set, RX_VK_NATIVE_FADD by #2042's
rule on any driver, so =on is on and =off, =false, =no and empty are off. Nothing runs on a GPU.

    python -I -B tests/test_native_float_atomic_add.py --headers <SPIRV-Headers/include> [--cxx g++] [--spirv-val spirv-val]
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{spirv_headers}", "--spirv-val", "{spirv_val}"]
# (spirv-val: SPIRV-Tools comes with the Linux runner's packages, not on Windows)
RUN_ALL_SKIP = {"windows": "needs spirv-val: checked on Linux"}

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
SPIRV = VK / "eden" / "shader_recompiler" / "backend" / "spirv"


def block(text, marker, start=0):
    at = text.index(marker, start)
    end = text.index("{", at) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[at:end]


def cut(text, marker, what):
    if marker not in text:
        raise SystemExit("FAIL: %s: no '%s'" % (what, marker))
    return block(text, marker)


FIXTURE = r"""
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <utility>
#include <vector>
#include <sirit/sirit.h>
#include "shader_recompiler/profile.h"
using Sirit::Id;
struct NotImplementedException : std::runtime_error {
    template <typename... A> explicit NotImplementedException(const char* what, A&&...) : std::runtime_error(what) {}
};
namespace IR {
struct Value {   // (immediate binding and offset only: what the fixture's one add uses)
    u32 value;
    bool IsImmediate() const { return true; }
    u32 U32() const { return value; }
};
}
enum class Operation { Increment, Decrement, FPAdd, FPMin, FPMax };
struct StorageTypeDefinition { Id array{}; Id element{}; };
struct StorageTypes { StorageTypeDefinition U32, F32; };
struct StorageDefinitions { Id U32{}; Id F32{}; };
struct Info { bool uses_atomic_f32_add{}; };
struct EmitContext : Sirit::Module {
    using Sirit::Module::Module;
    Shader::Profile profile{};
    Id U1{};
    std::array<Id, 5> U32{}, F32{};
    Id u32_zero_value{};
    bool uses_explicit_workgroup_layout{};
    Id shared_memory_u32{};
    StorageTypes storage_types{};
    std::array<StorageDefinitions, 1> ssbos{};
    Id f32_add_cas{};
    bool native_f32_add{};
    Id Const(u32 value) { return Constant(U32[1], value); }
    Id Def(const IR::Value&) { return u32_zero_value; }
    void define_storage_buffers(const Info& info);
};
@@CAS@@
void EmitContext::define_storage_buffers(const Info& info) {
@@DECIDE@@
@@LOOP@@
}
@@ATOMIC@@
int main(int argc, char** argv) {
    if (argc < 4) return 2;
    EmitContext ctx{0x00010600};
    ctx.profile.support_float32_atomic_add = argv[2][0] == '1';
    ctx.profile.support_descriptor_aliasing = argv[3][0] == '1';
    ctx.AddCapability(spv::Capability::Shader);
    ctx.AddCapability(spv::Capability::VariablePointersStorageBuffer);   // (as DefineStorageBuffers for a program with a float add)
    ctx.AddExtension("SPV_KHR_storage_buffer_storage_class");
    ctx.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    const Id void_t{ctx.TypeVoid()};
    ctx.U1 = ctx.TypeBool();
    ctx.U32[1] = ctx.TypeInt(32, false);
    ctx.F32[1] = ctx.TypeFloat(32);
    ctx.F32[2] = ctx.TypeVector(ctx.F32[1], 2);
    ctx.u32_zero_value = ctx.Constant(ctx.U32[1], 0u);
    std::vector<Id> interfaces;
    const auto view = [&](StorageTypeDefinition& def, Id StorageDefinitions::*member, Id element) {   // (as DefineSsbos)
        const Id array{ctx.TypeRuntimeArray(element)};
        ctx.Decorate(array, spv::Decoration::ArrayStride, 4U);
        const Id block{ctx.TypeStruct(array)};
        ctx.Decorate(block, spv::Decoration::Block);
        ctx.MemberDecorate(block, 0, spv::Decoration::Offset, 0U);
        def.array = ctx.TypePointer(spv::StorageClass::StorageBuffer, block);
        def.element = ctx.TypePointer(spv::StorageClass::StorageBuffer, element);
        const Id var{ctx.AddGlobalVariable(def.array, spv::StorageClass::StorageBuffer)};
        ctx.Decorate(var, spv::Decoration::Binding, 0U);
        ctx.Decorate(var, spv::Decoration::DescriptorSet, 0U);
        ctx.ssbos[0].*member = var;
        interfaces.push_back(var);
    };
    view(ctx.storage_types.U32, &StorageDefinitions::U32, ctx.U32[1]);
    Info info{true};
    ctx.define_storage_buffers(info);
    if (ctx.native_f32_add) view(ctx.storage_types.F32, &StorageDefinitions::F32, ctx.F32[1]);   // (used_types |= F32)
    const Id main_id{ctx.OpFunction(void_t, spv::FunctionControlMask::MaskNone, ctx.TypeFunction(void_t))};
    ctx.AddLabel();
    Shader::Backend::SPIRV::EmitStorageAtomicAddF32(ctx, IR::Value{0}, IR::Value{8}, ctx.Constant(ctx.F32[1], 1.0f));
    ctx.OpReturn();
    ctx.OpFunctionEnd();
    ctx.AddEntryPoint(spv::ExecutionModel::GLCompute, main_id, "main", interfaces);
    ctx.AddExecutionMode(main_id, spv::ExecutionMode::LocalSize, 64u, 1u, 1u);
    const std::vector<u32> code{ctx.Assemble()};
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 3;
    std::fwrite(code.data(), 4, code.size(), f);
    std::fclose(f);
    std::printf("native %d\n", ctx.native_f32_add ? 1 : 0);
    return 0;
}
"""

OP_EXTENSION, OP_CAPABILITY, OP_ATOMIC_COMPARE_EXCHANGE, OP_ATOMIC_FADD = 10, 17, 230, 6035
CAPABILITY_ATOMIC_FLOAT32_ADD = 6033


def instructions(words):
    i = 5
    while i < len(words):
        n = words[i] >> 16
        yield words[i] & 0xFFFF, words[i + 1:i + n]
        i += n


def check_source():
    context = (SPIRV / "spirv_emit_context.cpp").read_text(encoding="utf-8")
    storage = cut(context, "void EmitContext::DefineStorageBuffers(", "spirv_emit_context.cpp")
    early = storage.index("if (info.storage_buffers_descriptors.empty())")
    if storage.find("native_f32_add = ") < 0 or storage.index("native_f32_add = ") > early:
        raise SystemExit("FAIL: DefineStorageBuffers does not decide native_f32_add before its early return "
                         "(the global-address fallback reads it too)")
    if "if (native_f32_add) {\n        used_types |= IR::Type::F32;" not in storage:
        raise SystemExit("FAIL: the native float add has no f32 view of the buffers to add through")
    start = context.index("atomic_global_add_f32_func = func;")
    fallback = context[context.rfind("if (info.stores_global_memory)", 0, start):start]
    for piece, why in (("define_body(&StorageDefinitions::F32", "searches no f32 view"),
                       ("OpAtomicFAddEXT(F32[1]", "has no native float add"),
                       ("native_add(psb_member(F32[1], target))", "does not add natively through the page table")):
        if piece not in fallback:
            raise SystemExit("FAIL: the global-address float add fallback %s" % why)
    pointer_of = cut(context, "const auto struct_pointer_of{", "spirv_emit_context.cpp")
    if "if (m == &StorageDefinitions::F32) return storage_types.F32.array;" not in pointer_of:
        raise SystemExit("FAIL: the f32 views are searched without their bound length (struct_pointer_of)")
    header = (SPIRV / "spirv_emit_context.h").read_text(encoding="utf-8")
    if "bool native_f32_add{};" not in header:
        raise SystemExit("FAIL: EmitContext has no native_f32_add")
    main_cpp = (VK / "vk_main.cpp").read_text(encoding="utf-8")
    device = cut(main_cpp, "bool create_device() {", "vk_main.cpp")
    for piece, why in (
        ("if (fadd_wanted) optional(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME, atomic_float);",
         "VK_EXT_shader_atomic_float is not asked for, or is asked for where the native add is not wanted"),
        ('getenv("RX_VK_NATIVE_FADD")', "RX_VK_NATIVE_FADD is not read"),
        ("const bool fadd_wanted = native_fadd_wanted(fadd_env, fadd_driver.driverID);",
         "the native add is not decided by native_fadd_wanted from the switch and the driver"),
        ("vkGetPhysicalDeviceProperties2(dev.physical, &p2);\n    }\n    const bool fadd_wanted",
         "the driver is not asked who it is before the native add is decided"),
        ("if (atomic_float) *tail = &afloat, tail = &afloat.pNext;", "the atomic float features are not queried"),
        ("dev.float_atomic_add = atomic_float && afloat.shaderBufferFloat32AtomicAdd;",
         "the native add does not follow shaderBufferFloat32AtomicAdd"),
        ("afloat = VkPhysicalDeviceShaderAtomicFloatFeaturesEXT{", "every atomic float feature the device has is enabled"),
        ("afloat.shaderBufferFloat32AtomicAdd = dev.float_atomic_add ? VK_TRUE : VK_FALSE;",
         "the one feature used is not enabled"),
    ):
        if piece not in device:
            raise SystemExit("FAIL: create_device: %s" % why)
    query, reset = device.index("vkGetPhysicalDeviceFeatures2(dev.physical, &f);"), device.index("afloat = VkPhysicalDevice")
    if not device.index("*tail = &afloat") < query < reset < device.index("vkCreateDevice("):
        raise SystemExit("FAIL: create_device: the features are not queried, then cut to the one used, before vkCreateDevice")
    shader = (VK / "vk_shader.cpp").read_text(encoding="utf-8")
    profile = shader[shader.index("Shader::Profile make_profile()"):shader.index("Shader::HostTranslateInfo host_info()")]
    if "p.support_float32_atomic_add = dev.float_atomic_add;" not in profile:
        raise SystemExit("FAIL: the translator is not told the device's float add (make_profile)")
    # (the program cache's stamp: program_cache_stamp, vk_progcache.cpp, hashes translator_stamp's, which says what the
    # translator is told; it was disk_stamp's before #2323)
    progcache = (VK / "vk_progcache.cpp").read_text(encoding="utf-8")
    if "translator_stamp()" not in cut(progcache, "uint64_t program_cache_stamp() {", "vk_progcache.cpp"):
        raise SystemExit("FAIL: the program cache's stamp no longer holds translator_stamp's: update this check")
    stamp = shader[shader.index("uint64_t translator_stamp() {"):]
    if "profile().support_float32_atomic_add" not in stamp[:stamp.index("return h;")]:
        raise SystemExit("FAIL: programs made with OpAtomicFAddEXT stay in the program cache for a GPU without it")


# RX_VK_NATIVE_FADD's value (None: unset), the driver (VkDriverId: the Vulkan specification's numbers), and whether the
# float add is then asked to be the GPU's own: unset, NVIDIA's driver alone; set, #2042's rule (hookshot_env.h
# env_value_off) whatever the driver
DRIVERS = {"AMD_PROPRIETARY": 1, "MESA_RADV": 3, "NVIDIA_PROPRIETARY": 4, "INTEL_PROPRIETARY_WINDOWS": 5,
           "INTEL_OPEN_SOURCE_MESA": 6, "MESA_NVK": 24}
SWITCH_CASES = ([(None, "NVIDIA_PROPRIETARY", 1)] +
                [(None, d, 0) for d in DRIVERS if d != "NVIDIA_PROPRIETARY"] +
                [(v, d, 1) for v in ("1", "on", "yes", "true", "2") for d in ("MESA_RADV", "NVIDIA_PROPRIETARY")] +
                [(v, "AMD_PROPRIETARY", 1) for v in ("1", "on")] + [("1", "INTEL_OPEN_SOURCE_MESA", 1)] +
                [(v, "NVIDIA_PROPRIETARY", 0) for v in ("0", " 0 ", "off", "OFF", "false", "no", "")] +
                [("0", "MESA_RADV", 0), ("off", "AMD_PROPRIETARY", 0)])
SWITCH_FIXTURE = r"""
#include <cstdio>
#include <cstdlib>
#include <cstring>
enum VkDriverId { @@DRIVERS@@ };
@@WANTED@@
int main(int argc, char** argv) {
    for (int i = 1; i + 2 < argc; i += 3) {   // (argv: value or -unset, the driver's number, 1 native or 0 the loop, ...)
        const char* v = strcmp(argv[i], "-unset") ? argv[i] : nullptr;
        const VkDriverId driver = static_cast<VkDriverId>(atoi(argv[i + 1]));
        if (native_fadd_wanted(v, driver) != (atoi(argv[i + 2]) != 0)) {
            printf("FAIL: RX_VK_NATIVE_FADD=[%s], driver %d: %s\n", v ? v : "(unset)", static_cast<int>(driver),
                   native_fadd_wanted(v, driver) ? "native" : "the loop");
            return 1;
        }
    }
    return 0;
}
"""


def check_switch(cxx, tmp):
    main_cpp = (VK / "vk_main.cpp").read_text(encoding="utf-8")
    wanted = cut(main_cpp, "static bool native_fadd_wanted(", "vk_main.cpp")
    cpp, exe = tmp / "switch.cpp", tmp / "switch"
    drivers = ", ".join("VK_DRIVER_ID_%s = %d" % kv for kv in DRIVERS.items())
    cpp.write_text(SWITCH_FIXTURE.replace("@@DRIVERS@@", drivers).replace("@@WANTED@@", wanted), encoding="utf-8")
    r = subprocess.run([cxx, "-std=c++20", "-include", str(VK / "compat" / "hookshot_env.h"), "-Wall", "-Wextra", "-Werror",
                        str(cpp), "-o", str(exe)], capture_output=True, text=True, timeout=120)
    if r.returncode:
        print(r.stdout + r.stderr)
        raise SystemExit("FAIL: native_fadd_wanted did not build alone")
    argv = []
    for value, driver, want in SWITCH_CASES:
        argv += ["-unset" if value is None else value, str(DRIVERS[driver]), str(want)]
    r = subprocess.run([str(exe), *argv], capture_output=True, text=True, timeout=30)
    if r.returncode:
        print(r.stdout + r.stderr)
        raise SystemExit("FAIL: the native float add is not wanted as it should be (NVIDIA's driver by default, "
                         "RX_VK_NATIVE_FADD by the switches' rule)")
    print("switch: %d cases: unset, NVIDIA's driver alone; RX_VK_NATIVE_FADD by the switches' rule on any driver"
          % len(SWITCH_CASES))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--headers", type=Path, required=True)
    parser.add_argument("--cxx", default="g++")
    parser.add_argument("--spirv-val", default="spirv-val")
    args = parser.parse_args()
    check_source()
    context = (SPIRV / "spirv_emit_context.cpp").read_text(encoding="utf-8")
    atomic = (SPIRV / "emit_spirv_atomic.cpp").read_text(encoding="utf-8")
    storage = cut(context, "void EmitContext::DefineStorageBuffers(", "spirv_emit_context.cpp")
    decide = storage[storage.index("    native_f32_add = "):storage.index("if (info.storage_buffers_descriptors.empty())")]
    loop = cut(storage, "if (info.uses_atomic_f32_add && !native_f32_add) {", "DefineStorageBuffers")
    ceiling = context[context.index("constexpr u32 CAS_LOOP_MAX_TRIES"):]
    cas = "\n".join([cut(context, "Id CasFunction(EmitContext& ctx, Operation operation, Id value_type)", "CasFunction"),
                     ceiling[:ceiling.index("\n") + 1],
                     cut(context, "Id CasTriesVariable(EmitContext& ctx)", "CasTriesVariable"),
                     cut(context, "Id CasLoopDone(EmitContext& ctx", "CasLoopDone"),
                     cut(context, "Id CasLoop(EmitContext& ctx", "CasLoop")])
    helpers = "\n".join(cut(atomic, marker, "emit_spirv_atomic.cpp") for marker in (
        "Id StorageIndex(EmitContext& ctx", "Id StoragePointer(EmitContext& ctx", "std::pair<Id, Id> AtomicArgs(EmitContext& ctx",
        "Id EmitStorageAtomicAddF32(EmitContext& ctx"))
    helpers = "namespace Shader::Backend::SPIRV {\n" + helpers + "\n}\n"
    fixture = (FIXTURE.replace("@@CAS@@", cas).replace("@@DECIDE@@", decide).replace("@@LOOP@@", "    " + loop)
               .replace("@@ATOMIC@@", helpers))
    sirit = VK / "sirit"
    with tempfile.TemporaryDirectory(prefix="native-float-add-") as tmp:
        tmp = Path(tmp)
        check_switch(args.cxx, tmp)
        cpp, exe = tmp / "fixture.cpp", tmp / "fixture"
        cpp.write_text(fixture, encoding="utf-8")
        r = subprocess.run([args.cxx, "-std=c++20", "-O1", "-I" + str(sirit / "include"), "-I" + str(sirit / "src"),
                            "-I" + str(args.headers), "-I" + str(VK / "eden"), "-I" + str(VK / "compat"), str(cpp),
                            *map(str, sorted((sirit / "src").rglob("*.cpp"))), "-o", str(exe)],
                           capture_output=True, text=True, timeout=600)
        if r.returncode:
            print(r.stdout + r.stderr)
            raise SystemExit("FAIL: the fixture did not build")
        for name, device_add, aliasing, native in (("native", 1, 1, True), ("loop", 0, 1, False),
                                                   ("no aliasing", 1, 0, False)):
            spv = tmp / ("%s.spv" % name.replace(" ", "_"))
            r = subprocess.run([str(exe), str(spv), str(device_add), str(aliasing)], capture_output=True, text=True, timeout=30)
            if r.returncode or r.stdout.split() != ["native", "1" if native else "0"]:
                print(r.stdout + r.stderr)
                raise SystemExit("FAIL: %s: the fixture stopped or decided otherwise" % name)
            data = spv.read_bytes()
            ops = list(instructions(list(struct.unpack("<%dI" % (len(data) // 4), data))))
            fadds = sum(op == OP_ATOMIC_FADD for op, _ in ops)
            exchanges = sum(op == OP_ATOMIC_COMPARE_EXCHANGE for op, _ in ops)
            capability = any(op == OP_CAPABILITY and operands == [CAPABILITY_ATOMIC_FLOAT32_ADD] for op, operands in ops)
            extension = any(op == OP_EXTENSION and b"SPV_EXT_shader_atomic_float_add" in struct.pack("<%dI" % len(operands), *operands)
                            for op, operands in ops)
            if native and (fadds != 1 or exchanges or not capability or not extension):
                raise SystemExit("FAIL: with the device's float add: %d OpAtomicFAddEXT, %d compare-exchanges, capability %s, "
                                 "extension %s (want 1, 0, yes, yes)" % (fadds, exchanges, capability, extension))
            if not native and (fadds or not exchanges or capability or extension):
                raise SystemExit("FAIL: %s: %d OpAtomicFAddEXT, %d compare-exchanges, capability %s, extension %s "
                                 "(want the loop and no float add)" % (name, fadds, exchanges, capability, extension))
            r = subprocess.run([args.spirv_val, "--target-env", "vulkan1.3", str(spv)], capture_output=True, text=True,
                               timeout=60)
            if r.returncode:
                print(r.stdout + r.stderr)
                raise SystemExit("FAIL: %s: spirv-val" % name)
            print("%s: %d OpAtomicFAddEXT, %d compare-exchange, spirv-val ok" % (name, fadds, exchanges))
    print("PASS: a float add is the GPU's own atomic where it has one, the loop elsewhere; the device and cache follow")


if __name__ == "__main__":
    main()
