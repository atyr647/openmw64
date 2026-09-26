#include "meshloader.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <cstddef>

#include <osg/Matrixf>

#include <components/debug/debuglog.hpp>
#include <components/nif/data.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/nif/property.hpp>
#include <components/nif/texture.hpp>
#include <components/vfs/pathutil.hpp>

#include "datafiles.hpp"
#include "texture.hpp"

namespace OMW64
{
    namespace
    {
        // Display lists roughly double the frame rate, but libdragon's RSP GL
        // pipeline (preview 39d0d60) sometimes crashes replaying them -- an
        // RSP/RDP hang that comes and goes with unrelated code changes -- so
        // they are opt-in: `make DISPLAY_LISTS=1`.
#ifdef OMW64_DISPLAY_LISTS
        constexpr bool sUseDisplayLists = true;
#else
        constexpr bool sUseDisplayLists = false;
#endif

        // Render state inherited down the NIF scene graph, the way
        // components/nifosg applies NiProperty records to child nodes.
        struct PropertyState
        {
            const Nif::NiTexturingProperty* mTexturing = nullptr;
            const Nif::NiAlphaProperty* mAlpha = nullptr;
            const Nif::NiMaterialProperty* mMaterial = nullptr;
            const Nif::NiStencilProperty* mStencil = nullptr;

            void apply(const Nif::NiAVObject& node)
            {
                for (const auto& ptr : node.mProperties)
                {
                    if (ptr.empty())
                        continue;
                    const Nif::NiProperty* prop = ptr.getPtr();
                    switch (prop->mRecordType)
                    {
                        case Nif::RC_NiTexturingProperty:
                            mTexturing = static_cast<const Nif::NiTexturingProperty*>(prop);
                            break;
                        case Nif::RC_NiAlphaProperty:
                            mAlpha = static_cast<const Nif::NiAlphaProperty*>(prop);
                            break;
                        case Nif::RC_NiMaterialProperty:
                            mMaterial = static_cast<const Nif::NiMaterialProperty*>(prop);
                            break;
                        case Nif::RC_NiStencilProperty:
                            mStencil = static_cast<const Nif::NiStencilProperty*>(prop);
                            break;
                        default:
                            break;
                    }
                }
            }
        };

        std::uint8_t toByte(float v)
        {
            return static_cast<std::uint8_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
        }

        std::int8_t toSnorm(float v)
        {
            return static_cast<std::int8_t>(std::clamp(v, -1.f, 1.f) * 127.f);
        }

        // --- CHROMA64-style page splitting ---------------------------------
        //
        // TMEM holds 4 KB, so a texture drawn at native resolution is cut into
        // sPageSize^2 pages and every triangle is clipped along the page grid
        // until each piece samples a single page -- the same technique as
        // chroma64's tools/gen_rdp_mesh.py (split_uv / clip_axis), done here at
        // load time. Tiling UVs (common in Morrowind) land on the same pages.

        // A triangle wider than this many pages keeps the small whole texture.
        constexpr int sMaxPagesPerTriangle = 64;

        struct ClipVertex
        {
            float mPos[3];
            float mS, mT; // texel coordinates in the paged texture
            float mNormal[3];
            float mColor[4];
        };

        ClipVertex toClip(const Vertex& v, float width, float height)
        {
            ClipVertex c;
            for (int i = 0; i < 3; ++i)
            {
                c.mPos[i] = v.mPos[i];
                c.mNormal[i] = v.mNormal[i];
            }
            for (int i = 0; i < 4; ++i)
                c.mColor[i] = v.mColor[i];
            c.mS = v.mUV[0] * width;
            c.mT = v.mUV[1] * height;
            return c;
        }

