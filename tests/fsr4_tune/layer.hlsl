// AMD FSR 4's network layers in miniature, for tests/test_fsr4_tune.py: constant weight tables read at loop-dependent
// places (one copied in once, one copied in at every iteration as FSR 4's own layers do), a writable and a read-only
// byte-address buffer, counted loops three deep, each with its counter alone (the sum kept in memory, as FSR 4's layers
// keep theirs in arrays). Built in this folder with:
//   dxc -spirv -T cs_6_0 -E fsr4_model_test_pass7 -fspv-target-env=vulkan1.3 -Fo layer.spv layer.hlsl
RWByteAddressBuffer ScratchBuffer : register(u0);
ByteAddressBuffer InitializerBuffer : register(t1);
static const uint W[8] = { 0x0A0B0C0Du, 3u, 0xFFFFFFF0u, 77u, 5u, 0x12345678u, 9u, 1u };
[numthreads(64, 1, 1)]
void fsr4_model_test_pass7(uint3 id : SV_DispatchThreadID) {
    const uint o = id.x * 4;
    [unroll] for (uint i = 0; i < 4; i++)
        [unroll] for (uint j = 0; j < 2; j++)
            for (uint k = 0; k < 3; k++) {
                const uint V[4] = { 11u, 22u, 33u, 44u };
                ScratchBuffer.Store(o, ScratchBuffer.Load(o) + W[i * 2 + j] * ScratchBuffer.Load((id.x + k + 64) * 4) +
                                       InitializerBuffer.Load(k * 4) + V[(k + i) & 3]);
            }
}
