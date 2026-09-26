#ifndef OPENMW_N64_TEXTURE_HPP
#define OPENMW_N64_TEXTURE_HPP

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <GL/gl.h>
#include <surface.h>

namespace OMW64
{
    class DataFiles;

    // One TMEM-sized piece of a texture drawn at native resolution.
    struct TexturePage
    {
        surface_t mSurface{};
        GLuint mName = 0;
    };

    // Texels per side of a page: 32x32 RGBA16 = 2 KB, half of TMEM.
    constexpr int sPageSize = 32;

    struct Texture
    {
        // Whole texture shrunk to <= 1024 texels: used far away, and for any
        // triangle that would need too many pages.
        surface_t mSurface{};
        GLuint mName = 0;
        bool mHasAlpha = false;

        // CHROMA64-style paging (empty when paging is off): the texture at
        // native resolution, cut into mPagesX x mPagesY pages of sPageSize^2
        // texels. Meshes are split along the page grid so that every triangle
        // samples exactly one page, which is all TMEM can hold.
        int mPagedWidth = 0;
        int mPagedHeight = 0;
        int mPagesX = 0;
        int mPagesY = 0;
        std::vector<TexturePage> mPages;

        bool paged() const { return !mPages.empty(); }
        ~Texture();
    };

    // Loads Morrowind textures (DDS: DXT1/3/5 and uncompressed; TGA) and
    // shrinks them to fit TMEM: at most 1024 texels, stored as RGBA 5551
    // (2 KB). DDS files ship with mipmaps, so this usually just decodes the
    // small mip level instead of resampling the full image. With paging on,
    // textures are also kept at native resolution (up to the cap) as pages.
    class TextureCache
    {
    public:
        explicit TextureCache(const DataFiles& data)
            : mData(data)
        {
        }

        // 0 = off; otherwise the largest side, in texels, kept for paging.
        void setPageCap(int maxSide) { mPageCap = maxSide; }
        int pageCap() const { return mPageCap; }
        std::size_t pageBytes() const { return mPageBytes; }

        // Takes the path as written in a NIF (e.g. "tx_wood.tga") and resolves
        // it like OpenMW's Misc::ResourceHelpers::correctTexturePath: prefix
        // "textures\" and prefer a .dds of the same name. Returns nullptr if
        // the texture cannot be found or decoded.
        const Texture* get(std::string_view nifPath);

        void clear()
        {
            mTextures.clear();
            mPageBytes = 0;
        }
        std::size_t size() const { return mTextures.size(); }

        template <class F>
        void forEach(F&& f) const
        {
            for (const auto& [path, texture] : mTextures)
                if (texture)
                    f(path, *texture);
        }

    private:
        std::unique_ptr<Texture> load(const std::string& path);

        const DataFiles& mData;
        std::map<std::string, std::unique_ptr<Texture>, std::less<>> mTextures;
        int mPageCap = 0;
        std::size_t mPageBytes = 0;
    };
}

#endif
