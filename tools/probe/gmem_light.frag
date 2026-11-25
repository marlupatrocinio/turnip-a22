#version 450
// Lighting pass: reads the G-buffer back as input attachments (in GMEM mode
// these come straight from tile memory) and combines them.
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput g0;
layout(input_attachment_index = 1, set = 0, binding = 1) uniform subpassInput g1;
layout(input_attachment_index = 2, set = 0, binding = 2) uniform subpassInput g2;
layout(input_attachment_index = 3, set = 0, binding = 3) uniform subpassInput g3;

layout(location = 0) out vec4 o;

void main()
{
    vec4 a = subpassLoad(g0);
    vec4 b = subpassLoad(g1);
    vec4 c = subpassLoad(g2);
    vec4 d = subpassLoad(g3);
    o = vec4(fract(a.x + b.y + c.z * 2.0),
             fract(a.y * 3.0 + d.x),
             fract(b.x + c.y + d.y),
             1.0);
}
