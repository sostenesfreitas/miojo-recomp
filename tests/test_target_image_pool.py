"""A render target's image whose texture is gone waits in a pool by its create info, and a new texture of that create info
takes it, instead of each of OoT's shifting transient targets being a new image made and destroyed.

OoT makes a frame's transient targets in a scratch heap whose offsets shift with what came before them in the frame, so each
shifted one is a texture of its own (vk_texture.cpp's dead targets). Every 60 presents the sweep let go of the dead ones,
300-700 at once, and the next present destroyed them all: 20-65 ms on the replaying thread, about once a second while the
camera moves. These checks keep the pool's rules: what is pooled, that a pooled image is still put into GENERAL from UNDEFINED
and cleared as a new one is, that the dead list pools before it destroys, and that the pool is bounded and can be turned off.

python -I -B tests/test_target_image_pool.py
"""
from pathlib import Path
import re
import unittest
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


class TargetImagePoolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tex = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
        cls.hdr = (ROOT / "gpu" / "vk" / "vk_texture.h").read_text(encoding="utf-8")

    def test_the_key_is_the_whole_create_info(self):
        key = block(self.tex, "uint64_t pool_key_of(const VkImageCreateInfo& ci)")
        for field in ("ci.flags", "ci.imageType", "ci.format", "ci.extent.width", "ci.extent.height", "ci.extent.depth",
                      "ci.mipLevels", "ci.arrayLayers", "ci.samples", "ci.usage", "ci.tiling"):
            self.assertIn(field, key, field)
        self.assertIn("| 1", key)   # (never 0: 0 is "not pooled")

    def test_only_uncompressed_render_targets_are_pooled_and_zero_turns_it_off(self):
        make = block(self.tex, "Texture* create(const Key& key")
        rule = make[make.index("const uint64_t pool_key ="):make.index("x.pool_key = pool_key;")]
        for part in ("as_target", "!x.sparse", "pool_cap()", "!is_bc(x.format)", "!is_astc(x.format)",
                     "VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT", "VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT"):
            self.assertIn(part, rule, part)
        cap = block(self.tex, "uint64_t pool_cap()")
        self.assertIn('getenv("RX_VK_TARGET_POOL_MB")', cap)
        self.assertIn(": 256", cap)
        self.assertIn("uint64_t pool_key = 0;", self.hdr)

    def test_a_texture_made_for_a_target_is_one_and_its_image_comes_from_the_targets_blocks(self):
        get = block(self.tex, "Texture* texture_get(const nvn_tex* t, bool for_sampling)")
        self.assertIn("create(k, t, fi.format, fi.astc, bw, bh, bpb, !for_sampling)", get)
        make = block(self.tex, "Texture* create(const Key& key")
        self.assertIn("create_image(ci, x.image, x.allocation, false, as_target && !is_bc(x.format) && !is_astc(x.format))", make)
        vma = (ROOT / "gpu" / "vk" / "vk_vma.cpp").read_text(encoding="utf-8")
        image = block(vma, "bool create_image(const VkImageCreateInfo& ci")
        self.assertIn("target && target_images_pool() ? target_images_pool() : images_pool()", image)
        pool = block(vma, "VmaPool target_images_pool()")
        self.assertIn('getenv("RX_VK_TARGET_BLOCKS")', pool)
        self.assertIn("pi.blockSize = kImageBlock;", pool)
        self.assertIn("void vma_log_image_blocks()", vma)

    def test_the_dead_targets_are_swept_every_ten_presents(self):
        frame = block(self.tex, "void texture_frame(uint64_t completed)")
        self.assertIn('getenv("RX_VK_DEAD_EVERY")', frame)
        self.assertIn(": 10;", frame[frame.index('getenv("RX_VK_DEAD_EVERY")'):])
        self.assertIn("frame_no % (dead_targets ? dead_every : 600)", frame)

    def test_a_dead_target_is_what_every_newer_target_over_it_has_drawn_together(self):
        # (several over parts of it add up; judged one by one they left the big ones, 3-4 GB, issue 2180)
        frame = block(self.tex, "void texture_frame(uint64_t completed)")
        rule = frame[frame.index("const auto drawn_over_shifted"):frame.index("static const uint64_t dead_min_unused")]
        self.assertIn("target_overlap::DrawnOver over;", rule)
        self.assertIn("over.add(lo, hi, tex->last_frame, (*it)->gpu", rule)
        self.assertIn("over.an_eighth(hi - lo)", rule)
        self.assertNotIn("target_overlap::shifted_over", rule)

    def test_a_view_inside_a_target_is_not_a_new_target(self):
        # (5bfa6aa, PR #899: a live scene target's sub-rects redrawn as views must not make it a dead one)
        hdr = (ROOT / "gpu" / "vk" / "target_overlap.h").read_text(encoding="utf-8")
        add = block(hdr, "void add(")
        self.assertIn("if (olo == lo) return;", add)   # (drawn_over's own case)
        self.assertIn("if (olo >= lo && ohi <= hi) return;", add)

    def test_a_finalized_map_keeps_its_keep_against_a_view_inside_it(self):
        # (keep_finalized: a 260 x 260 normal map the game finalized and samples again has a view drawn inside it; the
        # view is not a new target, so the map is not drawn over and keeps its keep)
        keep = block(self.tex, "const auto keep_finalized = [&](const Texture* tex)")
        self.assertIn("return !drawn_over(tex) && !drawn_over_shifted(tex);", keep)
        hdr = (ROOT / "gpu" / "vk" / "target_overlap.h").read_text(encoding="utf-8")
        self.assertIn("if (olo >= lo && ohi <= hi) return;", block(hdr, "void add("))

    def test_a_pooled_image_is_taken_before_one_is_made_and_still_cleared_from_undefined(self):
        make = block(self.tex, "Texture* create(const Key& key")
        take = make.index("pool_take(pool_key, x.image, x.allocation, x.views)")
        self.assertLess(take, make.index("!create_image(ci, x.image, x.allocation, false, as_target"))
        after = make[take:]
        self.assertIn("b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;", after)
        self.assertIn("vkCmdClearColorImage", after)
        self.assertIn("vkCmdClearDepthStencilImage", after)

    def test_a_gone_render_targets_views_go_to_the_pool_with_its_image(self):
        later = block(self.tex, "void destroy_later(Texture& tex)")
        self.assertRegex(later, r"if \(tex\.pool_key && tex\.allocation && tex\.sparse_chunks\.empty\(\) && pool_cap\(\)\)")
        self.assertIn("d.view_map = std::move(tex.views)", later)
        self.assertIn("d.views.push_back(kv.second)", later)   # (the others' views destroyed, as before)
        self.assertIn("pool_put(d);", later)   # (pooled at eviction for the next frame: test_target_image_reuse)
        self.assertIn("dead.push_back(std::move(d));", later)

    def test_the_dead_list_pools_before_it_destroys_and_the_pool_is_trimmed_each_present(self):
        frame = block(self.tex, "void texture_frame(uint64_t completed)")
        loop = frame[frame.index("for (auto it = dead.begin(); it != dead.end();) {"):]
        self.assertLess(loop.index("if (it->pool_key) {"), loop.index("destroy_image(it->image, it->allocation);"))
        self.assertIn("pool_put(*it);", loop[:loop.index("destroy_image(")])
        trim = frame.index("pool_trim();")
        self.assertLess(trim, frame.index("if (frame_no % 10) return;"))   # (every present, not every tenth)
        self.assertIn("pool_pressed = known && pressure > allowed / 10 * 9;", frame)

    def test_the_trim_is_bounded_a_present_and_gives_back_idle_and_excess_images(self):
        trim = block(self.tex, "size_t pool_trim()")
        self.assertIn("pool_pressed ? 0 : pool_cap()", trim)
        self.assertIn("pool_pressed ? 64 : 16", trim)
        self.assertIn("frame_no - v[i].since <= 120", trim)
        self.assertIn("pool_bytes <= cap", trim)
        self.assertIn("pool_destroy(v[i]);", trim)
        destroy = block(self.tex, "void pool_destroy(Pooled& p)")
        self.assertIn("vkDestroyImageView", destroy)
        self.assertIn("destroy_image(p.image, p.allocation);", destroy)

    def test_the_pools_numbers_are_logged(self):
        self.assertIn("render target images: %llu reused from the pool, %llu made new, %llu destroyed from it", self.tex)


if __name__ == "__main__":
    unittest.main()
