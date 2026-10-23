// terminal.vert
#version 450
layout(location = 0) in ivec2 inPos;        // R16G16_SINT  offset 0  (iX, iY)
layout(location = 1) in uvec2 inSize;       // R16G16_UINT  offset 4
layout(location = 2) in uvec2 inAtlas;      // R16G16_UINT  offset 8
layout(location = 3) in uint inColor;       // R32_UINT     offset 12
layout(location = 4) in ivec2 inKindRow;    // R16G16_SINT  offset 16 (x = eKind | uReserved<<8, y = iRow)
layout(push_constant) uniform Push
{
    vec2 viewportSize;   // 장치 픽셀
    float padding;
    float cellHeight;
    float scrollOffset;  // 정수 장치 픽셀
} pc;
layout(location = 0) out vec2 outAtlasPx;
layout(location = 1) flat out vec4 outColor;
layout(location = 2) flat out uint outKind;
void main()
{
    vec2 corner = vec2(float(gl_VertexIndex & 1), float((gl_VertexIndex >> 1) & 1));   // 4 정점 triangle strip
    vec2 size = vec2(inSize);
    vec2 topLeft = vec2(pc.padding + float(inPos.x),
                        pc.padding + float(inKindRow.y) * pc.cellHeight + float(inPos.y) - pc.scrollOffset);
    vec2 px = topLeft + corner * size;
    gl_Position = vec4(px / pc.viewportSize * 2.0 - 1.0, 0.0, 1.0);
    outAtlasPx = vec2(inAtlas) + corner * size;
    outColor = unpackUnorm4x8(inColor);
    outKind = uint(inKindRow.x) & 0xFFu;
}
