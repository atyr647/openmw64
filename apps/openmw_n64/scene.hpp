#ifndef OPENMW_N64_SCENE_HPP
#define OPENMW_N64_SCENE_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <osg/Matrixf>
#include <osg/Vec3f>

namespace ESM
{
    class ESMReader;
    struct Cell;
}

namespace OMW64
{
    class ModelCache;
    class WorldIndex;
    struct Model;

    struct Instance
    {
        const Model* mModel = nullptr;
        osg::Matrixf mMatrix;
        osg::Vec3f mCenter; // world-space bounding sphere
        float mRadius = 0.f;
    };

    // One loaded interior cell: every reference that has a static mesh.
    struct CellScene
    {
        std::string mName;
        std::vector<Instance> mInstances;
        osg::Vec3f mStart;
        float mAmbient[3] = { 0.3f, 0.3f, 0.3f };
        float mSunlight[3] = { 0.7f, 0.7f, 0.7f };
        float mFog[3] = { 0.f, 0.f, 0.f };

        std::size_t mReferences = 0; // FRMR records read
        std::size_t mMissingModels = 0; // no model, unsupported type, or failed to load
        std::size_t mSkippedForMemory = 0;
        std::size_t mTriangles = 0;

        using Progress = std::function<void(float fraction, const char* what)>;

        void load(ESM::ESMReader& esm, const ESM::Cell& cell, const WorldIndex& index, ModelCache& models,
            const Progress& progress);
    };

    // Draws a CellScene with libdragon's OpenGL 1.1 implementation.
    class Renderer
    {
    public:
        void init();

        struct Camera
        {
            osg::Vec3f mPosition;
            float mYaw = 0.f; // radians, 0 = looking along +Y (north)
            float mPitch = 0.f;
        };

        struct Stats
        {
            std::size_t mDrawn = 0;
            std::size_t mCulled = 0;
            std::size_t mTriangles = 0;
        };

        Stats draw(const CellScene& scene, const Camera& camera);

        float mFovY = 60.f;
        float mNear = 8.f;
        float mFar = 3000.f; // Morrowind units; a cell is 8192 wide
    };
}

#endif
