#version 450
// G-buffer pass: up to eight outputs that depend on pixel position and draw
// id, so a pixel written to (or resolved from) the wrong place in GMEM is
// detectable. Outputs without an attachment are discarded by Vulkan.
// All values are in [0, 1] so unsigned formats (R11G11B10, RGB10A2) keep them.
layout(push_constant) uniform PC {
    vec4 rect;
    vec4 misc;
} pc;

layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
layout(location = 4) out vec4 o4;
layout(location = 5) out vec4 o5;
layout(location = 6) out vec4 o6;
layout(location = 7) out vec4 o7;

void main()
{
    float id = pc.misc.y;
    vec2 p = gl_FragCoord.xy;
    o0 = vec4(fract(p / vec2(61.0, 67.0)), fract(id * 0.0137), 1.0);
    o1 = vec4(fract(id * 0.031), fract(p.x * 0.0071 + id * 0.11), fract(p.y * 0.0053), 0.5);
    o2 = vec4(fract(p.y / 89.0), fract(id * 0.071), fract((p.x + p.y) / 131.0), 1.0);
    o3 = vec4(fract((p.x * 3.0 + id) / 211.0), fract(p.y / 41.0), 0.25, fract(id * 0.0091));
    o4 = vec4(fract(p.x / 23.0), fract(p.y / 29.0 + id * 0.013), fract(id * 0.17), 0.75);
    o5 = vec4(fract((p.x - p.y) / 97.0 + 0.5), fract(id * 0.29), fract(p.x * p.y / 4093.0), 1.0);
    o6 = vec4(fract(id * 0.0071 + p.y / 307.0), 0.125, fract(p.x / 13.0), fract(p.y / 17.0));
    o7 = vec4(fract(p.x / 509.0), fract(p.y / 503.0), fract(id * 0.41), 0.0625);
}
