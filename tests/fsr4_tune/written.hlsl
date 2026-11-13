// A table the shader writes into itself (tests/test_fsr4_tune.py): it must stay where it is. Built in this folder with:
//   dxc -spirv -T cs_6_0 -E fsr4_model_test_pass3 -fspv-target-env=vulkan1.3 -Fo written.spv written.hlsl
RWByteAddressBuffer ScratchBuffer : register(u0);
[numthreads(64, 1, 1)]
void fsr4_model_test_pass3(uint3 id : SV_DispatchThreadID) {
    uint t[4] = { 1u, 2u, 3u, 4u };
    t[ScratchBuffer.Load(0) & 3] = ScratchBuffer.Load(4);
    uint acc = 0;
    for (uint i = 0; i < 4; i++) acc += t[(i + id.x) & 3];
    ScratchBuffer.Store(id.x * 4, acc);
}
