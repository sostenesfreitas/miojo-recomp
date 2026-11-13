"""Windows' commit limit (RAM plus the page file). Every VRAM block is charged to the process's commit, as the guest's memory is: a
run held private commit at the VRAM blocks plus ~21 GB the whole way, and a 16 GB PC with a fixed 32 GB page file reached its 47.9 GB
limit at 33.9 GB of its own. A guest commit then failed while an area loaded and the game's next write there crashed it.

Checked here:
- the textures' budget has a commit rule: below RX_VK_COMMIT_RESERVE_MB of headroom (GlobalMemoryStatusEx's ullAvailPageFile,
  Windows only) the shortfall comes off the budget and the rule's budget, as the driver's limit's excess does;
- hos_commit and the first-touch handler wait at the commit limit (1455) and commit again before they give up, and they log a
  failure the same way (commit_refused: the out-of-memory notice).
python -I -B tests/test_commit_limit.py
"""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, head):
    at = text.find(head)
    if at < 0:
        raise SystemExit("FAIL: not found: " + head)
    depth, i = 0, text.index("{", at)
    for j in range(i, len(text)):
        depth += {"{": 1, "}": -1}.get(text[j], 0)
        if depth == 0:
            return text[at:j + 1]
    raise SystemExit("FAIL: unbalanced: " + head)


def main():
    tex = (ROOT / "gpu" / "vk" / "vk_texture.cpp").read_text(encoding="utf-8")
    head = block(tex, "static bool commit_headroom(uint64_t* free_bytes)")
    if "ullAvailPageFile" not in head or "#if defined(_WIN32)" not in head:
        raise SystemExit("FAIL: commit_headroom does not read GlobalMemoryStatusEx's ullAvailPageFile on Windows only")
    reserve = block(tex, "static uint64_t commit_reserve()")
    if "RX_VK_COMMIT_RESERVE_MB" not in reserve or "2560ull << 20" not in reserve:
        raise SystemExit("FAIL: commit_reserve is not RX_VK_COMMIT_RESERVE_MB with a 2560 MB default")
    if "const bool headroom_known = reserve && commit_headroom(&commit_free);" not in tex:
        raise SystemExit("FAIL: the commit rule does not read the headroom only with a reserve")
    rule = re.search(r"if \(headroom_known && commit_free < reserve\) \{(.*?)\n        \}", tex, re.S)
    if not rule:
        raise SystemExit("FAIL: the textures' budget has no commit rule")
    body = rule.group(1)
    for need in ("const uint64_t over = reserve - commit_free;",
                 "budget = std::min(budget, total_bytes > over ? total_bytes - over : 0);",
                 "rule_budget = std::min(rule_budget, total_bytes > over ? total_bytes - over : 0);"):
        if need not in body:
            raise SystemExit("FAIL: the commit rule lacks: " + need)
    # (the rule sits before the excess's eviction target is worked out, which reads rule_budget)
    if tex.find("commit_headroom(&commit_free)") > tex.find("vram_compact::rule_target(total_bytes, size_budget, rule_budget)"):
        raise SystemExit("FAIL: the commit rule comes after rule_target reads rule_budget")

    glue = (ROOT / "host" / "mkw_glue.c").read_text(encoding="utf-8")
    wait = block(glue, "static int commit_wait(void *p, SIZE_T size)")
    if "Sleep(50)" not in wait or "ERROR_COMMITMENT_LIMIT" not in wait or "i < 100" not in wait:
        raise SystemExit("FAIL: commit_wait does not retry for up to 5 s while the limit refuses")
    commit = block(glue, "void hos_commit(uint64_t addr, uint64_t size)")
    if "err == ERROR_COMMITMENT_LIMIT && commit_wait(" not in commit or "commit_refused(" not in commit:
        raise SystemExit("FAIL: hos_commit does not wait at the commit limit before it reports the failure")
    touch = block(glue, "static LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS *info)")
    if "err == ERROR_COMMITMENT_LIMIT && commit_wait(" not in touch or "commit_refused(" not in touch:
        raise SystemExit("FAIL: the first-touch handler does not wait at the commit limit")
    if "hos_commit(" in touch:
        raise SystemExit("FAIL: the first-touch handler calls hos_commit, which would wait a second time")
    refused = block(glue, "static void commit_refused(uint64_t lo, uint64_t size, DWORD err)")
    if "hos_out_of_memory_box" not in refused:
        raise SystemExit("FAIL: commit_refused does not show the out-of-memory notice")
    # (#2941: std::bad_alloc on the presenting thread, in the page table's flush, closed the game: the flush writes the changes
    # at once when its list cannot be made, and the renderer keeps a reserve that operator new's handler gives back)
    mem = (ROOT / "gpu" / "vk" / "vk_memory.cpp").read_text(encoding="utf-8")
    flush = block(mem, "bool memory_table_flush()")
    if "catch (const std::bad_alloc&)" not in flush or "guest_pages[r.first + i] =" not in flush.split("catch (const std::bad_alloc&)")[1]:
        raise SystemExit("FAIL: memory_table_flush does not write the table at once when its list cannot be allocated")
    handler = block(tex, "void oom_handler()")
    if "oom_block.exchange(nullptr)" not in handler or "VirtualFree(p, 0, MEM_RELEASE)" not in handler or "throw std::bad_alloc();" not in handler:
        raise SystemExit("FAIL: oom_handler does not give the reserve back once, then throw as before")
    tend = block(tex, "static void oom_reserve_tend(bool known, uint64_t commit_free, uint64_t reserve)")
    if "std::set_new_handler(oom_handler)" not in tend or "commit_free > reserve + kOomReserve" not in tend:
        raise SystemExit("FAIL: oom_reserve_tend does not install the handler and hold the reserve again above the rule's line")
    if "oom_reserve_tend(headroom_known, commit_free, reserve);" not in tex:
        raise SystemExit("FAIL: the commit rule does not tend the renderer's reserve")
    print("PASS: the textures give commit back near the limit, and a commit at the limit waits for it before it fails")


if __name__ == "__main__":
    main()
