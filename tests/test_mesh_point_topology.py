"""A mesh program whose NVN2 control section says topology 1 draws points, not lines.

OoT's four such mesh programs (the grass drawn into the 2400 x 2400 shadow map around Link) write one index byte per
primitive, their vertex count as the primitive count, and a point size. Read as lines, the epilogue took two index
bytes per primitive and joined vertex 2i to 2i + 1 of each meshlet: long thin stripes across the grass, a dark mesh on
the ground around Link (issue 302). This checks the whole path: the control section's 1 becomes GL's 0 (points) in
vk_shader.cpp, the translator accepts it, a point has one index byte, the SPIR-V says OutputPoints and
PrimitivePointIndicesEXT, and the epilogue stores a point's index as a scalar.

Their fragment program reads generic 5 (s, t, r), which the mesh program never writes: the hardware's point sprite fills
it with the coordinate within the point, and the program cuts the point's shape out of an alpha mask with it. The
generics a point mesh program leaves unwritten are therefore gl_PointCoord (s, t, 0, 1), only for points.
python -I -B tests/test_mesh_point_topology.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EDEN = ROOT / "gpu" / "vk" / "eden" / "shader_recompiler"
SHADER = ROOT / "gpu" / "vk" / "vk_shader.cpp"
INFO = EDEN / "shader_info.h"
RUNTIME = EDEN / "runtime_info.h"
GET_SET = EDEN / "backend" / "spirv" / "emit_spirv_context_get_set.cpp"
TRANSLATE = EDEN / "frontend" / "maxwell" / "translate_program.cpp"
CONTEXT = EDEN / "backend" / "spirv" / "spirv_emit_context.cpp"
EMIT = EDEN / "backend" / "spirv" / "emit_spirv.cpp"
MESH = EDEN / "backend" / "spirv" / "emit_spirv_mesh.cpp"


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def ternary(expr, names):
    """Evaluates a C chain of `cond ? a : b` over integer names (no calls, no casts)."""
    expr = re.sub(r"\b(\d+)[uU]\b", r"\1", expr.strip())
    depth = 0
    for i, ch in enumerate(expr):
        if ch == "?" and depth == 0:
            nested = 0
            for j in range(i + 1, len(expr)):
                if expr[j] == "?":
                    nested += 1
                elif expr[j] == ":":
                    if nested == 0:
                        cond, then, other = expr[:i], expr[i + 1:j], expr[j + 1:]
                        taken = eval(cond, {"__builtins__": {}}, names)  # noqa: S307 (a comparison of names)
                        return ternary(then if taken else other, names)
                    nested -= 1
            raise SystemExit("unbalanced ?: in %r" % expr)
        depth += (ch == "(") - (ch == ")")
    return eval(expr, {"__builtins__": {}}, names)  # noqa: S307


def main():
    shader = SHADER.read_text(encoding="utf-8")
    registers = block(shader, "std::array<u32, 3> mesh_registers_of(")
    found = re.search(r"const uint32_t topology = ([^;]+);", registers)
    if not found:
        raise SystemExit("mesh_registers_of: no topology line")
    to_gl = {control: ternary(found.group(1).replace("mesh.topology", "t"), {"t": control}) for control in (1, 3)}
    if to_gl != {1: 0, 3: 4}:
        raise SystemExit("control topology 1 must be points (GL 0) and 3 triangles (GL 4), got %r" % to_gl)

    info = INFO.read_text(encoding="utf-8")
    corners_body = block(info, "u32 Corners() const")
    found = re.search(r"return ([^;]+);", corners_body)
    if not found:
        raise SystemExit("MeshInfo::Corners: no return")
    corners = {gl: ternary(found.group(1), {"topology": gl}) for gl in (0, 1, 4)}
    if corners != {0: 1, 1: 2, 4: 3}:
        raise SystemExit("index bytes per primitive must be points 1, lines 2, triangles 3, got %r" % corners)

    translate = TRANSLATE.read_text(encoding="utf-8")
    check = translate[translate.index("if (program.mesh.topology != 4"):]
    check = check[:check.index("throw NotImplementedException")]
    if "program.mesh.topology != 0" not in check:
        raise SystemExit("the translator still rejects point output (topology 0)")

    context = CONTEXT.read_text(encoding="utf-8")
    define = block(context, "void EmitContext::DefineMesh(")
    # Whole blocks of 32 primitives: the program writes the index bytes of every primitive block, lanes past max_primitives too
    if "mesh_map_bytes = 4 + mesh_info.Corners() * Common::DivCeil(mesh_info.max_primitives, 32U) * 32" not in define:
        raise SystemExit("the map region is not sized by the index bytes per primitive, in whole blocks of 32 primitives")
    if not re.search(r"corners == 1\s*\?\s*spv::BuiltIn::PrimitivePointIndicesEXT", define):
        raise SystemExit("points do not output PrimitivePointIndicesEXT")
    if "TypeArray(U32[corners], primitive_count)" not in define:
        raise SystemExit("the primitive index array is not one element per index")

    emit = EMIT.read_text(encoding="utf-8")
    mesh_case = emit[emit.index("case Stage::Mesh:"):]
    mesh_case = mesh_case[:mesh_case.index("break;")]
    if not re.search(r"Corners\(\) == 1\s*\?\s*spv::ExecutionMode::OutputPoints", mesh_case):
        raise SystemExit("point output does not declare OutputPoints")

    mesh = MESH.read_text(encoding="utf-8")
    epilogue = block(mesh, "void MeshEpilogue(")
    if "const u32 corners{mesh.Corners()};" not in epilogue:
        raise SystemExit("the epilogue does not read Corners() index bytes per primitive")
    if not re.search(r"corners == 1 \? indices\[0\]", epilogue):
        raise SystemExit("a point's index must be stored as a scalar (U32[1] is not a vector)")

    # The point sprite's coordinate in place of the generics a point mesh program leaves unwritten
    if "std::bitset<32> point_coord_generics{};" not in RUNTIME.read_text(encoding="utf-8"):
        raise SystemExit("RuntimeInfo has no point_coord_generics")
    mesh_translation = shader[shader.index("void translate_mesh_into("):]
    fragment = mesh_translation[mesh_translation.index("Shader::RuntimeInfo frt{};"):]
    fragment = fragment[:fragment.index("EmitSPIRV(")]
    found = re.search(r"if \(mesh_t->program\.mesh\.topology == 0\)(.*?)frt\.point_coord_generics\.set\(i\);", fragment, re.S)
    if not found:
        raise SystemExit("vk_shader.cpp: point_coord_generics is not set for point mesh programs only")
    if not re.search(r"fs->program\.info\.loads\.Generic\(i\)\s*&&\s*!mesh_t->program\.info\.stores\.Generic\(i\)",
                     found.group(1)):
        raise SystemExit("vk_shader.cpp: point_coord_generics must be what the fragment reads and the mesh never writes")
    inputs = block(context, "void EmitContext::DefineInputs(")
    if not re.search(r"runtime_info\.point_coord_generics\.any\(\)\)\)\s*\{\s*point_coord = DefineInput\(\*this, F32\[2\], "
                     r"true, spv::BuiltIn::PointCoord\);", inputs):
        raise SystemExit("PointCoord is not declared when a generic is replaced by it")
    get = block(GET_SET.read_text(encoding="utf-8"), "Id EmitGetAttribute(")
    generic = get[get.index("if (IR::IsGeneric(attr))"):]
    replaced = generic.find("point_coord_generics.test(index)")
    if replaced < 0 or replaced > generic.index("ctx.input_generics.at(index)"):
        raise SystemExit("a replaced generic must be read from PointCoord before its own input is looked up")
    body = block(generic, "if (ctx.stage == Stage::Fragment && ctx.runtime_info.point_coord_generics.test(index))")
    if "ctx.point_coord, ctx.Const(element)" not in body or "element < 2" not in body or \
            "element == 3 ? 1.0f : 0.0f" not in body:
        raise SystemExit("a replaced generic must be (PointCoord.s, PointCoord.t, 0, 1)")
    print("mesh point topology and point sprite coordinate ok")


if __name__ == "__main__":
    main()
