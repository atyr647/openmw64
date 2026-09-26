#include "texture.hpp"

#include <algorithm>
#include <cstring>
#include <istream>
#include <vector>

#include <n64sys.h>
#include <rdpq_tex.h>

#include <components/debug/debuglog.hpp>

#include "datafiles.hpp"

namespace OMW64
{
    namespace
    {
        // The largest texture we upload: 1024 texels of RGBA16 = 2 KB, half of
        // TMEM, which leaves room for libdragon GL's own use of it.
        constexpr int sMaxTexels = 1024;

        struct Rgba
        {
            int r = 0, g = 0, b = 0, a = 255;
        };

        std::uint32_t le32(const std::uint8_t* p)
        {
            return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
        }

        std::uint16_t le16(const std::uint8_t* p)
        {
            return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
        }

        Rgba from565(std::uint16_t c)
        {
            Rgba out;
            out.r = ((c >> 11) & 31) * 255 / 31;
            out.g = ((c >> 5) & 63) * 255 / 63;
            out.b = (c & 31) * 255 / 31;
            return out;
        }

        // A decoded image we can sample one texel at a time. Keeping DXT data
        // compressed and decoding only the texels we sample avoids needing a
        // full-size RGBA buffer, which may not fit in RDRAM.
        class Image
        {
        public:
            enum class Format
            {
                DXT1,
                DXT3,
                DXT5,
                RGBA, // 8-bit channels with arbitrary masks
            };

            int mWidth = 0;
            int mHeight = 0;
            Format mFormat = Format::RGBA;
            int mBytesPerPixel = 4;
            std::uint32_t mMasks[4] = {}; // r g b a
            bool mTopDown = true;
            std::vector<std::uint8_t> mData;

            Rgba texel(int x, int y) const
            {
                x = std::clamp(x, 0, mWidth - 1);
                y = std::clamp(y, 0, mHeight - 1);
                if (!mTopDown)
                    y = mHeight - 1 - y;
                switch (mFormat)
                {
                    case Format::DXT1:
                        return dxt(x, y, 8, false);
                    case Format::DXT3:
                    case Format::DXT5:
                        return dxt(x, y, 16, true);
                    case Format::RGBA:
                        return rgba(x, y);
                }
                return {};
            }

        private:
            Rgba dxt(int x, int y, int blockBytes, bool separateAlpha) const
            {
                const int blocksWide = std::max(1, (mWidth + 3) / 4);
                const std::size_t offset = (static_cast<std::size_t>(y / 4) * blocksWide + x / 4) * blockBytes;
                if (offset + blockBytes > mData.size())
                    return {};
                const std::uint8_t* block = mData.data() + offset;
                const int px = x & 3;
                const int py = y & 3;

                int alpha = 255;
                if (separateAlpha)
                {
                    if (mFormat == Format::DXT3)
                    {
                        const int nibble = (block[py * 2 + px / 2] >> ((px & 1) * 4)) & 15;
                        alpha = nibble * 17;
                    }
                    else
                    {
                        const int a0 = block[0];
                        const int a1 = block[1];
                        std::uint64_t bits = 0;
                        for (int i = 0; i < 6; ++i)
                            bits |= static_cast<std::uint64_t>(block[2 + i]) << (8 * i);
                        const int code = static_cast<int>((bits >> (3 * (py * 4 + px))) & 7);
                        if (code == 0)
                            alpha = a0;
                        else if (code == 1)
                            alpha = a1;
                        else if (a0 > a1)
                            alpha = ((8 - code) * a0 + (code - 1) * a1) / 7;
                        else if (code == 6)
                            alpha = 0;
                        else if (code == 7)
                            alpha = 255;
                        else
                            alpha = ((6 - code) * a0 + (code - 1) * a1) / 5;
                    }
                    block += 8;
                }

                const std::uint16_t c0 = le16(block);
                const std::uint16_t c1 = le16(block + 2);
                const int code = (le32(block + 4) >> (2 * (py * 4 + px))) & 3;
                const Rgba a = from565(c0);
                const Rgba b = from565(c1);
                Rgba out;
                if (code == 0)
                    out = a;
                else if (code == 1)
                    out = b;
                else if (c0 > c1 || separateAlpha)
                {
                    const int wa = code == 2 ? 2 : 1;
                    const int wb = 3 - wa;
                    out.r = (wa * a.r + wb * b.r) / 3;
                    out.g = (wa * a.g + wb * b.g) / 3;
                    out.b = (wa * a.b + wb * b.b) / 3;
                }
                else if (code == 2)
                {
                    out.r = (a.r + b.r) / 2;
                    out.g = (a.g + b.g) / 2;
                    out.b = (a.b + b.b) / 2;
                }
                else
                    out.a = 0; // DXT1 punch-through transparency
                if (separateAlpha)
                    out.a = alpha;
                return out;
            }

            static int channel(std::uint32_t pixel, std::uint32_t mask)
            {
                if (mask == 0)
                    return 255;
                int shift = 0;
                while (!((mask >> shift) & 1))
                    ++shift;
                const std::uint32_t max = mask >> shift;
                return static_cast<int>(((pixel & mask) >> shift) * 255 / max);
            }

