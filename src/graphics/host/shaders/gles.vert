#version 450

layout(location = 0) in vec4 in_position;
layout(location = 1) in vec4 in_color;
layout(location = 2) in vec2 in_texture0;
layout(location = 3) in vec2 in_texture1;
layout(location = 0) out vec4 primary_color;
layout(location = 1) out vec2 texture0;
layout(location = 2) out vec2 texture1;

void main() {
    gl_Position = vec4(
        in_position.x, -in_position.y, in_position.z, in_position.w);
    primary_color = in_color;
    texture0 = in_texture0;
    texture1 = in_texture1;
}
