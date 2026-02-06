#version 330 core

in vec3 vDir;
out vec4 FragColor;

void main()
{
    vec3 d = normalize(vDir);

    float t = clamp(d.y * 0.5 + 0.5, 0.0, 1.0);
    vec3 horizon = vec3(0.80, 0.86, 0.95);
    vec3 zenith  = vec3(0.10, 0.25, 0.60);
    vec3 col = mix(horizon, zenith, t);

    vec3 sunDir = normalize(vec3(0.2, 0.7, 0.1));
    float sun = pow(max(dot(d, sunDir), 0.0), 256.0);
    col += vec3(1.2, 1.1, 0.9) * sun;

    FragColor = vec4(col, 1.0);
}