        ClipVertex lerp(const ClipVertex& a, const ClipVertex& b, float t)
        {
            ClipVertex c;
            for (int i = 0; i < 3; ++i)
            {
                c.mPos[i] = a.mPos[i] + (b.mPos[i] - a.mPos[i]) * t;
                c.mNormal[i] = a.mNormal[i] + (b.mNormal[i] - a.mNormal[i]) * t;
            }
            for (int i = 0; i < 4; ++i)
                c.mColor[i] = a.mColor[i] + (b.mColor[i] - a.mColor[i]) * t;
            c.mS = a.mS + (b.mS - a.mS) * t;
            c.mT = a.mT + (b.mT - a.mT) * t;
            return c;
        }

        // Sutherland-Hodgman against one boundary: keep the side where
        // sign * (coordinate - bound) >= 0.
        void clipAxis(std::vector<ClipVertex>& poly, bool sAxis, float bound, float sign)
        {
            if (poly.size() < 3)
            {
                poly.clear();
                return;
            }
            std::vector<ClipVertex> out;
            out.reserve(poly.size() + 2);
            for (std::size_t i = 0; i < poly.size(); ++i)
            {
                const ClipVertex& a = poly[i];
                const ClipVertex& b = poly[(i + 1) % poly.size()];
                const float da = sign * ((sAxis ? a.mS : a.mT) - bound);
                const float db = sign * ((sAxis ? b.mS : b.mT) - bound);
                const bool aIn = da >= -1e-5f;
                const bool bIn = db >= -1e-5f;
                if (aIn && bIn)
                    out.push_back(b);
                else if (aIn != bIn)
                {
                    out.push_back(lerp(a, b, da / (da - db)));
                    if (bIn)
                        out.push_back(b);
                }
            }
            poly.swap(out);
        }

        Vertex fromClip(const ClipVertex& c, float s0, float t0)
        {
            Vertex v;
            for (int i = 0; i < 3; ++i)
            {
                v.mPos[i] = c.mPos[i];
                v.mNormal[i] = static_cast<std::int8_t>(std::clamp(std::lround(c.mNormal[i]), -127L, 127L));
            }
            v.mPad = 0;
            for (int i = 0; i < 4; ++i)
                v.mColor[i] = static_cast<std::uint8_t>(std::clamp(std::lround(c.mColor[i]), 0L, 255L));
            v.mUV[0] = std::clamp((c.mS - s0) / sPageSize, 0.f, 1.f);
            v.mUV[1] = std::clamp((c.mT - t0) / sPageSize, 0.f, 1.f);
            return v;
        }

