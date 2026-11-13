"""The read-back command buffer is not reset while a timed-out submission may still hold it, and is not used half made.

readback_submit_wait waits 5 s for its fence. When that timed out, the next readback_begin called vkResetCommandPool and
vkBeginCommandBuffer on a command buffer the GPU could still be running (VUID-vkResetCommandPool-commandPool-00040,
VUID-vkBeginCommandBuffer-commandBuffer-00049), and readback_submit_wait reset a fence still tied to it
(VUID-vkResetFences-pFences-01123). Now the timed-out submission is remembered and waited for first.

When vkAllocateCommandBuffers or vkCreateFence failed, rb_pool stayed set with rb_cmd or rb_fence null: the next call
skipped the creation and began a null command buffer. Now the pool is destroyed and all three made again next time.

python -I -B tests/test_readback_pending.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"


def body_of(src, head):
	b = src[src.index(head):]
	return b[:b.index("\n}\n")]


def main():
	tex = (VK / "vk_texture.cpp").read_text(encoding="utf-8")
	if "bool rb_pending = false;" not in tex:
		raise SystemExit("a timed-out read-back is not remembered")
	begin = body_of(tex, "bool readback_begin() {")
	pend = begin.find("if (rb_pending)")
	reset = begin.find("vkResetCommandPool(")
	if pend < 0 or reset < 0 or pend > reset:
		raise SystemExit("the read-back pool is reset before a timed-out submission is waited for")
	if "vkWaitForFences(dev.device, 1, &rb_fence" not in begin[pend:reset]:
		raise SystemExit("a timed-out read-back is not waited for before its command buffer is reused")
	made = begin[begin.index("vkAllocateCommandBuffers("):reset]
	if "vkDestroyCommandPool(dev.device, rb_pool, nullptr);" not in made or "rb_pool = VK_NULL_HANDLE" not in made:
		raise SystemExit("a read-back pool whose command buffer or fence failed is kept half made")
	submit = body_of(tex, "bool readback_submit_wait() {")
	if "rb_pending = vkWaitForFences(" not in submit:
		raise SystemExit("readback_submit_wait does not note a wait that timed out")
	print("the read-back command buffer is never reused while pending, nor used half made")


if __name__ == "__main__":
	main()
