// (hookshot) The mesh fallback programs a start has in memory already when it reads the rest in for their compute pipelines
// (vk_progcache.cpp program_cache_load_some, for vk_draw.cpp precompile_run and precompile_at_create).
// tests/test_fallback_loaded.py
//
// On a GPU that draws mesh programs through the compute fallback, the start makes each such program's compute pipelines
// before the title: made at the first draw, the draw is left out until they are ready. The start found those programs by
// their records in the program cache's list, and a program read in earlier in the same start has none left there: the
// state lists' programs, which the shader preparation reads in to note their pipelines. So the start that prepared the
// shaders (a first start, an update, the Mesh shaders option changed) made the compute pipelines only of the fallback
// programs no state list names: 850 of them on one PC where a later start makes 2,096, and the session after it left 2,037
// draws out and compiled 42 pipelines in play. Those in memory are given to the same step now.
#pragma once
#include <cstddef>
#include <cstring>

namespace fallback_loaded {

/// RX_VK_FALLBACK_LOADED=0: as before, only the programs read in now
inline bool on(const char* env) { return !env || std::strcmp(env, "0") != 0; }

/// fn(program) for every translated mesh fallback program in `programs` (key -> its variants) whose key is not in `given`
/// (the keys read in now: all of their variants went to the caller already). Returns how many
template <class Programs, class Given, class Fn>
size_t each(const Programs& programs, const Given& given, Fn&& fn) {
    size_t n = 0;
    for (const auto& kv : programs) {
        if (given.count(kv.first)) continue;
        for (const auto& p : kv.second)
            if (p && p->fallback && !p->failed && p->ready.load()) n++, fn(*p);
    }
    return n;
}

}   // namespace fallback_loaded
