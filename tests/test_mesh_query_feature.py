"""The draw statistics capture asks for task and mesh shader counts only where the driver offers them.

stats_make_pool (gpu/vk/vk_draw.cpp) made its pipeline statistics pool with the task and mesh shader invocation counts
on every GPU with mesh shaders. Those counts are a feature of their own, meshShaderQueries, and a pool made with them
where it is off is not valid: RADV turned it off on RDNA2 after its queries hung now and then, and on a device of that
family with no compute queue of its own the first capture (RX_VK_DRAW_STATS) hung the GPU for good. dev.mesh_queries
(gpu/vk/vk_main.cpp) is
that feature as the device reports it, and the pool asks under it.

python -I -B tests/test_mesh_query_feature.py
"""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DRAW = ROOT / "gpu" / "vk" / "vk_draw.cpp"
MAIN = ROOT / "gpu" / "vk" / "vk_main.cpp"


def main():
    if "dev.mesh_queries = dev.mesh_shader && mesh.meshShaderQueries;" not in MAIN.read_text(encoding="utf-8"):
        raise SystemExit("dev.mesh_queries is not the device's meshShaderQueries")
    body = block(DRAW.read_text(encoding="utf-8"), "bool stats_make_pool() {")
    asked = body.index("VK_QUERY_PIPELINE_STATISTIC_TASK_SHADER_INVOCATIONS_BIT_EXT")
    guard = body.rfind("if (", 0, asked)
    if guard < 0 or not body[guard:].startswith("if (dev.mesh_queries) {"):
        raise SystemExit("the statistics pool asks for task and mesh counts without the meshShaderQueries feature")
    print("task and mesh shader counts are asked for only with meshShaderQueries: ok")


if __name__ == "__main__":
    main()
