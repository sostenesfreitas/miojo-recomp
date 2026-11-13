#include "../gpu/vk/reactive_policy.h"

#include <cstdio>

#define CHECK(name, condition) do { if (!(condition)) { std::fprintf(stderr, "FAIL %s at line %d\n", name, __LINE__); return 1; } } while (0)

int main() {
    namespace rp = mkwvk::reactive_policy;
    // which draws: (colour written, blend, depth test, depth write)
    CHECK("a depth pass's draw is neither", rp::classify(false, false, true, true) == rp::kNothing &&
                                                rp::classify(false, true, true, false) == rp::kNothing);
    CHECK("colour without blending is opaque", rp::classify(true, false, true, true) == rp::kOpaque &&
                                                   rp::classify(true, false, true, false) == rp::kOpaque &&
                                                   rp::classify(true, false, false, false) == rp::kOpaque);
    CHECK("blended, depth tested and not written: transparent", rp::classify(true, true, true, false) == rp::kTransparent);
    CHECK("blended with depth written (it has a depth to follow): neither", rp::classify(true, true, true, true) == rp::kNothing);
    CHECK("blended without a depth test (over the whole scene): neither", rp::classify(true, true, false, false) == rp::kNothing);

    const uint64_t scene = 0x1000, other = 0x2000;
    {   // the usual frame: depth, opaque, then one run of transparent draws
        rp::Run r;
        CHECK("no copy for the depth pass", !rp::copy_before(r, 7, scene, rp::kNothing));
        CHECK("nor for opaque draws", !rp::copy_before(r, 7, scene, rp::kOpaque) && !rp::copy_before(r, 7, scene, rp::kOpaque));
        CHECK("one before the first transparent draw", rp::copy_before(r, 7, scene, rp::kTransparent));
        for (int i = 0; i < 100; i++) CHECK("none within the run", !rp::copy_before(r, 7, scene, rp::kTransparent));
        CHECK("nor for what is neither", !rp::copy_before(r, 7, scene, rp::kNothing) && !rp::copy_before(r, 7, scene, rp::kTransparent));
        CHECK("one copy", r.copies == 1 && !r.capped);
        // the next frame starts over
        CHECK("a new frame's first transparent draw", rp::copy_before(r, 8, scene, rp::kTransparent) && r.copies == 1);
    }
    {   // a frame of only opaque draws: never a copy (and so no mask)
        rp::Run r;
        for (int i = 0; i < 50; i++) CHECK("opaque alone", !rp::copy_before(r, 1, scene, rp::kOpaque));
        CHECK("no copies", r.copies == 0);
    }
    {   // opaque draws after a copy would read as blended over it: the next run copies again
        rp::Run r;
        CHECK("first run", rp::copy_before(r, 3, scene, rp::kTransparent));
        CHECK("opaque after it", !rp::copy_before(r, 3, scene, rp::kOpaque));
        CHECK("second run copies again", rp::copy_before(r, 3, scene, rp::kTransparent));
        CHECK("and only once", !rp::copy_before(r, 3, scene, rp::kTransparent) && r.copies == 2);
    }
    {   // another scene-sized target: the copy is of the wrong one
        rp::Run r;
        CHECK("first target", rp::copy_before(r, 3, scene, rp::kTransparent));
        CHECK("another target's transparent draw copies it", rp::copy_before(r, 3, other, rp::kTransparent));
        CHECK("and its run goes on", !rp::copy_before(r, 3, other, rp::kTransparent));
        CHECK("back on the first: again", rp::copy_before(r, 3, scene, rp::kTransparent));
    }
    {   // draws that alternate: held to kCopies a frame, and said
        rp::Run r;
        int copies = 0;
        for (int i = 0; i < 40; i++) {
            copies += rp::copy_before(r, 5, scene, rp::kTransparent);
            rp::copy_before(r, 5, scene, rp::kOpaque);
        }
        CHECK("capped", copies == rp::kCopies && r.copies == rp::kCopies && r.capped);
        CHECK("until the next frame", rp::copy_before(r, 6, scene, rp::kTransparent) && !r.capped && r.copies == 1);
    }
    {   // a run that ends the frame without an opaque draw after it is never capped
        rp::Run r;
        for (int i = 0; i < rp::kCopies - 1; i++) {
            rp::copy_before(r, 2, scene, rp::kTransparent);
            rp::copy_before(r, 2, scene, rp::kOpaque);
        }
        CHECK("the last allowed copy", rp::copy_before(r, 2, scene, rp::kTransparent));
        CHECK("its run is not a cap", !rp::copy_before(r, 2, scene, rp::kTransparent) && !r.capped);
    }
    std::printf("reactive mask policy: ok\n");
    return 0;
}
