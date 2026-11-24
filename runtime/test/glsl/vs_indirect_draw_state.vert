#version 460
// Synthetic counterpart of native_vs in d3d12_indirect_draw_state_test.cpp.
// An OR of fetched vertex markers is independent of post-transform-cache reuse.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(location = 0) in vec2 position;
layout(location = 1) in uint marker;
layout(location = 2) in vec4 instance_color;
layout(location = 0) out vec4 color;
layout(buffer_reference, std430, buffer_reference_align = 4) buffer Counter { uint bits; };
layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

void main() {
    uint carry;
    uint low = uaddCarry(pc.heap_base.x, pc.s[4], carry);
    Counter output_bits = Counter(uvec2(low, pc.heap_base.y + pc.s[5] + carry));
    atomicOr(output_bits.bits, 1u << marker);
    gl_Position = vec4(position + vec2(uintBitsToFloat(pc.s[0]), uintBitsToFloat(pc.s[1])), 0.5, 1.0);
    color = instance_color;
}