        // Replaces one part by one part per page it touches (plus, if needed,
        // one using the small whole texture). Returns the triangle count.
        std::size_t splitIntoPages(MeshPart&& part, std::vector<MeshPart>& out)
        {
            const Texture& tex = *part.mTexture;
            const float width = static_cast<float>(tex.mPagedWidth);
            const float height = static_cast<float>(tex.mPagedHeight);
            std::map<int, MeshPart> byPage;
            std::size_t triangles = 0;

            auto partFor = [&](int page) -> MeshPart& {
                auto it = byPage.find(page);
                if (it != byPage.end() && it->second.mVertices.size() + 3 > 65535) // keep 16-bit indices valid
                {
                    out.push_back(std::move(it->second));
                    byPage.erase(it);
                    it = byPage.end();
                }
                if (it == byPage.end())
                {
                    it = byPage.try_emplace(page).first;
                    MeshPart& p = it->second;
                    p.mTexture = part.mTexture;
                    p.mPage = page;
                    std::copy(std::begin(part.mEmissive), std::end(part.mEmissive), p.mEmissive);
                    p.mAlphaTest = part.mAlphaTest;
                    p.mTwoSided = part.mTwoSided;
                }
                return it->second;
            };
            auto emit = [&](int page, const Vertex& a, const Vertex& b, const Vertex& c) {
                MeshPart& p = partFor(page);
                const auto base = static_cast<std::uint16_t>(p.mVertices.size());
                p.mVertices.insert(p.mVertices.end(), { a, b, c });
                p.mIndices.insert(p.mIndices.end(), { base, static_cast<std::uint16_t>(base + 1),
                                                        static_cast<std::uint16_t>(base + 2) });
                ++triangles;
            };
            auto wrap = [](int v, int n) { return ((v % n) + n) % n; };

            std::vector<ClipVertex> poly;
            for (std::size_t i = 0; i + 2 < part.mIndices.size(); i += 3)
            {
                const Vertex& v0 = part.mVertices[part.mIndices[i]];
                const Vertex& v1 = part.mVertices[part.mIndices[i + 1]];
                const Vertex& v2 = part.mVertices[part.mIndices[i + 2]];
                const ClipVertex c[3] = { toClip(v0, width, height), toClip(v1, width, height),
                    toClip(v2, width, height) };
                const float minS = std::min({ c[0].mS, c[1].mS, c[2].mS });
                const float maxS = std::max({ c[0].mS, c[1].mS, c[2].mS });
                const float minT = std::min({ c[0].mT, c[1].mT, c[2].mT });
                const float maxT = std::max({ c[0].mT, c[1].mT, c[2].mT });
                const int cx0 = static_cast<int>(std::floor(minS / sPageSize));
                const int cy0 = static_cast<int>(std::floor(minT / sPageSize));
                const int cx1 = std::max(cx0, static_cast<int>(std::floor((maxS - 1e-3f) / sPageSize)));
                const int cy1 = std::max(cy0, static_cast<int>(std::floor((maxT - 1e-3f) / sPageSize)));

                if ((cx1 - cx0 + 1) * (cy1 - cy0 + 1) > sMaxPagesPerTriangle)
                {
                    emit(-1, v0, v1, v2); // original UVs on the small, repeating texture
                    continue;
                }
                for (int cy = cy0; cy <= cy1; ++cy)
                {
                    for (int cx = cx0; cx <= cx1; ++cx)
                    {
                        const float s0 = static_cast<float>(cx * sPageSize);
                        const float t0 = static_cast<float>(cy * sPageSize);
                        poly.assign(std::begin(c), std::end(c));
                        clipAxis(poly, true, s0, 1.f);
                        clipAxis(poly, true, s0 + sPageSize, -1.f);
                        clipAxis(poly, false, t0, 1.f);
                        clipAxis(poly, false, t0 + sPageSize, -1.f);
                        if (poly.size() < 3)
                            continue;
                        const int page = wrap(cx, tex.mPagesX) + wrap(cy, tex.mPagesY) * tex.mPagesX;
                        const Vertex first = fromClip(poly[0], s0, t0);
                        for (std::size_t k = 1; k + 1 < poly.size(); ++k)
                            emit(page, first, fromClip(poly[k], s0, t0), fromClip(poly[k + 1], s0, t0));
                    }
                }
            }
            for (auto& [page, p] : byPage)
                out.push_back(std::move(p));
            return triangles;
        }

        class Builder
        {
        public:
            Builder(Model& model, TextureCache& textures)
                : mModel(model)
                , mTextures(textures)
            {
            }

            void walk(const Nif::NiAVObject* node, const osg::Matrixf& parent, PropertyState state)
            {
                if (node == nullptr || node->isHidden())
                    return;
                switch (node->mRecordType)
                {
                    // Collision-only geometry and particle systems are not drawn.
                    case Nif::RC_RootCollisionNode:
                    case Nif::RC_NiBSParticleNode:
                    case Nif::RC_NiParticles:
                        return;
                    default:
                        break;
                }

                const osg::Matrixf world = node->mTransform.toMatrix() * parent;
                state.apply(*node);

                switch (node->mRecordType)
                {
                    case Nif::RC_NiTriShape:
                    case Nif::RC_NiTriStrips:
                        addGeometry(static_cast<const Nif::NiTriBasedGeom&>(*node), world, state);
                        return;
                    default:
                        break;
                }

                const auto* asNode = dynamic_cast<const Nif::NiNode*>(node);
                if (asNode == nullptr)
                    return;

                // Switch and LOD nodes: draw only the child that would show first.
                if (const auto* sw = dynamic_cast<const Nif::NiSwitchNode*>(node))
                {
                    const std::size_t index = node->mRecordType == Nif::RC_NiLODNode ? 0 : sw->mInitialIndex;
                    if (index < asNode->mChildren.size() && !asNode->mChildren[index].empty())
                        walk(asNode->mChildren[index].getPtr(), world, state);
                    return;
                }

                for (const auto& child : asNode->mChildren)
                    if (!child.empty())
                        walk(child.getPtr(), world, state);
            }

