#version 450
// Multipass lighting: the G-buffer comes back through the texture unit
// (texelFetch on images written by a previous render pass), not as input
// attachments. Same arithmetic as gmem_light.frag.
layout(set = 0, binding = 0) uniform sampler2D t0;
layout(set = 0, binding = 1) uniform sampler2D t1;
layout(set = 0, binding = 2) uniform sampler2D t2;
layout(set = 0, binding = 3) uniform sampler2D t3;

layout(location = 0) out vec4 o;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 a = texelFetch(t0, p, 0);
    vec4 b = texelFetch(t1, p, 0);
    vec4 c = texelFetch(t2, p, 0);
    vec4 d = texelFetch(t3, p, 0);
    o = vec4(fract(a.x + b.y + c.z * 2.0),
             fract(a.y * 3.0 + d.x),
             fract(b.x + c.y + d.y),
             1.0);
}
