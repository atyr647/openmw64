#include "scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>

#include <GL/gl.h>
#include <n64sys.h>

#include <osg/Quat>

#include <components/debug/debuglog.hpp>
#include <components/esm3/cellref.hpp>
#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadcell.hpp>

#include "meshloader.hpp"
#include "texture.hpp"
#include "worldindex.hpp"

namespace OMW64
{
    namespace
    {
        // Keep this much heap free for the frame: GL command buffers, the
        // parser's temporary records and libdragon's own allocations.
        constexpr int sHeapReserve = 768 * 1024;

        int freeHeap()
        {
            heap_stats_t stats;
            sys_get_heap_stats(&stats);
            return stats.total - stats.used;
        }

        // Same as Misc::Convert::makeOsgQuat, which cannot be included here
        // because its header also pulls in Bullet.
        osg::Quat makeOsgQuat(const float (&rotation)[3])
        {
            return osg::Quat(rotation[2], osg::Vec3f(0, 0, -1)) * osg::Quat(rotation[1], osg::Vec3f(0, -1, 0))
                * osg::Quat(rotation[0], osg::Vec3f(-1, 0, 0));
        }

        void colorToFloats(std::uint32_t color, float (&out)[3])
        {
            // ESM colors are stored as 0x00BBGGRR.
            out[0] = static_cast<float>(color & 0xff) / 255.f;
            out[1] = static_cast<float>((color >> 8) & 0xff) / 255.f;
            out[2] = static_cast<float>((color >> 16) & 0xff) / 255.f;
        }
    }

    void CellScene::load(ESM::ESMReader& esm, const ESM::Cell& cell, const WorldIndex& index, ModelCache& models,
        const Progress& progress)
    {
        mName = cell.mName;
        mInstances.clear();
        mReferences = mMissingModels = mSkippedForMemory = mTriangles = 0;
        if (cell.hasAmbient())
        {
            colorToFloats(cell.mAmbi.mAmbient, mAmbient);
            colorToFloats(cell.mAmbi.mSunlight, mSunlight);
            colorToFloats(cell.mAmbi.mFog, mFog);
        }

        struct Placement
        {
            std::string_view mModel;
            osg::Matrixf mMatrix;
        };
        std::vector<Placement> placements;

        // Read the references with OpenMW's own ESM::Cell/CellRef code.
        if (progress)
            progress(0.f, "Reading references");
        cell.restore(esm, 0);
        ESM::CellRef ref;
        bool deleted = false;
        while (ESM::Cell::getNextRef(esm, ref, deleted))
        {
            ++mReferences;
            if (deleted)
                continue;
            const std::string_view model = index.modelFor(ref.mRefID.getRefIdString());
            if (model.empty())
            {
                ++mMissingModels;
                continue;
            }
            const osg::Vec3f pos(ref.mPos.pos[0], ref.mPos.pos[1], ref.mPos.pos[2]);
            placements.push_back({ model,
                osg::Matrixf::scale(ref.mScale, ref.mScale, ref.mScale)
                    * osg::Matrixf::rotate(makeOsgQuat(ref.mPos.rot)) * osg::Matrixf::translate(pos) });
        }

        // Load each distinct mesh once, stopping before RDRAM runs out.
        struct Loaded
        {
            const Model* mModel = nullptr;
            bool mSkipped = false;
        };
        std::map<std::string_view, Loaded> loaded;
        for (const Placement& p : placements)
            loaded.emplace(p.mModel, Loaded());
        std::size_t done = 0;
        for (auto& [path, entry] : loaded)
        {
            if (progress)
                progress(static_cast<float>(done++) / static_cast<float>(loaded.size()), "Loading meshes");
            if (freeHeap() < sHeapReserve)
            {
                Log(Debug::Warning) << "Out of memory, skipping " << path;
                entry.mSkipped = true;
                continue;
            }
            entry.mModel = models.get(path);
        }

        osg::Vec3f sum;
        float lowest = 1e9f;
        for (const Placement& p : placements)
        {
            const Loaded& entry = loaded[p.mModel];
            const Model* model = entry.mModel;
            if (model == nullptr)
            {
                ++(entry.mSkipped ? mSkippedForMemory : mMissingModels);
                continue;
            }
            Instance inst;
            inst.mModel = model;
            inst.mMatrix = p.mMatrix;
            inst.mCenter = model->mCenter * p.mMatrix;
            const float scale = static_cast<float>(p.mMatrix.getScale().x());
            inst.mRadius = model->mRadius * scale;
            const osg::Vec3f trans(p.mMatrix(3, 0), p.mMatrix(3, 1), p.mMatrix(3, 2));
            sum += trans;
            lowest = std::min(lowest, trans.z());
            mTriangles += model->mTriangles;
            mInstances.push_back(inst);
        }
        if (!mInstances.empty())
        {
            mStart = sum / static_cast<float>(mInstances.size());
            mStart.z() = lowest + 128.f; // roughly eye height above the floor
        }
        if (progress)
            progress(1.f, "Done");
    }