        private:
            const Texture* baseTexture(const PropertyState& state)
            {
                const Nif::NiTexturingProperty* tex = state.mTexturing;
                if (tex == nullptr || tex->mTextures.empty())
                    return nullptr;
                const auto& base = tex->mTextures[Nif::NiTexturingProperty::BaseTexture];
                if (!base.mEnabled || base.mSourceTexture.empty())
                    return nullptr;
                const Nif::NiSourceTexture* source = base.mSourceTexture.getPtr();
                if (!source->mExternal || source->mFile.empty())
                    return nullptr; // embedded pixel data is rare in Morrowind
                return mTextures.get(source->mFile);
            }

            void addGeometry(const Nif::NiTriBasedGeom& geom, const osg::Matrixf& world, const PropertyState& state)
            {
                if (geom.mData.empty() || !geom.mSkin.empty())
                    return; // skinned meshes need a skeleton to pose them

                const Nif::NiGeometryData& data = *geom.mData.getPtr();
                const std::size_t count = data.mVertices.size();
                if (count == 0 || count > 65535)
                    return;

                MeshPart part;
                part.mTexture = baseTexture(state);
                if (state.mAlpha)
                    part.mAlphaTest = (state.mAlpha->mFlags
                                          & (Nif::NiAlphaProperty::Flag_Testing | Nif::NiAlphaProperty::Flag_Blending))
                        != 0;
                if (part.mTexture && part.mTexture->mHasAlpha)
                    part.mAlphaTest = true;
                if (state.mStencil)
                    part.mTwoSided = state.mStencil->mDrawMode == Nif::NiStencilProperty::DrawMode::Both;

                osg::Vec3f diffuse(1.f, 1.f, 1.f);
                float alpha = 1.f;
                if (state.mMaterial)
                {
                    diffuse = state.mMaterial->mDiffuse;
                    alpha = state.mMaterial->mAlpha;
                    for (int i = 0; i < 3; ++i)
                        part.mEmissive[i] = state.mMaterial->mEmissive[i];
                }

                const bool hasNormals = data.mNormals.size() == count;
                const bool hasColors = data.mColors.size() == count;
                const bool hasUVs = !data.mUVList.empty() && data.mUVList[0].size() == count;

                // Normals only need the rotation part; the matrices here have no shear.
                osg::Matrixf normalMatrix = world;
                normalMatrix.setTrans(0.f, 0.f, 0.f);

                part.mVertices.resize(count);
                for (std::size_t i = 0; i < count; ++i)
                {
                    Vertex& v = part.mVertices[i];
                    const osg::Vec3f p = data.mVertices[i] * world;
                    v.mPos[0] = p.x();
                    v.mPos[1] = p.y();
                    v.mPos[2] = p.z();

                    osg::Vec3f n = hasNormals ? data.mNormals[i] * normalMatrix : osg::Vec3f(0.f, 0.f, 1.f);
                    n.normalize();
                    v.mNormal[0] = toSnorm(n.x());
                    v.mNormal[1] = toSnorm(n.y());
                    v.mNormal[2] = toSnorm(n.z());
                    v.mPad = 0;

                    v.mUV[0] = hasUVs ? data.mUVList[0][i].x() : 0.f;
                    v.mUV[1] = hasUVs ? data.mUVList[0][i].y() : 0.f;

                    osg::Vec4f c = hasColors ? data.mColors[i] : osg::Vec4f(1.f, 1.f, 1.f, 1.f);
                    v.mColor[0] = toByte(c.r() * diffuse.x());
                    v.mColor[1] = toByte(c.g() * diffuse.y());
                    v.mColor[2] = toByte(c.b() * diffuse.z());
                    v.mColor[3] = toByte(c.a() * alpha);
                }

                if (geom.mRecordType == Nif::RC_NiTriShape)
                {
                    const auto& tris = static_cast<const Nif::NiTriShapeData&>(data).mTriangles;
                    part.mIndices.assign(tris.begin(), tris.end() - tris.size() % 3);
                }
                else
                {
                    // Unroll strips; every other triangle flips its winding.
                    for (const auto& strip : static_cast<const Nif::NiTriStripsData&>(data).mStrips)
                    {
                        for (std::size_t i = 2; i < strip.size(); ++i)
                        {
                            std::uint16_t a = strip[i - 2], b = strip[i - 1], c = strip[i];
                            if (a == b || b == c || a == c)
                                continue;
                            if (i & 1)
                                std::swap(a, b);
                            part.mIndices.insert(part.mIndices.end(), { a, b, c });
                        }
                    }
                }

                // Drop indices that point past the vertex array (corrupt data).
                part.mIndices.erase(std::remove_if(part.mIndices.begin(), part.mIndices.end(),
                                        [&](std::uint16_t i) { return i >= count; }),
                    part.mIndices.end());
                part.mIndices.resize(part.mIndices.size() - part.mIndices.size() % 3);
                if (part.mIndices.empty())
                    return;

                if (part.mTexture != nullptr && part.mTexture->paged())
                {
                    mModel.mTriangles += splitIntoPages(std::move(part), mModel.mParts);
                    return;
                }
                mModel.mTriangles += part.mIndices.size() / 3;
                mModel.mParts.push_back(std::move(part));
            }

