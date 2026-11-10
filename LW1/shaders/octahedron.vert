#version 450
layout(location=0) in vec3 position;
layout(location=0) out vec3 color;
layout(set=0,binding=0) uniform ObjectData { mat4 mvp; vec4 tint; vec4 options; } object;
void main() {
    gl_Position = object.mvp * vec4(position, 1.0);
    vec3 vertexColor = position * 0.5 + 0.5;
    color = object.tint.rgb * mix(vec3(1.0), vertexColor, object.options.x);
}
