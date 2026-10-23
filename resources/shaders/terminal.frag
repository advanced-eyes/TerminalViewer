// terminal.frag
#version 450
layout(set = 0, binding = 0) uniform sampler2D monoAtlas;    // R8_UNORM
layout(set = 0, binding = 1) uniform sampler2D colorAtlas;   // R8G8B8A8_UNORM (premultiplied)
layout(location = 0) in vec2 inAtlasPx;
layout(location = 1) flat in vec4 inColor;
layout(location = 2) flat in uint inKind;
layout(location = 0) out vec4 outColor;
void main()
{
    if (inKind == 0u)      { outColor = inColor; }
    else if (inKind == 1u) { outColor = inColor * texelFetch(monoAtlas, ivec2(inAtlasPx), 0).r; }
    else                   { outColor = texelFetch(colorAtlas, ivec2(inAtlasPx), 0); }
}