            Model& mModel;
            TextureCache& mTextures;
        };
    }

    MeshPart::MeshPart(MeshPart&& other) noexcept
        : mVertices(std::move(other.mVertices))
        , mIndices(std::move(other.mIndices))
        , mList(other.mList)
        , mVertexArray(other.mVertexArray)
        , mIndexCount(other.mIndexCount)
        , mTexture(other.mTexture)
        , mPage(other.mPage)
        , mAlphaTest(other.mAlphaTest)
        , mTwoSided(other.mTwoSided)
    {
        std::copy(std::begin(other.mEmissive), std::end(other.mEmissive), mEmissive);
        std::copy(std::begin(other.mBuffers), std::end(other.mBuffers), mBuffers);
        other.mList = 0;
        other.mVertexArray = 0;
        other.mBuffers[0] = other.mBuffers[1] = 0;
    }

    MeshPart::~MeshPart()
    {
        if (mList != 0)
            glDeleteLists(mList, 1);
        if (mVertexArray != 0)
            glDeleteVertexArrays(1, &mVertexArray);
        if (mBuffers[0] != 0)
            glDeleteBuffersARB(2, mBuffers);
    }

    void MeshPart::upload(bool displayList)
    {
        // Temporary buffer objects and vertex array to record the draw from.
        GLuint buffers[2];
        glGenBuffersARB(2, buffers);
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, buffers[0]);
        glBufferDataARB(GL_ARRAY_BUFFER_ARB, mVertices.size() * sizeof(Vertex), mVertices.data(), GL_STATIC_DRAW_ARB);
        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, buffers[1]);
        glBufferDataARB(
            GL_ELEMENT_ARRAY_BUFFER_ARB, mIndices.size() * sizeof(std::uint16_t), mIndices.data(), GL_STATIC_DRAW_ARB);

        GLuint vertexArray;
        glGenVertexArrays(1, &vertexArray);
        glBindVertexArray(vertexArray);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, mPos)));
        glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, mUV)));
        glNormalPointer(GL_BYTE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, mNormal)));
        glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), reinterpret_cast<void*>(offsetof(Vertex, mColor)));

        if (displayList)
        {
            mList = glGenLists(1);
            glNewList(mList, GL_COMPILE);
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(mIndices.size()), GL_UNSIGNED_SHORT, nullptr);
            glEndList();
        }

        glBindVertexArray(0);
        glBindBufferARB(GL_ARRAY_BUFFER_ARB, 0);
        glBindBufferARB(GL_ELEMENT_ARRAY_BUFFER_ARB, 0);
        if (displayList)
        {
            glDeleteVertexArrays(1, &vertexArray);
            glDeleteBuffersARB(2, buffers);
        }
        else
        {
            mVertexArray = vertexArray;
            mBuffers[0] = buffers[0];
            mBuffers[1] = buffers[1];
            mIndexCount = static_cast<GLsizei>(mIndices.size());
        }

        // The display list or the buffer objects hold the only copy now.
        mVertices = {};
        mIndices = {};
    }

    const Model* ModelCache::get(std::string_view path)
    {
        std::string key = normalizePath(path);
        auto it = mModels.find(key);
        if (it != mModels.end())
            return it->second.get();

        std::unique_ptr<Model> model;
        try
        {
            model = load(key);
        }
        catch (const std::exception& e)
        {
            Log(Debug::Error) << "Failed to load mesh " << key << ": " << e.what();
        }
        const Model* result = model.get();
        mModels.emplace(std::move(key), std::move(model));
        return result;
    }

    std::unique_ptr<Model> ModelCache::load(const std::string& path)
    {
        const std::string fullPath = "meshes\\" + path;
        Files::IStreamPtr stream = mData.open(fullPath);
        if (!stream)
        {
            Log(Debug::Warning) << "Mesh not found: " << fullPath;
            return nullptr;
        }

        // The actual NIF parsing is OpenMW's own components/nif.
        const VFS::Path::Normalized nifPath(fullPath);
        Nif::NIFFile file(nifPath);
        {
            Nif::Reader reader(file, nullptr);
            reader.parse(std::move(stream));
        }

        auto model = std::make_unique<Model>();
        Builder builder(*model, mTextures);
        for (const Nif::Record* root : file.mRoots)
            if (const auto* node = dynamic_cast<const Nif::NiAVObject*>(root))
                builder.walk(node, osg::Matrixf::identity(), PropertyState());

        if (model->mParts.empty())
            return nullptr;

        osg::Vec3f lo(1e9f, 1e9f, 1e9f);
        osg::Vec3f hi(-1e9f, -1e9f, -1e9f);
        for (const MeshPart& part : model->mParts)
            for (const Vertex& v : part.mVertices)
                for (int i = 0; i < 3; ++i)
                {
                    lo[i] = std::min(lo[i], v.mPos[i]);
                    hi[i] = std::max(hi[i], v.mPos[i]);
                }
        model->mCenter = (lo + hi) * 0.5f;
        model->mRadius = (hi - lo).length() * 0.5f;

        float extent = 0.f;
        for (int i = 0; i < 3; ++i)
            extent = std::max({ extent, std::abs(lo[i]), std::abs(hi[i]) });
        while (extent / model->mVertexScale >= 1000.f)
            model->mVertexScale *= 2.f;
        const float inv = 1.f / model->mVertexScale;
        for (MeshPart& part : model->mParts)
        {
            for (Vertex& v : part.mVertices)
                for (int i = 0; i < 3; ++i)
                    v.mPos[i] *= inv;
            part.upload(sUseDisplayLists && (part.mTexture == nullptr || !part.mTexture->paged()));
        }
        Log(Debug::Info) << "Loaded " << fullPath << ": " << model->mParts.size() << " parts, " << model->mTriangles
                         << " triangles, radius " << model->mRadius << ", vertex scale " << model->mVertexScale;
        return model;
    }
}
