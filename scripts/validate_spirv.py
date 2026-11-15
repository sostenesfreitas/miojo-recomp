"""The SPIR-V Hookshot's translator kept in hookshot_programs.bin, checked offline before a driver sees it again.

Each stage goes through the Vulkan SDK's spirv-val, and through the device limits spirv-val does not know: the workgroup's
invocations and its workgroup memory (the mesh fallback keeps a mesh program's outputs there, on top of the game's own
shared memory). A stage that fails is written to --out as <key>_<n>_<stage>.spv, its disassembly (.spvasm, spirv-dis) and
the reasons (.txt); the summary groups the validator's errors by message.

python -I scripts/validate_spirv.py "D:\\Games\\Hookshot\\cache\\hookshot_programs.bin"    (the mesh-as-compute programs)
python -I scripts/validate_spirv.py <file> --all                                        (every program)
python -I scripts/validate_spirv.py <file> --key a5da9f84b72d1471                       (the programs whose key starts so)

The exit status is 1 when a stage failed, 0 when none did.
"""
import argparse
import collections
import concurrent.futures
import os
import re
import shutil
import struct
import subprocess
import sys

CACHE_MAGIC = 0x31505348   # (gpu/vk/vk_shader.cpp kDiskMagic)
FALLBACK = 4               # (a record's kind: a task or mesh program run as compute, issue 640)
SPV_MAGIC = b"\x03\x02\x23\x07"
STAGES = {0x1: "vertex", 0x2: "tesscontrol", 0x4: "tesseval", 0x8: "geometry", 0x10: "fragment", 0x20: "compute",
          0x40: "task", 0x80: "mesh"}


def records(path, want):
    """(key, kind, bytes) of each record want(key, kind) takes, in file order. The layout is disk_init's: a 16-byte head
    (magic, 0, stamp), then records of a u64 key, a u64 size and that many bytes, which start with the u32 kind. A record
    cut short ends the walk, as it does there"""
    with open(path, "rb") as f:
        head = f.read(16)
        if len(head) < 16 or struct.unpack_from("<I", head)[0] != CACHE_MAGIC:
            raise SystemExit("%s: not a Hookshot program cache" % path)
        while True:
            rec = f.read(16)
            if len(rec) < 16:
                return
            key, size = struct.unpack("<QQ", rec)
            at = f.tell()
            kind = f.read(4)
            if len(kind) < 4:
                return
            kind = struct.unpack("<I", kind)[0]
            if want(key, kind):
                f.seek(at)
                data = f.read(size)
                if len(data) < size:
                    return
                yield key, kind, data
            f.seek(at + size)


def stages(data):
    """(nvn stage, vk stage, SPIR-V bytes) of each stage of a record. program_cache_save writes a stage as int nvn_stage,
    VkShaderStageFlagBits vk_stage, then the SPIR-V as a list (a u64 word count, the words): each is found by its magic
    word and kept when the three fields before it agree, so the record's other fields (the answers, Shader::Info) need no
    layout of their own here"""
    out = []
    i = data.find(SPV_MAGIC)
    while i != -1:
        if i >= 16:
            nvn, vk = struct.unpack_from("<iI", data, i - 16)
            n = struct.unpack_from("<Q", data, i - 8)[0]
            if 0 <= nvn <= 7 and vk in STAGES and 5 <= n and i + 4 * n <= len(data):
                out.append((nvn, vk, data[i:i + 4 * n]))
                i = data.find(SPV_MAGIC, i + 4 * n)
                continue
        i = data.find(SPV_MAGIC, i + 1)
    return out


