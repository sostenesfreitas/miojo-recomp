"""A GPU without mesh shaders (RDNA1, issue 640) runs task and mesh programs as compute shaders.

The console writes a mesh program's vertices and primitives to an internal stage buffer, and Vulkan's
VK_EXT_mesh_shader hands them to the rasterizer from there. A replay only needs the records: here the
task and mesh programs are ordinary compute shaders, their epilogues write the vertices to a record
buffer, and a generated vertex stage (drawing an ordinary graphics pipeline) reads one corner a
vertex from it. This checks that path end to end in the sources: the device enables it with buffer
device addresses, the translator builds the compute stages and the generated vertex stage, the
dispatches run before the pass, and the draw uses the GPU-written record count.

python -I -B tests/test_mesh_fallback.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

def _with_cache(path):
    """vk_shader.cpp with the program cache it held before (vk_progcache.cpp and .h beside it, outside the translator stamp): definitions first"""
    parts = [path.read_text(encoding="utf-8")]
    for name in ("vk_progcache.cpp", "vk_progcache.h"):
        if (path.parent / name).exists():
            parts.append((path.parent / name).read_text(encoding="utf-8"))
    return "\n".join(parts)

VK = ROOT / "gpu" / "vk"
SHADER = VK / "vk_shader.cpp"
DRAW = VK / "vk_draw.cpp"
MAIN = VK / "vk_main.cpp"
HEADER = VK / "vk_shader.h"
EMIT = VK / "eden" / "shader_recompiler" / "backend" / "spirv" / "emit_spirv.cpp"
MESH = VK / "eden" / "shader_recompiler" / "backend" / "spirv" / "emit_spirv_mesh.cpp"
SPIRV = VK / "eden" / "shader_recompiler" / "backend" / "spirv" / "emit_spirv.h"
VMA = VK / "vk_vma.cpp"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def main():
    main_cpp = MAIN.read_text(encoding="utf-8")
    if "dev.mesh_fallback = !dev.mesh_shader && f12.bufferDeviceAddress" not in main_cpp:
        raise SystemExit("vk_main.cpp: no fallback without buffer device addresses")
    if "issue 640 fallback" not in main_cpp:
        raise SystemExit("vk_main.cpp: the fallback is not logged")
    if "if (!dev.mesh_shader && !dev.mesh_fallback" not in main_cpp:
        raise SystemExit("vk_main.cpp: the no-mesh-shader warning ignores the fallback")

    program = block(HEADER.read_text(encoding="utf-8"), "struct Program {")
    for field in ("bool fallback", "bool fallback_task", "fallback_topology", "fallback_vertices_per_workgroup",
                  "fallback_record_words", "fallback_payload_words", "fallback_task_threads"):
        if field not in program:
            raise SystemExit("Program has no %s" % field)

    mesh_shader = _with_cache(SHADER)
    translate = block(mesh_shader, "void translate_mesh_into(")
    if "rt.mesh_fallback = fallback;" not in translate or "mrt.mesh_fallback = fallback;" not in translate:
        raise SystemExit("the task and mesh stages are not emitted as fallback stages")
    if "vrt.mesh_fallback_vertex = true;" not in translate or "vrt.mesh_fallback = true;" not in translate:
        raise SystemExit("no generated vertex stage")
    if "mesh_t->program.stage = Shader::Stage::VertexB;" not in translate:
        raise SystemExit("the generated vertex stage is not emitted as a vertex stage")
    if "mesh_t->program.info = Shader::Info{};" not in translate or \
            "mesh_t->program.info.stores = saved_info.stores;" not in translate:
        raise SystemExit("the generated vertex stage is not emitted with only the mesh program's stores")
    if "v.info = mesh_t->program.info;" in translate:
        raise SystemExit("the vertex stage still declares the mesh program's descriptors")
    if "vrt.per_primitive_generics.set(" not in translate:
        raise SystemExit("the generated vertex stage does not know the mesh program's per-primitive generics")
    if "fs->program.info.interpolation[" not in translate or "Shader::Interpolation::Flat" not in translate:
        raise SystemExit("the fallback fragment stage still declares PerPrimitiveEXT")
    if "VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_TASK_BIT_EXT" not in translate or \
            "VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_MESH_BIT_EXT" not in translate:
        raise SystemExit("the task and mesh stages are not compute stages in the fallback")
    if "p.fallback_task_threads = task_t ? task_t->program.workgroup_size[0] : 1;" not in translate:
        raise SystemExit("the task's threads are not kept for the mesh slot range")

    emit = EMIT.read_text(encoding="utf-8")
    entry = block(emit, "void DefineEntryPoint(")
    if "spv::ExecutionModel::GLCompute" not in entry or "ctx.mesh_fallback" not in entry:
        raise SystemExit("the fallback stages have no GLCompute entry point")
    if "spv::ExecutionModel::MeshEXT" not in entry or "spv::ExecutionModel::TaskEXT" not in entry:
        raise SystemExit("native mesh shaders were removed")

    mesh = MESH.read_text(encoding="utf-8")
    if "void MeshFallbackEpilogue(" not in mesh or "void EmitMeshFallbackVertexBody(" not in mesh:
        raise SystemExit("the fallback epilogue or vertex body is missing")
    if "AddLocalVariable" in block(mesh, "void Strided("):
        raise SystemExit("the strided copies declare a function variable mid-function (spirv-val rejects it)")
    task = block(mesh, "void TaskEpilogue(")
    if "ctx.OpULessThan(ctx.U1, LocalInvocation(ctx), count)" not in task:
        raise SystemExit("a task starts a mesh workgroup for every thread, not only the meshlets it kept")
    if "PushU64(ctx, MESH_FALLBACK_" in mesh:
        raise SystemExit("a push constant byte offset is used as the push block's member index")
    # (a started meshlet's map entry is its slot, a u32 word: a bool stored there is invalid SPIR-V, which RADV rejects)
    if "ctx.mesh_fallback_payload_member_index), index), slot);" not in task:
        raise SystemExit("a started meshlet does not write its slot, a u32 word, into the map")
    if "spv::MemoryAccessMask::Aligned, 4U" not in block(mesh, "Id LoadWord(") or \
            "spv::MemoryAccessMask::Aligned, 4U" not in block(mesh, "void StoreWord("):
        raise SystemExit("the fallback's PhysicalStorageBuffer accesses do not say their alignment")
    epilogue = block(mesh, "void MeshFallbackEpilogue(")
    # (records are taken a primitive's corners at a time from one area, and the draw's vertex count is theirs: a slot's worst
    # case each was ~650 MB for the floor grass's ~13900 slots)
    if "atomic_add(MESH_FALLBACK_ALLOC_WORD, taken)" not in epilogue or \
            "atomic_add(MESH_FALLBACK_DRAW_WORD, taken)" not in epilogue:
        raise SystemExit("a mesh workgroup does not take its records and add them to the draw's count")
    if "MESH_FALLBACK_CAPACITY" not in epilogue:
        raise SystemExit("a mesh workgroup past the slots is not left out")
    isbe = block(mesh, "Id EmitIsbeRead(")
    if "mesh_fallback_task_threads" not in isbe or "ctx.OpUMod" not in isbe:
        raise SystemExit("a fallback mesh workgroup takes its slot, not its meshlet within the task workgroup, as its index")
    if "mrt.mesh_fallback_task_threads = task_t ? task_t->program.workgroup_size[0] : 0;" not in translate:
        raise SystemExit("the mesh stage is not told its task program's threads")
    body = block(mesh, "void EmitMeshFallbackVertexBody(")
    if "ctx.Const(MESH_FALLBACK_HEADER_WORDS)" not in body or "ctx.vertex_index" not in body:
        raise SystemExit("the vertex stage does not read the record of its VertexIndex past the header")
    if "ctx.OpIEqual(ctx.U1, valid, ctx.u32_zero_value)" not in body:
        raise SystemExit("a vertex past its workgroup's primitive count is not culled")
    # (an output component the mesh program does not write is (0,0,0,1), as the native mesh stage's: undefined, AMD drew
    # specks of colour in Kokiri Forest's bushes)
    if "const Id value{element == 3 ? ctx.Const(1.0f) : ctx.f32_zero_value};" not in body or \
            "ctx.OpStore(ctx.output_point_size, ctx.Const(1.0f));" not in body:
        raise SystemExit("the generated vertex stage leaves the outputs the mesh program does not write undefined")
    if "per_primitive_generics" not in body or "spv::Decoration::Flat" not in body:
        raise SystemExit("the generated vertex stage does not declare its per-primitive outputs Flat")

    spirv = SPIRV.read_text(encoding="utf-8")
    if "MESH_FALLBACK_CAPACITY" not in spirv or "MESH_FALLBACK_PAYLOAD_PUSH_OFFSET" not in spirv or \
            "MESH_FALLBACK_RECORDS_PUSH_OFFSET" not in spirv:
        raise SystemExit("the fallback's push constants and capacity are not declared")
    if "MESH_FALLBACK_COUNTER_PUSH_OFFSET" in spirv:
        raise SystemExit("the removed atomic counter is still pushed")
    if "MESH_FALLBACK_CAPACITY = 65535;" not in spirv:
        raise SystemExit("the fallback no longer covers the indirect task lists in Hyrule Field")

    draw = DRAW.read_text(encoding="utf-8")
    if "bool fallback_record(VkCommandBuffer cmd, Program& p, const DrawArgs& d)" not in draw:
        raise SystemExit("vk_draw.cpp has no fallback dispatch")
    if "vkCmdDispatchIndirect(cmd, mesh_conv_buf, mesh_conv_off);" not in draw:
        raise SystemExit("an indirect mesh draw does not dispatch from its converted arguments")
    if "batch.first_task += (uint32_t)batch.groups[0];" not in draw or \
            "push_constants(cmd, p, nullptr, d.first_task);" not in draw or \
            "LoadWord(ctx, BufferWordAt(ctx, base, ctx.Const(1U)))" not in isbe:
        raise SystemExit("a task draw loses its direct or GPU-provided starting workgroup")
    if "if (p.fallback && d.mesh && !fallback_record(cmd, p, d)) {" not in draw:
        raise SystemExit("the fallback dispatches do not run before the pass")
    if "vkCmdDrawIndirect(cmd, fallback_draw_records, 0, 1, sizeof(VkDrawIndirectCommand));" not in draw:
        raise SystemExit("the fallback is not drawn from the records' header as an indirect draw")
    if "fallback_payload_address" not in draw or "MESH_FALLBACK_PAYLOAD_PUSH_OFFSET" not in draw:
        raise SystemExit("the fallback's buffer addresses are not pushed")
    # (a task's mesh workgroups are dispatched from the count its started threads take in the records' header, each writing
    # its slot to the map: DrawMeshTasksIndirect records no task count, and groups * threads was 0, so no mesh workgroup ran
    # and the floor grass was not drawn. A mesh workgroup reads its slot from the map in the entry block)
    record = block(draw, "bool fallback_record(VkCommandBuffer cmd, Program& p, const DrawArgs& d) {")
    if "MESH_FALLBACK_DISPATCH_WORD" not in task:
        raise SystemExit("a task's started threads do not count the mesh dispatch")
    if "vkCmdDispatchIndirect(cmd, f->records, (VkDeviceSize)Shader::Backend::SPIRV::MESH_FALLBACK_DISPATCH_WORD * 4)" not in record:
        raise SystemExit("a task program's mesh workgroups are not dispatched from the count the tasks took")
    if "ctx.mesh_fallback_slot =" not in block(mesh, "void EmitMeshPrologue(EmitContext& ctx)"):
        raise SystemExit("a mesh workgroup does not read its slot from its task's map")
    if "ctx.OpUDiv(ctx.U32[1], FallbackSlot(ctx), ctx.Const(ctx.runtime_info.mesh_fallback_task_threads))" not in mesh:
        raise SystemExit("a mesh workgroup does not read its task workgroup's payload")
    get_set = (EMIT.parent / "emit_spirv_context_get_set.cpp").read_text(encoding="utf-8")
    if "mesh_fallback_task_threads" not in block(get_set, "Id EmitWorkgroupId(EmitContext& ctx)"):
        raise SystemExit("a fallback mesh workgroup's id is its slot, not its index in its task")
    # (the records' header is reset after a barrier: the shared pair's last draw, of any program, has read it)
    reset = record[record.index("end_pass(cmd);"):record.index("vkCmdFillBuffer(cmd, f->records")]
    if "full_barrier(cmd);" not in reset:
        raise SystemExit("a fallback draw resets the shared records before the last draw has read them")
    if "vkCmdFillBuffer(cmd, f->records, 4, 4, 1);" not in record:
        raise SystemExit("the draw's instance count is not set to one")
    if "VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT" not in block(draw, "bool fallback_buffers_of(const Program& p, FallbackBuffers*& out) {"):
        raise SystemExit("the records buffer cannot be read as indirect arguments")
    # (every fallback program a recording thread draws shares one pair of buffers: per-program pairs were ~2.2 GB of VRAM
    # on an RX 5500M and the cause of the fallback's lock-up without enough of it)
    if "TLS_OBJECT(fallback_buffers, std::vector<FallbackBuffers>)" not in draw:
        raise SystemExit("the fallback still makes a record and payload pair for every program")
    if "fallback_buffers()[&p]" in draw:
        raise SystemExit("the fallback buffers are still keyed by the program")
    if "(!p.fallback || dw().prog == &p)" not in draw or "dw().prog = &p;" not in draw:
        raise SystemExit("a fallback draw reusing its descriptors after a dispatch pushes the dispatch's at the compute bind point")
    if "VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT" not in VMA.read_text(encoding="utf-8"):
        raise SystemExit("the allocator does not make the fallback's buffers addressable")
    if "st.vk_stage & (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT)" \
            not in draw:
        raise SystemExit("the graphics pipeline still takes the compute stages")
    # (a GPU with mesh shaders: the task and mesh stages are the graphics pipeline's own. Left out of a native mesh program,
    # its pipeline was its fragment stage alone, drew none of the grass, leaves or terrain and then lost the device)
    graphics = block(draw, "VkPipeline build_graphics(const Program& p, const NvnState& s, const Targets& at, "
                           "VkPrimitiveTopology topo, int part, bool* cold) {")
    if "if (p.fallback && (st.vk_stage & (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_TASK_BIT_EXT |" \
            not in graphics:
        raise SystemExit("build_graphics leaves out the task and mesh stages of native mesh programs too")
    if "dynamic_state(cmd, s, at, topo, d.mesh && !p.fallback);" not in draw:
        raise SystemExit("a fallback draw does not use the mesh program's topology")
    if "return p.fallback ? p.fallback_topology : drawn;" not in draw or "topo = pipeline_topology(p, topo);" not in draw:
        raise SystemExit("the fallback's points, lines or triangles are not used")
    runtime_mesh = block(mesh_shader, "const Program* mesh_program_locked(")
    if "p->fallback != fallback || p->mesh == fallback" not in runtime_mesh:
        raise SystemExit("a cached program from a GPU with the other mesh path is used")
    if "dev.uses_mesh_fallback(task != nullptr)" not in runtime_mesh or \
            mesh_shader.count("dev.uses_mesh_fallback(by_stage[6] != nullptr)") != 2:
        raise SystemExit("the runtime and both shader preparation paths disagree about task/mesh fallback")
    # (the fallback's compute pipelines are compiled on a worker, not in the draw: AMD took up to 12 s on one and the game froze
    # behind it. And before play, in the precompile, with every program translated for the fallback: white foliage until then)
    if "bool fallback_pipelines_ready(Program& p) {" not in draw:
        raise SystemExit("the fallback's compute pipelines are compiled in the draw, not on a worker (AMD: seconds each)")
    ready = block(draw, "bool fallback_pipelines_ready(Program& p) {")
    if "compare_exchange_strong(none, 1)" not in ready or "jobs_push(std::move(build), true, nullptr, failed)" not in ready or \
            "fallback_compute_pipelines(*job);" not in ready:
        raise SystemExit("the fallback's compute pipelines are compiled in the draw, not on a worker (AMD: seconds each)")
    record = block(draw, "bool fallback_record(VkCommandBuffer cmd, Program& p, const DrawArgs& d) {")
    if "fallback_pipelines_ready(p)" not in record or "fallback_compute_pipelines(p)" in record:
        raise SystemExit("a fallback draw compiles its compute pipelines itself")
    build = block(draw, "bool fallback_compute_pipelines(Program& p) {")
    if build.index("cached_compute(") < build.index("    }\n", build.index("std::lock_guard lk(mu);")):
        raise SystemExit("the fallback's build holds its lock through the compiles (one program at a time)")
    precompile = block(draw, "void precompile_run(void (*progress)(size_t done, size_t total)) {")
    if "else if (p.fallback) fallbacks.push_back(&p);" not in precompile or "fallback_compute_pipelines(p);" not in precompile:
        raise SystemExit("the precompile leaves the mesh fallback's compute pipelines to play")
    shader = _with_cache(ROOT / "gpu" / "vk" / "vk_shader.cpp")
    if "(fallbacks && (it->second[0].kind & 4))" not in block(shader, "size_t program_cache_load_some(") or \
            "}, plan.all, true);" not in precompile:
        raise SystemExit("the precompile loads only the mesh fallback programs play has drawn (white until compiled)")
    print("mesh shader fallback (compute records and a vertex stage) ok")


if __name__ == "__main__":
    main()
