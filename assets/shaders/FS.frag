#version 330 core
in vec2 vUV;
in vec3 vN;
out vec4 oColor;

uniform sampler2D uTex;
uniform int uUseTex;
uniform vec4 uColor;

void main()
{
  float ndotl = max(dot(normalize(vN), normalize(vec3(0.3,1.0,0.2))), 0.0);
  vec4 c = uColor;
  if (uUseTex != 0) c = texture(uTex, vUV);
  oColor = vec4(c.rgb * (0.2 + 0.8 * ndotl), c.a);
}
