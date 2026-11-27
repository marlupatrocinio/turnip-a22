#version 450
// a22cpu: one UBO and one sampled texture, like a typical emulator draw.
layout(set = 0, binding = 0) uniform UBO { vec4 scale; } ubo;
layout(set = 0, binding = 1) uniform sampler2D tex;
layout(location = 0) out vec4 o;

void main()
{
    o = texture(tex, gl_FragCoord.xy / 256.0) * ubo.scale.z;
}
