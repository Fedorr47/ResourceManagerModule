#version 330 core

layout(location = 0) in vec3 aPos;   // Position из VertexDesc

out vec3 vDir;

uniform mat4 uVP;

void main()
{
    vDir = aPos;

    vec4 clip = uVP * vec4(aPos, 1.0);

    // "Прижать" к far plane: z = w -> после деления z = 1
    gl_Position = vec4(clip.xy, clip.w, clip.w);
}
