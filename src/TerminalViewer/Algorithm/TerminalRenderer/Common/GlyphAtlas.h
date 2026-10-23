#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>

#include <cstdint>
#include <vector>

enum class EAtlasFormat { A8, RGBA8 };   // QImage::Format_Alpha8 / Format_RGBA8888_Premultiplied

class GlyphAtlas
{
public:
    static constexpr int kDefaultInitialSize = 2048;
    static constexpr int kDefaultMaxSize = 4096;

    explicit GlyphAtlas(EAtlasFormat format, int initialSize = kDefaultInitialSize, int maxSize = kDefaultMaxSize);

    GlyphAtlas(const GlyphAtlas&) = delete;              // 복사하면 GetInstanceId 가 겹친다
    GlyphAtlas& operator=(const GlyphAtlas&) = delete;

public:
    // 자리를 잡아 픽셀을 복사한다(형식이 다르면 변환). 빈 이미지는 넣지 않는다.
    // 공간이 없으면 maxSize 까지 2배로 키우고(기존 좌표 유지), 그래도 없으면 false.
    bool Insert(const QImage& image, QPoint& outTopLeft);
    // 비우고 세대 +1. 크기는 그대로 둔다.
    void Reset();
    // 지난번 부른 뒤 바뀐 사각형. 크기가 바뀌었거나 Reset 뒤면 전체 한 개. 부르면 비운다.
    std::vector<QRect> TakeDirtyRects();

public:
    EAtlasFormat GetFormat() const;
    int GetSize() const;                    // 한 변(정사각형)
    uint32_t GetGeneration() const;
    // 객체마다 다른 번호(1부터, 프로세스 안에서 다시 쓰지 않는다). 넣기·키우기·비우기에는 바뀌지 않는다.
    // GPU 텍스처가 같은 주소에 새로 만든 아틀라스를 알아보는 데 쓴다.
    uint64_t GetInstanceId() const;
    const QImage& GetImage() const;         // CPU 원본, GetSize() × GetSize()
    bool WasResizedSinceLastTake() const;   // GPU 가 텍스처를 다시 만들어야 하는지

private:
    struct Shelf
    {
        int iY = 0;
        int iHeight = 0;
        int iNextX = 0;
    };

    bool TryPlace(int width, int height, QPoint& outTopLeft);
    void Grow();
    QImage MakeBlank(int size) const;

    EAtlasFormat m_format;
    int m_iSize;
    int m_iMaxSize;
    uint32_t m_uiGeneration;
    uint64_t m_uInstanceId;
    QImage m_image;
    std::vector<Shelf> m_vecShelves;
    std::vector<QRect> m_vecDirty;
    bool m_bFullDirty;
    bool m_bResized;
};
