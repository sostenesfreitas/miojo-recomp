"""The actual TAAU replacement has matching packed/RGBA storage types and format-keyed history."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--write', action='store_true')
    parser.add_argument('--glslang', default='glslang')
    args = parser.parse_args()
    shader = ROOT/'gpu/vk/shaders/taau_upscale.comp'
    digest = hashlib.sha256(shader.read_bytes()).hexdigest()
    for rgba in (False, True):
        name = 'taau_upscale_rgba_comp_spv.h' if rgba else 'taau_upscale_comp_spv.h'
        symbol = 'TAAU_UPSCALE_RGBA_COMP_SPV' if rgba else 'TAAU_UPSCALE_COMP_SPV'
        header = shader.with_name(name)
        if args.write:
            subprocess.run([args.glslang, '-V', '--target-env', 'vulkan1.3',
                            *(['-DTAAU_RGBA16F=1'] if rgba else []), '--vn', symbol,
                            '-o', str(header), str(shader)], check=True)
            header.write_bytes(('// Generated from taau_upscale.comp; SHA-256: '+digest+'\n'+
                                header.read_text(encoding='utf-8')).encode())
        text = header.read_text(encoding='utf-8')
        assert digest in text.splitlines()[0]
        words = [int(w, 16) for w in re.findall(r'0x[0-9a-fA-F]{8}', text)]
        assert words[0] == 0x07230203
        formats, images, pointers, variables, bindings, i = [], {}, {}, {}, {}, 5
        while i < len(words):
            count, op = words[i] >> 16, words[i] & 65535
            assert count and i+count <= len(words)
            if op == 25:
                formats.append(words[i+8])
                images[words[i+1]] = words[i+8]
            elif op == 32:
                pointers[words[i+1]] = words[i+3]
            elif op == 59:
                variables[words[i+2]] = words[i+1]
            elif op == 71 and words[i+2] == 33:
                bindings[words[i+3]] = words[i+1]
            i += count
        # SPIR-V interns one image type shared by output and history; follow both binding variables.
        for binding in (1, 2):
            assert images[pointers[variables[bindings[binding]]]] == (2 if rgba else 8)
        assert (8 if rgba else 2) not in formats
    draw = (ROOT/'gpu/vk/vk_draw.cpp').read_text(encoding='utf-8')
    assert 'history.format != out->format' in draw
    assert 'history.format = out->format, hist_frame = 0' in draw
    assert 'ic.imageType = VK_IMAGE_TYPE_2D, ic.format = out->format;' in draw
    assert 'Pipeline& pipeline = pipelines[rgba ? 1 : 0];' in draw
    assert 'mi.pCode = rgba ? TAAU_UPSCALE_RGBA_COMP_SPV : TAAU_UPSCALE_COMP_SPV;' in draw
    assert 'vec3 c = s * s;' in shader.read_text(encoding='utf-8')
    print('PsychoV35 TAAU: both compiled storage types, generated hashes and format/history selection pass')


if __name__ == '__main__':
    main()
