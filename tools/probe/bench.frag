#version 450

layout(location = 0) out vec4 out_color;

void main()
{
    out_color = vec4(gl_FragCoord.x / 1920.0, gl_FragCoord.y / 1080.0, 0.5, 1.0);
}
