#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aN;
layout(location=2) in vec2 aUV;

uniform mat4 uMVP;

out vec2 vUV;
out vec3 vN;

void main()
{
  vUV = aUV;
  vN = aN;
  gl_Position = uMVP * vec4(aPos, 1.0);
}