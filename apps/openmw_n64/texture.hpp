#ifndef OPENMW_N64_TEXTURE_HPP
#define OPENMW_N64_TEXTURE_HPP

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include <GL/gl.h>
#include <surface.h>

namespace OMW64
{
    class DataFiles;

    struct Texture
    {
        surface_t mSurface{};
        GLuint mName = 0;
        bool mHasAlpha = false;

        ~Texture();
    };

    // Loads Morrowind textures (DDS: DXT1/3/5 and uncompressed; TGA) and
    // shrinks them to fit TMEM: at most 1024 texels, stored as RGBA 5551
    // (2 KB). DDS files ship with mipmaps, so this usually just decodes the
    // small mip level instead of resampling the full image.
    class TextureCache
    {
    public:
        explicit TextureCache(const DataFiles& data)
            : mData(data)
        {
        }

        // Takes the path as written in a NIF (e.g. "tx_wood.tga") and resolves
        // it like OpenMW's Misc::ResourceHelpers::correctTexturePath: prefix
        // "textures\" and prefer a .dds of the same name. Returns nullptr if
        // the texture cannot be found or decoded.
        const Texture* get(std::string_view nifPath);

        void clear() { mTextures.clear(); }
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
    };
}

#endif