            Rgba rgba(int x, int y) const
            {
                const std::size_t offset = (static_cast<std::size_t>(y) * mWidth + x) * mBytesPerPixel;
                if (offset + mBytesPerPixel > mData.size())
                    return {};
                std::uint32_t pixel = 0;
                for (int i = 0; i < mBytesPerPixel; ++i)
                    pixel |= static_cast<std::uint32_t>(mData[offset + i]) << (8 * i);
                return { channel(pixel, mMasks[0]), channel(pixel, mMasks[1]), channel(pixel, mMasks[2]),
                    mMasks[3] ? channel(pixel, mMasks[3]) : 255 };
            }
        };

        std::size_t levelSize(const Image& image, int w, int h)
        {
            switch (image.mFormat)
            {
                case Image::Format::DXT1:
                    return static_cast<std::size_t>(std::max(1, (w + 3) / 4)) * std::max(1, (h + 3) / 4) * 8;
                case Image::Format::DXT3:
                case Image::Format::DXT5:
                    return static_cast<std::size_t>(std::max(1, (w + 3) / 4)) * std::max(1, (h + 3) / 4) * 16;
                case Image::Format::RGBA:
                    return static_cast<std::size_t>(w) * h * image.mBytesPerPixel;
            }
            return 0;
        }

        bool readDds(std::istream& in, Image& image)
        {
            std::uint8_t header[128];
            if (!in.read(reinterpret_cast<char*>(header), sizeof(header)) || std::memcmp(header, "DDS ", 4) != 0)
                return false;
            const int height = static_cast<int>(le32(header + 12));
            const int width = static_cast<int>(le32(header + 16));
            const int mipCount = std::max<int>(1, static_cast<int>(le32(header + 28)));
            const std::uint8_t* pf = header + 76;
            const std::uint32_t pfFlags = le32(pf + 4);
            if (width <= 0 || height <= 0)
                return false;

            if (pfFlags & 0x4) // DDPF_FOURCC
            {
                if (std::memcmp(pf + 8, "DXT1", 4) == 0)
                    image.mFormat = Image::Format::DXT1;
                else if (std::memcmp(pf + 8, "DXT3", 4) == 0)
                    image.mFormat = Image::Format::DXT3;
                else if (std::memcmp(pf + 8, "DXT5", 4) == 0)
                    image.mFormat = Image::Format::DXT5;
                else
                    return false;
            }
            else
            {
                const int bits = static_cast<int>(le32(pf + 12));
                if (bits != 24 && bits != 32)
                    return false;
                image.mFormat = Image::Format::RGBA;
                image.mBytesPerPixel = bits / 8;
                image.mMasks[0] = le32(pf + 16);
                image.mMasks[1] = le32(pf + 20);
                image.mMasks[2] = le32(pf + 24);
                image.mMasks[3] = (pfFlags & 0x1) ? le32(pf + 28) : 0; // DDPF_ALPHAPIXELS
            }

            // Skip to the first mip level that fits, or the smallest there is.
            int w = width;
            int h = height;
            std::size_t skip = 0;
            for (int level = 0; level + 1 < mipCount && w * h > sMaxTexels; ++level)
            {
                skip += levelSize(image, w, h);
                w = std::max(1, w / 2);
                h = std::max(1, h / 2);
            }
            in.seekg(static_cast<std::streamoff>(sizeof(header) + skip));
            image.mWidth = w;
            image.mHeight = h;
            image.mData.resize(levelSize(image, w, h));
            return static_cast<bool>(in.read(reinterpret_cast<char*>(image.mData.data()), image.mData.size()));
        }

        bool readTga(std::istream& in, Image& image)
        {
            std::uint8_t header[18];
            if (!in.read(reinterpret_cast<char*>(header), sizeof(header)))
                return false;
            const int type = header[2];
            const int width = le16(header + 12);
            const int height = le16(header + 14);
            const int bpp = header[16];
            if ((type != 2 && type != 10) || (bpp != 24 && bpp != 32) || width <= 0 || height <= 0 || header[1] != 0)
                return false;
            in.seekg(sizeof(header) + header[0]);

            image.mFormat = Image::Format::RGBA;
            image.mWidth = width;
            image.mHeight = height;
            image.mBytesPerPixel = bpp / 8;
            image.mMasks[0] = 0x00ff0000;
            image.mMasks[1] = 0x0000ff00;
            image.mMasks[2] = 0x000000ff;
            image.mMasks[3] = bpp == 32 ? 0xff000000 : 0;
            image.mTopDown = (header[17] & 0x20) != 0;
            const std::size_t size = static_cast<std::size_t>(width) * height * image.mBytesPerPixel;
            image.mData.resize(size);
            if (type == 2)
                return static_cast<bool>(in.read(reinterpret_cast<char*>(image.mData.data()), size));

            // RLE
            std::size_t pos = 0;
            while (pos < size)
            {
                const int packet = in.get();
                if (packet < 0)
                    return false;
                const int count = (packet & 0x7f) + 1;
                if (packet & 0x80)
                {
                    std::uint8_t pixel[4];
                    in.read(reinterpret_cast<char*>(pixel), image.mBytesPerPixel);
                    for (int i = 0; i < count && pos < size; ++i, pos += image.mBytesPerPixel)
                        std::memcpy(&image.mData[pos], pixel, image.mBytesPerPixel);
                }
                else
                {
                    const std::size_t bytes = std::min<std::size_t>(count * image.mBytesPerPixel, size - pos);
                    in.read(reinterpret_cast<char*>(&image.mData[pos]), bytes);
                    pos += bytes;
                }
            }
            return static_cast<bool>(in);
        }

