#include "TerminalViewer/Algorithm/TerminalRenderer/Common/GlyphAtlas.h"

#include <algorithm>
#include <atomic>
#include <cstring>

namespace
{
constexpr int kShelfGap = 1;

// 아틀라스 객체 번호. 0 은 "없음" 으로 남기고 1 부터 준다. 프로세스 안에서 다시 쓰지 않는다.
uint64_t NextInstanceId()
{
    static std::atomic<uint64_t> s_uNext{1};
    return s_uNext.fetch_add(1);
}

QImage::Format ToImageFormat(EAtlasFormat format)
{
    return format == EAtlasFormat::A8 ? QImage::Format_Alpha8 : QImage::Format_RGBA8888_Premultiplied;
}

int BytesPerPixel(EAtlasFormat format)
{
    return format == EAtlasFormat::A8 ? 1 : 4;
}
}

GlyphAtlas::GlyphAtlas(EAtlasFormat format, int initialSize, int maxSize)
    : m_format(format)
    , m_iSize(std::max(1, initialSize))
    , m_iMaxSize(std::max(m_iSize, maxSize))
    , m_uiGeneration(0)
    , m_uInstanceId(NextInstanceId())
    , m_image(MakeBlank(m_iSize))
    , m_vecShelves()
    , m_vecDirty()
    , m_bFullDirty(false)
    , m_bResized(false)
{
}

QImage GlyphAtlas::MakeBlank(int size) const
{
    QImage image(size, size, ToImageFormat(m_format));
    image.fill(0);
    return image;
}

bool GlyphAtlas::TryPlace(int width, int height, QPoint& outTopLeft)
{
    if (width > m_iSize || height > m_iSize)
        return false;

    if (!m_vecShelves.empty())
    {
        Shelf& last = m_vecShelves.back();
        if (last.iNextX + width <= m_iSize && last.iY + std::max(last.iHeight, height) <= m_iSize)
        {
            outTopLeft = QPoint(last.iNextX, last.iY);
            last.iNextX += width + kShelfGap;
            last.iHeight = std::max(last.iHeight, height);
            return true;
        }
    }

    const int iY = m_vecShelves.empty() ? 0 : m_vecShelves.back().iY + m_vecShelves.back().iHeight + kShelfGap;
    if (iY + height > m_iSize)
        return false;

    Shelf shelf;
    shelf.iY = iY;
    shelf.iHeight = height;
    shelf.iNextX = width + kShelfGap;
    m_vecShelves.push_back(shelf);
    outTopLeft = QPoint(0, iY);
    return true;
}

void GlyphAtlas::Grow()
{
    const int iNewSize = std::min(m_iSize * 2, m_iMaxSize);
    QImage bigger = MakeBlank(iNewSize);
    for (int iY = 0; iY < m_iSize; ++iY)
        std::memcpy(bigger.scanLine(iY), m_image.constScanLine(iY), size_t(m_iSize) * size_t(BytesPerPixel(m_format)));
    m_image = bigger;
    m_iSize = iNewSize;
    m_bResized = true;
    m_bFullDirty = true;
    m_vecDirty.clear();
}

bool GlyphAtlas::Insert(const QImage& image, QPoint& outTopLeft)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return false;
    if (image.width() > m_iMaxSize || image.height() > m_iMaxSize)
        return false;

    QPoint topLeft;
    while (!TryPlace(image.width(), image.height(), topLeft))
    {
        if (m_iSize >= m_iMaxSize)
            return false;
        Grow();
    }

    const QImage converted = image.format() == m_image.format() ? image : image.convertToFormat(m_image.format());
    const int iBpp = BytesPerPixel(m_format);
    for (int iY = 0; iY < converted.height(); ++iY)
        std::memcpy(m_image.scanLine(topLeft.y() + iY) + topLeft.x() * iBpp, converted.constScanLine(iY), size_t(converted.width()) * size_t(iBpp));

    if (!m_bFullDirty)
        m_vecDirty.push_back(QRect(topLeft, converted.size()));
    outTopLeft = topLeft;
    return true;
}

void GlyphAtlas::Reset()
{
    m_image.fill(0);
    m_vecShelves.clear();
    ++m_uiGeneration;
    m_bFullDirty = true;
    m_vecDirty.clear();
}

EAtlasFormat GlyphAtlas::GetFormat() const
{
    return m_format;
}

int GlyphAtlas::GetSize() const
{
    return m_iSize;
}

uint32_t GlyphAtlas::GetGeneration() const
{
    return m_uiGeneration;
}

uint64_t GlyphAtlas::GetInstanceId() const
{
    return m_uInstanceId;
}

const QImage& GlyphAtlas::GetImage() const
{
    return m_image;
}

std::vector<QRect> GlyphAtlas::TakeDirtyRects()
{
    std::vector<QRect> vecResult;
    if (m_bFullDirty)
        vecResult.push_back(QRect(0, 0, m_iSize, m_iSize));
    else
        vecResult.swap(m_vecDirty);
    m_vecDirty.clear();
    m_bFullDirty = false;
    m_bResized = false;
    return vecResult;
}

bool GlyphAtlas::WasResizedSinceLastTake() const
{
    return m_bResized;
}
