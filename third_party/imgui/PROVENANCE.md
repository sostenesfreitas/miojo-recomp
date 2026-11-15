Dear ImGui v1.92.9b, from https://github.com/ocornut/imgui (tag v1.92.9b, archive sha256
21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99), unmodified.
MIT license: LICENSE.txt.

Kept: the core (imgui*.cpp, imgui*.h, imstb_*.h, imconfig.h) and the Vulkan renderer backend
(backends/imgui_impl_vulkan.*). Left out: the demo, the examples, the docs, the other backends and misc/.
Settings are given as compiler defines by nx2/build.py, not by editing imconfig.h.
Used by gpu/vk/vk_imgui.cpp and vk_menu.cpp: the Insert menu, the FPS counter and the start-up screen, drawn over the
window's image. ImGui's own widgets and input are not used: the menu draws on its draw lists and keeps its own keys, gamepad
and mouse. Textures go to the GPU through vk_imgui.cpp's own copies, not the backend's (which waits on the queue).

upstream: https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b.tar.gz
upstream-sha256: 21d8a0a565e85dce943e375db00812c2f3f0ab21f3f0f7964e364a63422d7f99
upstream-root: imgui-1.92.9b

tree-sha256: aa55f1b9fd78ed8bafee44a2659fba7574812b99eae8b52b7d51586434c2205a