def reflect(spirv):
    """(local size or None, workgroup memory bytes) of a module. The bytes are its Workgroup variables' types summed with
    no padding: a lower bound of what the driver lays out, so a module over the limit by it is over by any layout"""
    w = struct.unpack("<%dI" % (len(spirv) // 4), spirv[:len(spirv) // 4 * 4])
    size, const, pointee = {}, {}, {}
    local, local_ids, shared = None, None, 0
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if n == 0 or i + n > len(w):
            break
        a = w[i + 1:i + n]
        if op == 16 and len(a) >= 5 and a[1] == 17:           # OpExecutionMode LocalSize x y z
            local = tuple(a[2:5])
        elif op == 331 and len(a) >= 5 and a[1] == 38:        # OpExecutionModeId LocalSizeId (constants defined later)
            local_ids = a[2:5]
        elif op in (43, 50) and len(a) >= 3:                  # OpConstant, OpSpecConstant (its default): the low word
            const[a[1]] = a[2]
        elif op == 20:                                        # OpTypeBool
            size[a[0]] = 4
        elif op in (21, 22):                                  # OpTypeInt, OpTypeFloat: width in bits
            size[a[0]] = a[1] // 8
        elif op in (23, 24):                                  # OpTypeVector, OpTypeMatrix: component type, count
            size[a[0]] = size.get(a[1], 0) * a[2]
        elif op == 28:                                        # OpTypeArray: element type, length constant
            size[a[0]] = size.get(a[1], 0) * const.get(a[2], 0)
        elif op == 30:                                        # OpTypeStruct: members
            size[a[0]] = sum(size.get(m, 0) for m in a[1:])
        elif op == 32:                                        # OpTypePointer: id, storage class, pointee
            pointee[a[0]] = a[2]
        elif op == 59 and len(a) >= 3 and a[2] == 4:          # OpVariable of storage class Workgroup
            shared += size.get(pointee.get(a[0]), 0)
        i += n
    if local_ids is not None:
        local = tuple(const.get(x, 0) for x in local_ids)
    return local, shared


def find_tool(name):
    """A Vulkan SDK tool: on PATH, else in %VULKAN_SDK%\\Bin"""
    found = shutil.which(name)
    if not found and os.environ.get("VULKAN_SDK"):
        candidate = os.path.join(os.environ["VULKAN_SDK"], "Bin", name + (".exe" if os.name == "nt" else ""))
        found = candidate if os.path.isfile(candidate) else None
    return found


def check(job, a, val):
    """The reasons a stage fails ([] when it passes): spirv-val's errors (on stdin), then the device limits"""
    key, nvn, vk, spirv = job
    reasons = []
    r = subprocess.run([val, "--target-env", a.target_env, *a.val_arg, "-"], input=spirv, capture_output=True)
    if r.returncode:
        text = (r.stdout + r.stderr).decode("utf-8", "replace").strip()
        reasons.append(("spirv-val", text or "spirv-val exit status %d" % r.returncode))
    local, shared = reflect(spirv)
    if local and local[0] * local[1] * local[2] > a.max_invocations:
        reasons.append(("limit", "workgroup %dx%dx%d is %d invocations, over maxComputeWorkGroupInvocations %d"
                        % (*local, local[0] * local[1] * local[2], a.max_invocations)))
    if shared > a.max_shared:
        reasons.append(("limit", "workgroup memory at least %d bytes, over maxComputeSharedMemorySize %d"
                        % (shared, a.max_shared)))
    return reasons, local, shared


def first_error(text):
    """A validator message with its numbers taken out, so the same fault in many stages counts as one"""
    line = next((x for x in text.splitlines() if x.startswith("error")), text.splitlines()[0] if text else "")
    return re.sub(r"\d+", "N", line).strip()


def dump(out, job, reasons, local, shared, dis):
    key, nvn, vk, spirv = job
    base = os.path.join(out, "%016x_%d_%s" % (key, nvn, STAGES[vk]))
    with open(base + ".spv", "wb") as f:
        f.write(spirv)
    with open(base + ".txt", "w", encoding="utf-8") as f:
        f.write("program %016x, NVN stage %d, %s stage, %d words\n" % (key, nvn, STAGES[vk], len(spirv) // 4))
        f.write("local size %s, workgroup memory at least %d bytes\n\n" % (local, shared))
        for kind, text in reasons:
            f.write("[%s]\n%s\n\n" % (kind, text))
    if dis:
        subprocess.run([dis, base + ".spv", "-o", base + ".spvasm"], capture_output=True)
    return base


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("cache", help="hookshot_programs.bin")
    p.add_argument("--all", action="store_true", help="every program, not only the mesh-as-compute ones")
    p.add_argument("--key", action="append", default=[], help="a program key (hex) or its start; may be given again")
    p.add_argument("--out", help="where failing stages are written (default: spirv_failures beside the cache)")
    p.add_argument("--target-env", default="vulkan1.3", help="spirv-val's target (Hookshot asks for Vulkan 1.3)")
    p.add_argument("--val-arg", action="append", default=[], help="an argument passed on to spirv-val")
    p.add_argument("--max-shared", type=int, default=65536, help="maxComputeSharedMemorySize (RDNA1: 65536)")
    p.add_argument("--max-invocations", type=int, default=1024, help="maxComputeWorkGroupInvocations (RDNA1: 1024)")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    a = p.parse_args(argv)
    val, dis = find_tool("spirv-val"), find_tool("spirv-dis")
    if not val:
        raise SystemExit("spirv-val not found: install the Vulkan SDK or put its Bin folder on PATH")
    keys = [k.lower().removeprefix("0x") for k in a.key]
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(a.cache)), "spirv_failures")

    def want(key, kind):
        if keys:
            return any(("%016x" % key).startswith(k) for k in keys)
        return a.all or bool(kind & FALLBACK)

    programs, failed, groups = 0, [], collections.defaultdict(list)
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as pool:
        pending = collections.deque()

        def settle(future):
            job, (reasons, local, shared) = future.job, future.result()
            if not reasons:
                return
            os.makedirs(out, exist_ok=True)
            base = dump(out, job, reasons, local, shared, dis)
            failed.append(base)
            for kind, text in reasons:
                groups[first_error(text) if kind == "spirv-val" else re.sub(r"\d+", "N", text)].append(
                    "%016x %s" % (job[0], STAGES[job[2]]))

        for key, kind, data in records(a.cache, want):
            programs += 1
            for nvn, vk, spirv in stages(data):
                job = (key, nvn, vk, spirv)
                future = pool.submit(check, job, a, val)
                future.job = job
                pending.append(future)
                while len(pending) > 4 * a.jobs:   # (a few batches in flight: --all reads gigabytes)
                    settle(pending.popleft())
        while pending:
            settle(pending.popleft())

    checked = "programs" if a.all or keys else "mesh-as-compute programs"
    print("%s: %d %s, %d stages failed" % (a.cache, programs, checked, len(failed)))
    for message, where in sorted(groups.items(), key=lambda g: -len(g[1])):
        print("  %4dx  %s" % (len(where), message))
        print("         e.g. %s" % ", ".join(where[:3]))
    if failed:
        print("written to %s (.spv, .spvasm, .txt)" % out)
    if not programs:
        print("  (no records matched%s)" % (": is the key right?" if keys else ""))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
