# SPDX-License-Identifier: GPL-3.0-or-later
"""Handle tables as descriptor arrays (Lichii bd0171f7): a texture handle the shader reads from a table at an index of its own.

OoT's clustered lights sample each light's shadow map with a handle read per pixel from the light table (a light index from
the pixel's list, then the light's 96-byte entry). The translation now makes that a descriptor array of the table's first
entries, whose handle program computes entry k's handle (HandleOp::Index), and the shader indexes the array per pixel.
Before, the samples took the light table's address word for the handle and bound no texture: the lights' shadow tests read
nothing (dark and green blocks on Zora's Domain's ice and snow, https://git.notrexed.dev/Hookshot/Hookshot/issues/52,
https://github.com/NotRexed/Hookshot/issues/2509).

Built from the sources' own text: shader_info.h's HandleOp and EvaluateHandleProgram, texture_pass.cpp's TrimTrailingTees,
vk_shader.cpp's stage_handle / stage_handles, with stubs for the banks and memory; then checks of the code that has no
fixture here (the translation's paths, the draw's table binding).
    python -I -B tests/test_handle_table_arrays.py [--cxx clang++]
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

RUN_ALL_ARGS = ["--cxx", "{cxx}"]


# These production bodies have balanced braces; literals in them contain no braces.
def block(text, signature):
    start = text.find(signature)
    assert start >= 0, f'Missing anchor: {signature}'
    brace = text.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    if text.startswith(';', end):   # (an enum's)
        end += 1
    return text[start:end]


def require(text, needle, why):
    if needle not in text:
        raise SystemExit(f'FAIL: {why}: missing {needle!r}')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--cxx', default='c++')
    a = p.parse_args()
    root = a.source_root
    info_h = (root / 'gpu/vk/eden/shader_recompiler/shader_info.h').read_text(encoding='utf-8')
    texture_pass = (root / 'gpu/vk/eden/shader_recompiler/ir_opt/texture_pass.cpp').read_text(encoding='utf-8')
    shader = (root / 'gpu/vk/vk_shader.cpp').read_text(encoding='utf-8')
    draw = (root / 'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')

    extracted = '\n'.join([
        'namespace Shader {',
        block(info_h, 'enum class HandleOp : u32 {'),
        block(info_h, 'template <typename ReadCbuf, typename ReadStorage>\nstd::optional<u32> EvaluateHandleProgram('),
        '}',
        'namespace Shader::Optimization {',
        block(texture_pass, 'size_t HandleOperands(HandleOp op)'),
        block(texture_pass, 'void TrimTrailingTees(std::vector<u32>& code)'),
        '}',
        block(shader, 'uint64_t stage_storage_base('),
        block(shader, 'uint32_t stage_storage_word('),
        block(shader, 'uint32_t stage_handle(const NvnState& s, const Stage& st, uint32_t program, uint32_t element)'),
        block(shader, 'void stage_handles('),
    ])
    prelude = r'''
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <vector>
using u32 = uint32_t; using s32 = int32_t; using u64 = uint64_t;
namespace Shader {
struct StorageBufferDescriptor { u32 cbuf_index, cbuf_offset; };
struct Info {
 static constexpr size_t MAX_SSBOS{64};
 std::vector<StorageBufferDescriptor> storage_buffers_descriptors;
 std::vector<std::vector<u32>> handle_programs;
};
}

struct NvnState {};
struct Stage { Shader::Info info; };
// a constant bank of 64 words; memory of 1024 words at 0x10000
u32 bank[64]{}; u32 memory[1024]{};
int cbuf_reads = 0, memory_reads = 0;
uint32_t stage_cbuf_word(const NvnState&, const Stage&, uint32_t index, uint32_t offset) {
 ++cbuf_reads; return index == 3 && offset / 4 < 64 ? bank[offset / 4] : 0;
}
bool env_on(const char*) { return false; }
void ub_read(uint64_t addr, void* out, size_t n) {
 ++memory_reads; uint32_t v = addr >= 0x10000 && addr < 0x10000 + 4096 ? memory[(addr - 0x10000) / 4] : 0; std::memcpy(out, &v, n);
}
struct Host { void gpu_read(uint64_t addr, void* out, size_t n) { ub_read(addr, out, n); } } host_object;
Host* host = &host_object;
uint32_t stage_handle(const NvnState& s, const Stage& st, uint32_t program, uint32_t element = 0);   // (vk_shader.h's)
'''
    checks = r'''
int main() {
 int failures = 0;
 auto eq = [&](u64 actual, u64 expected, const char* label) {
  if (actual != expected) { std::cerr << label << ": " << actual << " != " << expected << '\n'; ++failures; }
 };
 using Op = Shader::HandleOp;
 auto run = [](std::vector<u32> code, u32 element) {
  return Shader::EvaluateHandleProgram(code, [](u32, u32 o) { return o; }, [](u32 b, u32 o) { return b << 24 | o; }, element);
 };
 // Index pushes the element; programs without it are as before
 eq(run({u32(Op::Index)}, 7).value_or(~0u), 7, "Index is the element");
 eq(run({u32(Op::Immediate), 5}, 9).value_or(~0u), 5, "a program without Index ignores the element");
 eq(Shader::EvaluateHandleProgram(std::vector<u32>{u32(Op::Index)}, [](u32, u32) { return 0u; }, [](u32, u32) { return 0u; }).value_or(~0u),
    0, "the element defaults to 0");
 // entry k of a 96-byte table in storage buffer 2, its handle the first word: offset 96k
 const std::vector<u32> entry{u32(Op::Index), u32(Op::Immediate), 96, u32(Op::Mul), u32(Op::Storage), 2};
 eq(run(entry, 0).value_or(~0u), 2u << 24 | 0, "table entry 0");
 eq(run(entry, 5).value_or(~0u), 2u << 24 | 480, "table entry 5");
 // trailing Tees go (they keep the result for nothing); inner ones and the operands stay
 std::vector<u32> tees{u32(Op::Index), u32(Op::Tee), 0, u32(Op::Load), 0, u32(Op::Add), u32(Op::Tee), 1, u32(Op::Tee), 2};
 Shader::Optimization::TrimTrailingTees(tees);
 eq(tees.size(), 6, "trailing Tees trimmed, inner Tee kept");
 eq(tees.back(), u32(Op::Add), "the program ends at its last value");
 std::vector<u32> imm_tee{u32(Op::Immediate), u32(Op::Tee)};   // (an immediate whose value is the Tee opcode: an operand, kept)
 Shader::Optimization::TrimTrailingTees(imm_tee);
 eq(imm_tee.size(), 2, "an operand equal to Tee's code is not a Tee");
 // stage_handles: storage buffer 0's address in c3[0x10..0x18], the table at 0x10000, entry k's handle at 96k + 4
 NvnState s; Stage st;
 st.info.storage_buffers_descriptors = {{3, 0x10}};
 bank[4] = 0x10000; bank[5] = 0;
 for (u32 k = 0; k < 8; k++) memory[(96 * k + 4) / 4] = 0x100 + k;
 st.info.handle_programs = {{u32(Op::Index), u32(Op::Immediate), 96, u32(Op::Mul), u32(Op::Immediate), 4, u32(Op::Add),
                             u32(Op::Storage), 0}};
 u32 out[8]{};
 cbuf_reads = memory_reads = 0;
 stage_handles(s, st, 0, 8, out);
 for (u32 k = 0; k < 8; k++) eq(out[k], 0x100 + k, "stage_handles entry k");
 eq(cbuf_reads, 2, "the storage buffer's address read once for all elements");
 eq(memory_reads, 8, "one memory read an element");
 eq(stage_handle(s, st, 0, 3), 0x103, "stage_handle of element 3");
 eq(stage_handle(s, st, 0), 0x100, "stage_handle defaults to element 0");
 u32 none[4]{1, 1, 1, 1};
 stage_handles(s, st, 9, 4, none);
 eq(none[0] | none[3], 0, "an unknown program gives no handles");
 if (!failures) std::cout << "handle table checks passed\n";
 return failures ? 1 : 0;
}
'''
    with tempfile.TemporaryDirectory(prefix='hookshot-handle-tables-') as tmp:
        src = Path(tmp) / 'check.cpp'
        exe = Path(tmp) / ('check.exe' if os.name == 'nt' else 'check')
        src.write_text(prelude + extracted + checks, encoding='utf-8')
        subprocess.run([a.cxx, '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', str(src), '-o', str(exe)],
                       check=True, timeout=120)
        result = subprocess.run([str(exe)], check=True, capture_output=True, text=True, timeout=10)
        assert result.stdout.strip() == 'handle table checks passed', 'Fixture did not report completed checks'
        print(result.stdout, end='')

    # The translation's paths (no fixture: they need the IR)
    track = block(texture_pass, 'std::optional<ConstBufferAddr> Track(const IR::Value& value, Environment& env, '
                                'const HostTranslateInfo& host_info, const IR::Block* block) {')
    require(track, 'IsTextureInstruction(*inst) || IsFloatValue(*inst)',
            'Track must not search through texture results or float math (c6[0x430], a float, became the handle)')
    require(track, 'if (host_info.support_handle_table_arrays &&', 'RX_VK_HANDLE_TABLES=0 translates as before handle tables')
    node = block(texture_pass, 'bool BuildHandleNode(const IR::Value& value, std::vector<u32>& code, int& budget,\n                     const IR::Block* block) {')
    for op in ('CompositeExtractU32x3', 'CompositeExtractU32x4'):
        require(node, f'case IR::Opcode::{op}:', 'the handle builder must read a word of a 12- or 16-byte load (the light table)')
    require(node, 'if (!address(source->Arg(1))) return index_word();', 'a storage read at a per-invocation address is the index word')
    require(node, 'emit(Op::Index);', 'the index word becomes HandleOp::Index')
    require(node, '(!g_handle_tables && inst->GetOpcode() != IR::Opcode::CompositeExtractU32x2)',
            'RX_VK_HANDLE_TABLES=0 reads no word of a 12- or 16-byte load, as before handle tables')
    make = block(texture_pass, 'TextureInst MakeInst(')
    require(make, 'host_info.support_handle_table_arrays', 'handle tables only where the host has them')
    require(make, 'g_dominance->Dominates(index.InstRecursive(), &inst)', 'the index word must dominate the sample to index the array')
    require(make, 'const u32 count{index.IsEmpty() ? 1U : HANDLE_TABLE_SIZE};',
            'a handle table is an array of HANDLE_TABLE_SIZE elements')
    dom = make.index('g_dominance->Dominates(index.InstRecursive(), &inst)')
    require(make[dom:make.index('if (!built) {', dom)], 'built = false;',
            'an index word that does not dominate the sample takes the earlier search, not table entry 0 for every pixel')
    for gated in ('if (host_info.support_handle_table_arrays) {   // (as before handle tables without them)\n                TrimTrailingTees(code);',
                  'const bool pointer_word{host_info.support_handle_table_arrays &&'):
        require(make, gated, 'RX_VK_HANDLE_TABLES=0 translates as before handle tables')
    require(make, 'TrimTrailingTees(code);', 'the samples of one table share one handle program and one array')
    require(make, '.pointer_word = pointer_word', 'a legacy word reached only through a load\'s address is a pointer (Lichii 282a585a)')
    m = re.search(r'constexpr u32 HANDLE_TABLE_SIZE = (\d+);', texture_pass)
    assert m and 1 < int(m.group(1)) <= 1024, 'HANDLE_TABLE_SIZE must be an array of 2..1024'
    tp = block(texture_pass, 'void TexturePass(Environment& env, IR::Program& program, const HostTranslateInfo& host_info) {')
    require(tp, 'if (cbuf.handle_program == ~0U) {', 'the stage cap (1 with Hookshot\'s host_info) must not cut a handle table')
    require(tp, '.handle_is_pointer_word = texture_inst.pointer_word,', 'the descriptor carries the pointer word')
    require(tp, 'g_handle_tables = host_info.support_handle_table_arrays;', 'the handle builder sees the host\'s switch')

    # The host's switch and the draw's binding
    host = block(shader, 'Shader::HostTranslateInfo host_info() {')
    require(host, 'h.support_handle_table_arrays = handle_tables_on();', 'host_info must give the translation the switch')
    tables = block(shader, 'bool handle_tables_on() {')
    require(tables, 'shaderSampledImageArrayNonUniformIndexing', 'handle tables need nonuniform sampled image array indexing')
    require(tables, 'RX_VK_HANDLE_TABLES', 'RX_VK_HANDLE_TABLES=0 turns them off')
    for limit in ('maxPerStageDescriptorSampledImages', 'maxPerStageDescriptorSamplers', 'maxPerStageResources',
                  'maxDescriptorSetSampledImages', 'maxDescriptorSetSamplers'):
        require(tables, f'need("{limit}", l.{limit}, ', 'the log line names the limit that turned handle tables off')
    stamp = block(shader, 'uint64_t translator_stamp() {')
    require(stamp, 'if (!handle_tables_on()) {', 'programs translated without handle tables must not share the cache')
    # The offline copies of host_info: a bank baked offline must be translated as a live run with the same stamp is
    bake = (root / 'nx2/bake_ds/bake_ds.cpp').read_text(encoding='utf-8')
    require(block(bake, 'Shader::HostTranslateInfo host_info() {'), 'h.support_handle_table_arrays = g_handle_tables;',
            'bake_ds translates with handle tables as live does')
    require(block(bake, 'uint64_t disk_stamp(uint64_t translator_stamp, uint32_t depth) {'), 'if (!g_handle_tables) {',
            'a bank baked without handle tables carries live\'s "nhtb" word')
    batch = (root / 'gpu/shaderbatch.cpp').read_text(encoding='utf-8')
    require(block(batch, 'Shader::HostTranslateInfo make_host() {'), 'h.support_handle_table_arrays = true;',
            'shaderbatch translates with handle tables as live does')
    pointer = block(draw, 'bool handle_is_storage_pointer(')
    require(pointer, 'if (d.handle_is_pointer_word) return true;', 'a pointer word binds no texture')
    require(draw, 'stage_handles(s, st, d.handle_program, d.count, table.data());', 'the draw binds every entry of a handle table')
    require(draw, 'is_table && view && tex && d.is_depth && !is_depth_format(tex->format)',
            'a table entry naming a colour texture for a depth comparison binds nothing')
    require(draw, 'if (is_table && k + 1 == d.count && view) {',
            'a table whose last element names a texture (indices past it are clamped to it) is logged')
    print('handle table source checks passed')


if __name__ == '__main__':
    main()
