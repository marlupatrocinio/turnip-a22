#version 450
// Screen-space quad from push constants; no vertex buffers.
layout(push_constant) uniform PC {
    vec4 rect;   // x0, y0, x1, y1 in NDC
    vec4 misc;   // x = depth, y = draw id
} pc;

void main()
{
    const vec2 c[6] = vec2[](vec2(0, 0), vec2(1, 0), vec2(0, 1),
                             vec2(1, 0), vec2(1, 1), vec2(0, 1));
    vec2 t = c[gl_VertexIndex];
    gl_Position = vec4(mix(pc.rect.xy, pc.rect.zw, t), pc.misc.x, 1.0);
}
