#version 450
layout(set = 0, binding = 0, std430) readonly buffer Color { vec4 value; } input_color;
layout(location = 0) out vec4 color;
void main() { color = input_color.value; }