    void Renderer::init()
    {
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
        glFrontFace(GL_CCW);
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        glEnable(GL_NORMALIZE);
        glEnable(GL_COLOR_MATERIAL);
        glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);
        glAlphaFunc(GL_GREATER, 0.5f);
        glShadeModel(GL_SMOOTH);
    }

    Renderer::Stats Renderer::draw(const CellScene& scene, const Camera& camera)
    {
        Stats stats;
        const float aspect = 320.f / 240.f;
        // Eye-space positions and fog distances also go through the RSP's
        // 16-bit fixed point (+-1024), so draw in render units (see Model).
        // Scaling positions rather than the modelview keeps normals unit length.
        constexpr float worldScale = 1.f / sUnitsPerRenderUnit;

        glClearColor(scene.mFog[0], scene.mFog[1], scene.mFog[2], 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(osg::Matrixf::perspective(mFovY, aspect, mNear * worldScale, mFar * worldScale).ptr());

        // Morrowind is Z-up; build the view with OSG's lookAt like OpenMW does.
        const osg::Vec3f forward(std::sin(camera.mYaw) * std::cos(camera.mPitch),
            std::cos(camera.mYaw) * std::cos(camera.mPitch), std::sin(camera.mPitch));
        const osg::Matrixf view
            = osg::Matrixf::lookAt(camera.mPosition, camera.mPosition + forward, osg::Vec3f(0.f, 0.f, 1.f));
        const osg::Matrixf viewUnits = osg::Matrixf::lookAt(
            camera.mPosition * worldScale, (camera.mPosition + forward) * worldScale, osg::Vec3f(0.f, 0.f, 1.f));
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(viewUnits.ptr());

        // Interiors are lit by the cell's AMBI ambient + "sunlight" colors.
        const GLfloat ambient[4] = { scene.mAmbient[0], scene.mAmbient[1], scene.mAmbient[2], 1.f };
        const GLfloat sun[4] = { scene.mSunlight[0], scene.mSunlight[1], scene.mSunlight[2], 1.f };
        const GLfloat sunDir[4] = { -0.3f, 0.4f, 0.87f, 0.f };
        const GLfloat black[4] = { 0.f, 0.f, 0.f, 1.f };
        glLightModelfv(GL_LIGHT_MODEL_AMBIENT, ambient);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, sun);
        glLightfv(GL_LIGHT0, GL_AMBIENT, black);
        glLightfv(GL_LIGHT0, GL_POSITION, sunDir);

        // No GL_FOG: in libdragon's preview branch, enabling fog on the RSP
        // pipeline corrupts texture coordinates (textures collapse to a few
        // texels; its gldemo cube shows it too). The cell's fog color is
        // still used as the clear color above.

        // Cull against the view frustum in view space.
        const float tanY = std::tan(osg::DegreesToRadians(mFovY) * 0.5f);
        const float tanX = tanY * aspect;
        const float secY = std::sqrt(1.f + tanY * tanY);
        const float secX = std::sqrt(1.f + tanX * tanX);

        const Texture* bound = nullptr;
        bool texturing = false;
        bool alphaTest = false;
        bool culling = true;
        GLfloat emission[4] = { 0.f, 0.f, 0.f, 1.f };
        glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, emission);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);

        for (const Instance& inst : scene.mInstances)
        {
            const osg::Vec3f c = inst.mCenter * view;
            const float depth = -c.z();
            const float r = inst.mRadius;
            if (depth + r < mNear || depth - r > mFar || std::abs(c.x()) > depth * tanX + r * secX
                || std::abs(c.y()) > depth * tanY + r * secY)
            {
                ++stats.mCulled;
                continue;
            }
            ++stats.mDrawn;
            stats.mTriangles += inst.mModel->mTriangles;

            glPushMatrix();
            osg::Matrixf model = inst.mMatrix;
            model.setTrans(model.getTrans() * worldScale);
            glMultMatrixf(model.ptr());
            const float extra = inst.mModel->mVertexScale * worldScale;
            if (extra != 1.f)
                glScalef(extra, extra, extra);
            for (const MeshPart& part : inst.mModel->mParts)
            {
                if (part.mTexture != bound || (part.mTexture != nullptr) != texturing)
                {
                    texturing = part.mTexture != nullptr;
                    if (texturing)
                    {
                        glEnable(GL_TEXTURE_2D);
                        glBindTexture(GL_TEXTURE_2D, part.mTexture->mName);
                    }
                    else
                        glDisable(GL_TEXTURE_2D);
                    bound = part.mTexture;
                }
                if (part.mAlphaTest != alphaTest)
                {
                    alphaTest = part.mAlphaTest;
                    alphaTest ? glEnable(GL_ALPHA_TEST) : glDisable(GL_ALPHA_TEST);
                }
                if (part.mTwoSided == culling)
                {
                    culling = !part.mTwoSided;
                    culling ? glEnable(GL_CULL_FACE) : glDisable(GL_CULL_FACE);
                }
                if (!std::equal(std::begin(part.mEmissive), std::end(part.mEmissive), emission))
                {
                    std::copy(std::begin(part.mEmissive), std::end(part.mEmissive), emission);
                    glMaterialfv(GL_FRONT_AND_BACK, GL_EMISSION, emission);
                }

                glCallList(part.mList);
            }
            glPopMatrix();
        }
        return stats;
    }
}