        std::string replaceExtension(std::string_view path, std::string_view ext)
        {
            const std::size_t dot = path.rfind('.');
            const std::size_t slash = path.find_last_of("\\/");
            std::string result((dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash))
                    ? path.substr(0, dot)
                    : path);
            result += ext;
            return result;
        }
    }

    Texture::~Texture()
    {
        if (mName != 0)
            glDeleteTextures(1, &mName);
        surface_free(&mSurface);
    }

    const Texture* TextureCache::get(std::string_view nifPath)
    {
        std::string path = normalizePath(nifPath);
        while (!path.empty() && (path.front() == '\\' || path.front() == '.'))
            path.erase(0, 1);
        if (path.rfind("textures\\", 0) != 0)
            path = "textures\\" + path;

        auto it = mTextures.find(path);
        if (it != mTextures.end())
            return it->second.get();

        std::unique_ptr<Texture> texture;
        const std::string dds = replaceExtension(path, ".dds");
        if (mData.exists(dds))
            texture = load(dds);
        if (!texture)
            texture = load(path);
        if (!texture)
            Log(Debug::Warning) << "Texture not found: " << path;

        const Texture* result = texture.get();
        mTextures.emplace(std::move(path), std::move(texture));
        return result;
    }

    std::unique_ptr<Texture> TextureCache::load(const std::string& path)
    {
        Files::IStreamPtr stream = mData.open(path);
        if (!stream)
            return nullptr;

        Image image;
        const bool isDds = path.size() > 4 && path.compare(path.size() - 4, 4, ".dds") == 0;
        const bool ok = isDds ? readDds(*stream, image) : readTga(*stream, image);
        if (!ok)
            return nullptr;

        // Halve until it fits; libdragon GL wants power-of-two sizes, which
        // Morrowind's textures already are.
        int w = image.mWidth;
        int h = image.mHeight;
        while (w * h > sMaxTexels)
        {
            if (w >= h)
                w /= 2;
            else
                h /= 2;
        }
        w = std::max(w, 4);
        h = std::max(h, 4);

        auto texture = std::make_unique<Texture>();
        texture->mSurface = surface_alloc(FMT_RGBA16, w, h);
        auto* pixels = static_cast<std::uint16_t*>(texture->mSurface.buffer);
        const int strideTexels = texture->mSurface.stride / 2;
        bool hasAlpha = false;

        // Box-ish filter: average 4 samples spread over each output texel's
        // footprint in the source level.
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                Rgba sum{ 0, 0, 0, 0 };
                int maxAlpha = 0;
                for (int s = 0; s < 4; ++s)
                {
                    const int sx = ((x * 4 + 1 + (s & 1) * 2) * image.mWidth) / (w * 4);
                    const int sy = ((y * 4 + 1 + (s >> 1) * 2) * image.mHeight) / (h * 4);
                    const Rgba t = image.texel(sx, sy);
                    sum.r += t.r;
                    sum.g += t.g;
                    sum.b += t.b;
                    sum.a += t.a;
                    maxAlpha = std::max(maxAlpha, t.a);
                }
                // Alpha-tested foliage would vanish if alpha were averaged, so a
                // texel is opaque when any of its samples is.
                const int a = maxAlpha;
                hasAlpha |= a < 128;
                pixels[y * strideTexels + x] = static_cast<std::uint16_t>(
                    ((sum.r / 4 >> 3) << 11) | ((sum.g / 4 >> 3) << 6) | ((sum.b / 4 >> 3) << 1) | (a >= 128 ? 1 : 0));
            }
        }
        data_cache_hit_writeback(texture->mSurface.buffer, texture->mSurface.stride * h);
        texture->mHasAlpha = hasAlpha;
        Log(Debug::Info) << "Loaded " << path << ": " << image.mWidth << "x" << image.mHeight << " -> " << w << "x" << h
                         << (hasAlpha ? " (alpha)" : "") << ", first texel " << std::hex << pixels[0] << std::dec;

        glGenTextures(1, &texture->mName);
        glBindTexture(GL_TEXTURE_2D, texture->mName);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        // Wrapping is given to libdragon directly, as its own GL demo does.
        rdpq_texparms_t parms{};
        parms.s.repeats = REPEAT_INFINITE;
        parms.t.repeats = REPEAT_INFINITE;
        glSurfaceTexImageN64(GL_TEXTURE_2D, 0, &texture->mSurface, &parms);
        return texture;
    }
}
