"""The pipeline notes file (hookshot_pipelines.bin) and the pipeline state list are opened by the precompile, before play, with
the notes it has just read, not at the first new pipeline in play.

pipeline_note opened the notes file at the first new pipeline of a run and read its keys back with a seek and an 8-byte read
each: 142 ms for 1150 notes on the replaying thread (RTX 5070, Windows); state_record read and hashed the whole state list
there too, 8-18 ms. These checks keep both opened in precompile_run, the notes' keys taken from what pipe_notes_read returned,
and, where the precompile did not open them, the records read in order rather than a seek a record.

python -I -B tests/test_pipeline_notes_open.py
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


class PipelineNotesOpenTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")

    def test_pipeline_note_opens_through_the_shared_function(self):
        note = block(self.draw, "void pipeline_note(uint64_t key")
        self.assertIn("if (!note_file) notes_open_locked(nullptr);", note)
        self.assertNotIn("_fseeki64", note)

    def test_the_keys_come_from_the_notes_read_or_the_records_in_order(self):
        open_ = block(self.draw, "void notes_open_locked(const std::vector<std::unique_ptr<PipeNote>>* known)")
        self.assertIn("for (const auto& n : *known) noted.insert(n->key);", open_)
        self.assertIn("_fseeki64(f, 16, SEEK_SET)", open_)   # (one seek, past the head)
        self.assertIn("while (fread(rec.get(), sizeof(PipeNote), 1, f) == 1) noted.insert(rec->key);", open_)
        self.assertNotIn("at += sizeof(PipeNote)", open_)   # (no seek a record)
        self.assertIn('note_file = fopen(path.c_str(), same ? "ab" : "wb");', open_)

    def test_state_record_opens_through_the_shared_function(self):
        record = block(self.draw, "void state_record(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, const uint8_t color_types[8]) {")
        self.assertIn("if (!state_file) state_open_locked();", record)
        self.assertNotIn("state_list_read", record)
        open_ = block(self.draw, "void state_open_locked()")
        # (the records as the file has them, hashed as state_record hashes the ones it writes; a version 1 list kept aside)
        self.assertIn("while (fread(&r, sizeof r, 1, f) == 1) state_seen.insert(state_hash(r))", open_)
        self.assertIn('state_list_path("hookshot_pipeline_states_v1.bin")', open_)

    def test_the_precompile_opens_both_before_play_with_the_notes_it_read(self):
        run = block(self.draw, "void precompile_run(void (*progress)(size_t done, size_t total))")
        read = run.index("std::vector<std::unique_ptr<PipeNote>> notes = pipe_notes_read();")
        notes = run.index("if (!note_file) notes_open_locked(&notes);")
        states = run.index("if (!state_file) state_open_locked();")
        self.assertLess(read, notes)
        self.assertLess(notes, run.index("program_cache_load_some("))
        self.assertLess(states, run.index("program_cache_load_some("))
        self.assertIn('if (!env_on("RX_VK_NO_PROGRAM_CACHE"))', run[read:notes])
        self.assertIn('if (!env_on("RX_VK_NO_STATE_LIST"))', run[notes:states])
        self.assertIn("std::lock_guard lk(note_mu);", run[read:notes])
        self.assertIn("std::lock_guard lk(state_mu);", run[notes:states])


if __name__ == "__main__":
    unittest.main()
