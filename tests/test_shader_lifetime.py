"""Execute borrowed shader cleanup and detached batch ownership without a GPU."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


class LifetimeTests(unittest.TestCase):
    def compile_run(self, code):
        cxx = shutil.which('clang++') or shutil.which('g++')
        if not cxx:
            self.skipTest('C++ compiler unavailable')
        with tempfile.TemporaryDirectory() as temp:
            source = pathlib.Path(temp) / 'test.cpp'
            exe = pathlib.Path(temp) / 'test.exe'
            source.write_text(code)
            subprocess.run([cxx, '-std=c++20', '-pthread', '-Wall', '-Wextra', '-Werror',
                            str(source), '-o', str(exe)], check=True, timeout=60)
            subprocess.run([str(exe)], check=True, timeout=10)

    def test_borrow_normal_callback_failure_and_partial_load(self):
        source = (ROOT / 'gpu/vk/vk_progcache.cpp').read_text()
        function = block(source, 'void program_cache_borrow(')
        self.compile_run(r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include <vector>
using Handle = unsigned;
[[maybe_unused]] constexpr Handle VK_NULL_HANDLE = 0;
struct Stage { Handle module; };
struct Program { std::vector<Stage> stages; Handle layout=0, set_layout=0; };
struct { int device=1; } dev;
std::unordered_set<Handle> live;
unsigned next_handle=0;
int load_failure=-1;
Handle create() { live.insert(++next_handle); return next_handle; }
void destroy(Handle h) { assert(h && live.erase(h)==1); }
void vkDestroyShaderModule(int,Handle h, const void*) { destroy(h); }
void vkDestroyPipelineLayout(int,Handle h, const void*) { destroy(h); }
void vkDestroyDescriptorSetLayout(int,Handle h, const void*) { destroy(h); }
void program_cache_load(uint64_t, std::vector<std::unique_ptr<Program>>& variants, bool keep) {
    assert(keep);
    for (int i=0; i<3; ++i) {
        auto p=std::make_unique<Program>();
        p->stages.push_back({create()});
        variants.push_back(std::move(p));
        if (i==load_failure) throw std::runtime_error("load");
    }
}
''' + function + r'''
int main() {
    for (int failure=-1; failure<3; ++failure) {
        int called=0;
        try {
            program_cache_borrow(1,[&](Program& p) {
                // Every earlier variant is already released before the next callback.
                assert(live.size()==static_cast<unsigned>(3-called));
                p.layout=create(); p.set_layout=create();
                if (called++==failure) throw std::runtime_error("callback");
            });
            assert(failure==-1);
        } catch (const std::runtime_error&) { assert(failure>=0); }
        assert(live.empty());
    }
    for (load_failure=0; load_failure<3; ++load_failure) {
        try { program_cache_borrow(1,[](Program&) { assert(false); }); assert(false); }
        catch (const std::runtime_error&) {}
        assert(live.empty());
    }
}
''')

    def test_batches_survive_handover_then_release(self):
        source = (ROOT / 'gpu/vk/vk_draw.cpp').read_text()
        for name in ('precompile_run', 'predict_run'):
            with self.subTest(batch=name):
                function = block(source, 'void ' + name + '(')
                state = block(function, 'struct Pre {') + ';'
                allocation = next(line.strip() for line in function.splitlines()
                                  if 'pre = ' in line and 'Pre' in line)
                # Production closures must retain their owning reference by value.
                self.assertIn('const auto worker = [pre,', function)
                self.assertNotIn('delete pre;', function)
                self.compile_run(r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
std::atomic<int> alive{0};
struct PipeNote { PipeNote(){++alive;} ~PipeNote(){--alive;} };
struct Program {};
''' + state + r'''
int main() {
    for (bool detached : {false,true}) {
        std::mutex mutex;
        std::condition_variable cv;
        bool release=false, finished=false;
        std::weak_ptr<Pre> weak;
        {
''' + allocation + r'''
            weak=pre;
            pre->notes.push_back(std::make_unique<PipeNote>());
            std::thread worker([pre,&mutex,&cv,&release,&finished]() mutable {
                std::unique_lock lock(mutex);
                cv.wait(lock,[&]{return release;});
                assert(alive==1);
                pre.reset();
                finished=true;
                cv.notify_all();
            });
            if (detached) worker.detach();
            else {
                { std::lock_guard lock(mutex); release=true; }
                cv.notify_all();
                worker.join();
                assert(alive==1); // caller still needs summary data
            }
        }
        if (detached) {
            assert(!weak.expired() && alive==1); // worker owns data after caller returns
            { std::lock_guard lock(mutex); release=true; }
            cv.notify_all();
            std::unique_lock lock(mutex);
            cv.wait(lock,[&]{return finished;});
        }
        assert(weak.expired() && alive==0);
    }
}
''')


if __name__ == '__main__':
    unittest.main()
