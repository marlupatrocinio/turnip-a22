#version 450
// a22cpu: trivial vertex stage; the benchmark measures driver CPU cost, not GPU work.
layout(location = 0) in vec2 pos;
layout(push_constant) uniform PC { vec4 offset; } pc;
layout(set = 0, binding = 0) uniform UBO { vec4 scale; } ubo;

void main()
{
    gl_Position = vec4(pos * ubo.scale.xy + pc.offset.xy, 0.5, 1.0);
}
