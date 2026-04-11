#version 410 core

in vec2 texCoord;
out vec4 outColor;

uniform sampler2D uAtlas;
uniform vec4 uTextColor;

void main()
{
    float alpha = texture(uAtlas, texCoord).r;
    outColor = vec4(uTextColor.rgb, uTextColor.a * alpha);
}
