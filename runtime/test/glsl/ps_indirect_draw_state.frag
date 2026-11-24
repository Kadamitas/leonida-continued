#version 460
// Root CBV tint at s2:s3, using the same VA-relative contract as translated shaders.
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

layout(location = 0) in vec4 color;
layout(location = 0) out vec4 target;
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Tint { vec4 value; };
layout(push_constant) uniform UserData {
    uint s[32];
    uvec2 heap_base;
    uvec2 code_base;
} pc;

void main() {
    uint carry;
    uint low = uaddCarry(pc.heap_base.x, pc.s[2], carry);
    Tint tint = Tint(uvec2(low, pc.heap_base.y + pc.s[3] + carry));
    target = color * tint.value;
}
