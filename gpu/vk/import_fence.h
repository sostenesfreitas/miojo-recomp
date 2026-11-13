#pragma once
// (hookshot) A host-memory import does not cost a walk over the game's heap. The game's heap is one run of committed
// read-write pages, 6.9 GB in OoT, and Windows answers a question about an address's region (VirtualQuery) by walking
// page by page from that address to the end of the run of pages with the same state and protection. NVIDIA's
// vkAllocateMemory for a host pointer pays that walk: an import in the heap's low 2 GB took a median 13-15 ms whatever
// its size, 4 MB or 88 MB, 23-30 ms late in a session and 21-33 ms with the whole heap touched, against 2-6 ms for
// imports higher up, where the run ends sooner (RTX 4070 Ti SUPER, driver 621.24; VirtualQuery at such an import's
// start took 13-17 ms itself and gave a region of 5,838 MB). The imports in play are for the game's small pools, 64
// bytes in a page of their own somewhere in the low heap, first bound when what they belong to comes into view: one
// stalled frame each on the thread that replays the frame.
//
// While the driver is asked, the page after the range has another protection that reads and writes the same,
// PAGE_EXECUTE_READWRITE, so the run from the range's start ends with the range; then it has its own back. Only a
// committed PAGE_READWRITE page is changed: anything else (reserved, a write watch's read-only page) already ends the run.
// Nothing runs from guest memory, with the page's protection or without: the game's code is the lifted host code.
// Measured on one six-minute route, two runs each: 12-13 imports of 2 ms or more, 41-55 ms in all, none of them in play,
// against 48-56, 525-788 ms, 6-10 of them in play at 15-29 ms.
#include <cstdint>
#ifndef __linux__
#include <windows.h>
#endif

namespace mkwvk {

class ImportFence {
#ifndef __linux__
    void* page_ = nullptr;

public:
    /// The page after [ptr, ptr + bytes) ends the region that starts at ptr, until this goes out of scope
    ImportFence(uint8_t* ptr, uint64_t bytes, bool on) {
        if (!on || !ptr || !bytes) return;
        void* const page = ptr + bytes;
        DWORD old = 0;
        if (!VirtualProtect(page, 1, PAGE_EXECUTE_READWRITE, &old)) return;   // (not committed, or refused: the import as before)
        if (old != PAGE_READWRITE) {   // (not ours to change)
            VirtualProtect(page, 1, old, &old);
            return;
        }
        page_ = page;
    }
    ~ImportFence() {
        if (!page_) return;
        DWORD old = 0;
        // (changed by someone else meanwhile: theirs stands)
        if (VirtualProtect(page_, 1, PAGE_READWRITE, &old) && old != PAGE_EXECUTE_READWRITE) VirtualProtect(page_, 1, old, &old);
    }
    bool set() const { return page_ != nullptr; }
#else
public:
    // (Linux: no region walk; a pool is a dma-buf or a host pointer the kernel pins)
    ImportFence(uint8_t*, uint64_t, bool) {}
    bool set() const { return false; }
#endif
    ImportFence(const ImportFence&) = delete;
    ImportFence& operator=(const ImportFence&) = delete;
};

} // namespace mkwvk
