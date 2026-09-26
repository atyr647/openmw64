#ifndef OPENMW_N64_MESHLOADER_HPP
#define OPENMW_N64_MESHLOADER_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <GL/gl.h>
#include <osg/Vec3f>

namespace OMW64
{
    class DataFiles;
    class TextureCache;
    struct Texture;

    // One vertex as libdragon GL consumes it from a buffer object.
    struct Vertex
    {
        float mPos[3];
        float mUV[2];
        std::int8_t mNormal[3];
        std::int8_t mPad;
        std::uint8_t mColor[4];
    };

    // A run of triangles sharing one texture and render state.
    //
    // Geometry is built in mVertices/mIndices, then moved into GL buffer
    // objects drawn through a vertex array. With `make DISPLAY_LISTS=1` it
    // is recorded into a display list instead: libdragon converts the
    // vertices to the RSP's fixed-point format once, and each frame just
    // replays the command block, instead of the VR4300 re-converting every
    // vertex every frame. That is about twice as fast but crashes
    // libdragon's RSP GL pipeline now and then (preview 39d0d60), and always
    // for parts cut into texture pages, which therefore never use one.
    struct MeshPart
    {
        std::vector<Vertex> mVertices;
        std::vector<std::uint16_t> mIndices;
        GLuint mList = 0; // display list replaying the draw on the RSP
        GLuint mVertexArray = 0; // only for parts drawn without a display list
        GLuint mBuffers[2] = { 0, 0 };
        GLsizei mIndexCount = 0;
        const Texture* mTexture = nullptr;
        int mPage = -1; // index into mTexture->mPages, or -1 for the whole (small) texture
        float mEmissive[3] = { 0.f, 0.f, 0.f };
        bool mAlphaTest = false;
        bool mTwoSided = false;

        MeshPart() = default;
        MeshPart(MeshPart&& other) noexcept;
        MeshPart& operator=(MeshPart&&) = delete;
        ~MeshPart();

        void upload(bool displayList);
    };

    // A NIF flattened for drawing: every node transform is baked into the
    // vertices, so one model matrix per placed object is all the GPU needs.
    //
    // libdragon's RSP converts positions to 16-bit fixed point with 5
    // fractional bits, so model- and eye-space coordinates must stay within
    // +-1024. The renderer therefore works in "render units" of
    // sUnitsPerRenderUnit Morrowind units, and vertices are stored divided by
    // mVertexScale (sUnitsPerRenderUnit, or a larger power of two for big
    // meshes, which the renderer undoes with glScalef).
    constexpr float sUnitsPerRenderUnit = 4.f;

    struct Model
    {
        std::vector<MeshPart> mParts;
        osg::Vec3f mCenter; // in Morrowind units
        float mRadius = 0.f;
        float mVertexScale = sUnitsPerRenderUnit;
        std::size_t mTriangles = 0;
    };

    class ModelCache
    {
    public:
        ModelCache(const DataFiles& data, TextureCache& textures)
            : mData(data)
            , mTextures(textures)
        {
        }

        // path is relative to meshes\ as stored in the ESM. Returns nullptr if
        // the file is missing or has no drawable geometry.
        const Model* get(std::string_view path);

        void clear() { mModels.clear(); }
        std::size_t size() const { return mModels.size(); }

    private:
        std::unique_ptr<Model> load(const std::string& path);

        const DataFiles& mData;
        TextureCache& mTextures;
        std::map<std::string, std::unique_ptr<Model>, std::less<>> mModels;
    };
}

#endif
