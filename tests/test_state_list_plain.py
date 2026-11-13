"""A pipeline state list record with variant 0 and no answers or questions stands for the variant nothing bound makes.

Lists converted from records that hold no texture answers (assets/pipeline_states/contrib/*-fritchii-recipe*) name a program
variant by nothing: variant 0, no answers, no questions. state_list_prepare (gpu/vk/vk_draw.cpp) prepares those with the
variant the program's translation makes with nothing bound (seed_plain_of, gpu/vk/vk_shader.cpp), the one draws whose textures
are what the instructions declare ask for. Not with seed_variant_of and empty answers: that variant would have no answers, and
a variant without answers stands for any textures (still_valid). And only when the record's own variant is not found: a program
without textures has variant 0 already. The plain variant's own variant is the hash of its answers, not 0, so it is looked up by
them (program_find_plain: every slot answering what its instruction declares, as record_answers makes it with nothing bound):
loaded from the program cache, it is neither translated nor saved again at each prepare. This checks the source.
python -I -B tests/test_state_list_plain.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(text, marker):
    start = text.index(marker)
    at = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    return text[start:at]


def main():
    shader = (ROOT / "gpu" / "vk" / "vk_shader.cpp").read_text(encoding="utf-8")
    made = body(shader, "static const Program* seed_variant_made(")
    plain = body(shader, "const Program* seed_plain_of(")
    of = body(shader, "const Program* seed_variant_of(")
    if "if (questions) questions_in = questions" not in made:
        raise SystemExit("seed_variant_made gives the translation questions even when none are given: the plain variant would "
                         "not answer them as nothing bound does")
    if "nullptr, nullptr" not in plain:
        raise SystemExit("seed_plain_of does not translate with nothing given")
    if "&questions, &answers" not in of:
        raise SystemExit("seed_variant_of does not hand its questions and answers to seed_variant_made")
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    prep = body(draw, "void state_list_prepare(")
    if "!r.variant && !r.nanswers && !r.nquestions" not in prep:
        raise SystemExit("state_list_prepare does not tell records with variant 0 and no answers or questions apart")
    if "seed_plain_of(" not in prep:
        raise SystemExit("state_list_prepare does not make the variant nothing bound makes for those records")
    found = prep.index("const Program* p = program_find_portable(key, r.variant);", prep.index("plain_of.emplace"))
    if prep.index("if (!p && plain(r))", found) - found > 120:
        raise SystemExit("state_list_prepare takes the variant nothing bound makes before the record's own variant")
    # (the plain variant loaded already is found by its answers, not queued and translated again: hckrman101 on #2134)
    find = body(shader, "const Program* program_find_plain(")
    if "p->answers.size() == slots" not in find or "canonical_answer(*p, a, declared_answer(*p, a.stage, a.desc, a.image))" not in find:
        raise SystemExit("program_find_plain does not ask for an answer per slot, each its declared type as record_answers makes it")
    answers = body(shader, "void record_answers(Program& p, const NvnState& s) {")
    if answers.count("declared_answer(p, k, i,") != 2:
        raise SystemExit("record_answers does not answer an unbound slot with declared_answer: program_find_plain could miss its variant")
    queue = prep[prep.index("std::vector<size_t> variant_of;"):prep.index("total = want.size() + variant_of.size();")]
    if "!(plain(r) && program_find_plain(key))" not in queue:
        raise SystemExit("state_list_prepare queues a plain record although its variant is loaded: translated and saved again "
                         "at each prepare")
    if "program_find_plain(key)" not in prep[found:]:
        raise SystemExit("state_list_prepare does not note the loaded plain variant's pipeline")
    print("PASS: records without a variant are prepared with the variant nothing bound makes, after their own, and once")


if __name__ == "__main__":
    main()
