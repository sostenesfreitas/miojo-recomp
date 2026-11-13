"""A mesh program's stage buffer (its map and attribute regions in workgroup memory) starts at zero.

Workgroup memory holds what the last workgroup left. A vertex or an index byte the game's mesh program leaves unwritten
was that, and on AMD it drew long thin triangles across the foliage's shadow map. The prologue zeroes both regions, with
straight-line stores (its entry block is a phi predecessor: no branch), before the primitive count and the barrier.
RX_VK_MESH_NO_ZERO turns it off. python -I -B tests/test_mesh_stage_buffer_zero.py
"""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "gpu" / "vk" / "eden" / "shader_recompiler" / "backend" / "spirv" / "emit_spirv_mesh.cpp"


def main():
    source = SOURCE.read_text(encoding="utf-8")
    prologue = block(source, "void EmitMeshPrologue(")
    mesh = prologue[prologue.index("if (ctx.stage != Stage::Mesh)"):]
    for name, size in (("ctx.mesh_map", "ctx.mesh_map_bytes"), ("ctx.mesh_attributes", "ctx.mesh_attribute_words")):
        if "zero_all(%s, %s)" % (name, size) not in mesh:
            raise SystemExit("the mesh prologue does not zero %s" % name)
    zero_at = mesh.index("zero_all(ctx.mesh_map")
    count_at = mesh.index("ctx.OpStore(ctx.mesh_primitive_count, ctx.u32_zero_value)")
    barrier_at = mesh.index("Barrier(ctx)", count_at)
    if not zero_at < count_at < barrier_at:
        raise SystemExit("the zeroing must come before the count and its barrier")
    zero_fn = block(mesh, "const auto zero_all = [&]")
    if "If(ctx" in zero_fn or "OpBranch" in zero_fn or "OpSelectionMerge" in zero_fn:
        raise SystemExit("the zeroing branches: the prologue's entry block is a phi predecessor")
    if "Clamped(ctx," not in zero_fn:
        raise SystemExit("the zeroing stores are not clamped into their arrays")
    if 'env_on("RX_VK_MESH_NO_ZERO")' not in mesh:
        raise SystemExit("no RX_VK_MESH_NO_ZERO switch")
    print("mesh stage buffer zeroed in the prologue ok")


if __name__ == "__main__":
    main()
