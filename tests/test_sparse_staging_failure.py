"""A sparse texture's upload that runs out of staging memory binds what it queued before it gives up.

The upload queues the sparse binds of each run of rows (texture_resident_queue) into sparse_binds, one list for every
texture, and binds them with texture_resident_flush. When staging failed part-way, upload_inner returned false with the
binds of the rows before still queued: their tiles were marked bound but were not, and the next flush, made for another
texture, bound that memory into the other texture's image (VkSparseImageMemoryBindInfo takes one image for the list).
Now the failure flushes first, as the other ways out of the loop do.

python -I -B tests/test_sparse_staging_failure.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def main():
	tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
	body = tex[tex.index("bool upload_inner(Texture& tex, const Ranges* dirty) {"):]
	body = body[:body.index("\n}\n")]
	if "texture_resident_queue(&tex" not in body:
		raise SystemExit("upload_inner no longer queues sparse binds: this test needs updating")
	queued = body.index("texture_resident_queue(&tex")
	# (every return false after the first staging call can leave binds queued by the rows before)
	loop = body[body.index("const Staging st = staging((out_bytes"):queued]
	at = 0
	while True:
		at = loop.find("return false;", at)
		if at < 0:
			break
		before = loop[max(0, at - 400):at]
		if "texture_resident_flush(&tex);" not in before[before.rfind("{"):]:
			raise SystemExit("a sparse upload that fails part-way leaves its queued binds for another texture's flush")
		at += 1
	if "if (!st.buffer) return false;" in body:
		raise SystemExit("a staging failure returns without binding the sparse blocks queued before it")
	print("a sparse upload that fails part-way binds what it queued")


if __name__ == "__main__":
	main()
