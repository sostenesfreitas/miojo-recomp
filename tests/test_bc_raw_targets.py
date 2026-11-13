"""Render and clear BC textures in blocks. A 3D BC slice uses an uncompressed attachment and copies its blocks back.

OoT renders the slices of two 64^3 BC4 noise volumes as RGBA16UI targets while an area loads (program 53509044401d78d2).
BC images had no colour attachment usage, so those views were not made; the pass ran over no target at the texels' 64 x 64,
and it was left out by default after it lost the device on first starts. The volumes stayed zero and the foliage's dithered
LOD crossfade, which reads them, left trees bare at mid distance (issue 1151).
Attaching a raw view of a 3D BC image still loses the device on Pascal even with the right extent. Drawing to a
separate attachment preserves the pass and the compressed sampling format.

python -I -B tests/test_bc_raw_targets.py
"""
from pathlib import Path
import re
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


class BcRawTargetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tex = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
        cls.draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
        cls.xfer = (ROOT / "gpu" / "vk" / "vk_xfer.cpp").read_text(encoding="utf-8")

    def test_bc_volumes_are_not_used_as_direct_colour_attachments(self):
        raw = self.tex.index("Raw storage views address one compressed block per texel")
        branch = self.tex[raw:self.tex.index("CUBE_COMPATIBLE_BIT", raw)]
        self.assertRegex(branch, r"if \(x\.type != VK_IMAGE_TYPE_3D && .*VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT")
        self.assertRegex(branch, r"x\.usage \|= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;")
        attachment = block(self.tex, "TextureAttachment texture_attachment(")
        self.assertIn("ci.format = format;", attachment)
        self.assertIn("ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;", attachment)
        self.assertIn("vi.image = attachment->image;", attachment)

    def test_a_refused_bc_image_is_made_again_without_the_attachment_usage_first(self):
        fallback = self.tex.index("// (a compressed image refused:")
        begin = self.tex.rfind("} else if (", 0, fallback)
        self.assertGreaterEqual(begin, 0)
        chain = self.tex[begin:]
        chain = chain[:chain.index("logf(\"vk: image %ux%ux%u")]
        self.assertIn("!create_image(ci, x.image, x.allocation", chain)
        attach = chain.index("ci.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT")
        storage = chain.index("ci.usage &= ~VK_IMAGE_USAGE_STORAGE_BIT")
        self.assertLess(attach, storage)

    def test_raw_views_of_bc_targets_are_sized_in_blocks(self):
        targets = block(self.draw, "Targets targets(NvnState& s)")
        self.assertRegex(targets, r"if \(bc\(tex->format\) && !bc\(f\)\) w = \(w \+ 3\) / 4, h = \(h \+ 3\) / 4;")
        self.assertLess(targets.index("w = (w + 3) / 4"), targets.index("at.width = at.width ?"))

    def test_clears_through_raw_views_are_sized_in_blocks(self):
        clear = block(self.xfer, "void clear_tex(")
        self.assertIn("raw ? (lw + 3) / 4 : lw", clear)
        self.assertIn("ri.renderArea = {{0, 0}, {aw, ah}};", clear)
        self.assertIn("const VkClearRect rect{{{rx, ry}, {rw, rh}}, 0, 1};", clear)

    def test_volume_attachments_are_loaded_before_rendering_and_stored_after(self):
        begin = block(self.draw, "void begin_pass(")
        self.assertLess(begin.index("copy_block_attachment("), begin.index("vkCmdBeginRendering("))
        end = block(self.draw, "void end_pass(")
        self.assertLess(end.index("vkCmdEndRendering("), end.index("copy_block_attachment("))
        self.assertLess(end.index("copy_block_attachment("), end.index("probe_image("))

    def test_the_noise_pass_is_left_out_only_while_its_targets_have_no_view(self):
        line = re.search(r'const char\* list = .*RX_VK_SKIP_PROG.*;', self.draw).group(0)
        self.assertNotIn("53509044401d78d2", line)
        self.assertRegex(self.draw, r"default_list && cp->key == 0x53509044401d78d2ull && !at\.n")


if __name__ == "__main__":
    unittest.main()
