// nifstat: what each NIF mesh would cost after conversion, measured with
// OpenMW's own NIF reader. Prints one JSON object per mesh (JSON Lines).
//
//   nifstat Morrowind.bsa "Data Files"      # archives and/or loose folders
//
// For a folder, every .nif under it is read and named by its path relative
// to the folder ("meshes/x/y.nif"); archive entries are named as stored. The
// census (../census) merges the two, loose files overriding the archive.

#include <components/bsa/bsafile.hpp>
#include <components/files/constrainedfilestream.hpp>
#include <components/nif/controller.hpp>
#include <components/nif/data.hpp>
#include <components/nif/extra.hpp>
#include <components/nif/niffile.hpp>
#include <components/nif/node.hpp>
#include <components/nif/particle.hpp>
#include <components/nif/property.hpp>
#include <components/nif/texture.hpp>
#include <components/vfs/pathutil.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

namespace
{
    std::string normalized(std::string name)
    {
        VFS::Path::normalizeFilenameInPlace(name);
        return name;
    }

    struct Stats
    {
        long shapes = 0, verts = 0, tris = 0, uvVerts = 0, colorVerts = 0, skinnedShapes = 0;
        long collVerts = 0, collTris = 0;
        long particleVerts = 0;
        long rotKeys = 0, transKeys = 0, scaleKeys = 0, floatKeys = 0, morphVerts = 0, skinWeights = 0;
        std::set<std::string> textures;
    };

    long triCount(const Nif::NiGeometryData& data)
    {
        if (data.mRecordType == Nif::RC_NiTriShapeData)
            return static_cast<long>(static_cast<const Nif::NiTriShapeData&>(data).mTriangles.size() / 3);
        if (data.mRecordType == Nif::RC_NiTriStripsData)
        {
            long n = 0;
            for (const auto& strip : static_cast<const Nif::NiTriStripsData&>(data).mStrips)
                n += strip.size() >= 3 ? static_cast<long>(strip.size()) - 2 : 0;
            return n;
        }
        return 0;
    }

    template <class M>
    long keys(const M& ptr)
    {
        return ptr ? static_cast<long>(ptr->mKeys.size()) : 0;
    }

    // Geometry, split into what is drawn and what only collides.
    void walk(const Nif::NiAVObject* node, bool collision, Stats& s)
    {
        if (!node)
            return;
        if (node->mRecordType == Nif::RC_RootCollisionNode)
            collision = true;
        const bool hidden = node->isHidden();

        if (const auto* geom = dynamic_cast<const Nif::NiGeometry*>(node))
        {
            if (!geom->mData.empty())
            {
                const Nif::NiGeometryData& data = *geom->mData.getPtr();
                const long v = static_cast<long>(data.mVertices.size());
                if (geom->mRecordType == Nif::RC_NiTriShape || geom->mRecordType == Nif::RC_NiTriStrips)
                {
                    if (collision)
                    {
                        s.collVerts += v;
                        s.collTris += triCount(data);
                    }
                    else if (!hidden)
                    {
                        s.shapes++;
                        s.verts += v;
                        s.tris += triCount(data);
                        if (!data.mUVList.empty())
                            s.uvVerts += v;
                        if (!data.mColors.empty())
                            s.colorVerts += v;
                        if (!geom->mSkin.empty())
                            s.skinnedShapes++;
                    }
                }
                else
                    s.particleVerts += v;
            }
        }
        if (const auto* n = dynamic_cast<const Nif::NiNode*>(node))
            for (const auto& child : n->mChildren)
                if (!child.empty())
                    walk(child.getPtr(), collision, s);
    }

    void count(const Nif::NIFFile& file, Stats& s)
    {
        for (const auto& rec : file.mRecords)
        {
            if (!rec)
                continue;
            switch (rec->mRecordType)
            {
                case Nif::RC_NiKeyframeData:
                {
                    const auto& d = static_cast<const Nif::NiKeyframeData&>(*rec);
                    s.rotKeys += keys(d.mRotations) + keys(d.mXRotations) + keys(d.mYRotations) + keys(d.mZRotations);
                    s.transKeys += keys(d.mTranslations);
                    s.scaleKeys += keys(d.mScales);
                    break;
                }
                case Nif::RC_NiFloatData:
                    s.floatKeys += keys(static_cast<const Nif::NiFloatData&>(*rec).mKeyList);
                    break;
                case Nif::RC_NiPosData:
                    s.transKeys += keys(static_cast<const Nif::NiPosData&>(*rec).mKeyList);
                    break;
                case Nif::RC_NiMorphData:
                    for (const auto& morph : static_cast<const Nif::NiMorphData&>(*rec).mMorphs)
                        s.morphVerts += static_cast<long>(morph.mVertices.size());
                    break;
                case Nif::RC_NiSkinData:
                    for (const auto& bone : static_cast<const Nif::NiSkinData&>(*rec).mBones)
                        s.skinWeights += static_cast<long>(bone.mWeights.size());
                    break;
                case Nif::RC_NiSourceTexture:
                {
                    const auto& t = static_cast<const Nif::NiSourceTexture&>(*rec);
                    if (t.mExternal && !t.mFile.empty())
                        s.textures.insert(normalized(t.mFile));
                    break;
                }
                default:
                    break;
            }
        }
        for (const Nif::Record* root : file.mRoots)
            if (const auto* node = dynamic_cast<const Nif::NiAVObject*>(root))
                walk(node, false, s);
    }

    std::string json(const std::string& v)
    {
        std::string out = "\"";
        for (const char c : v)
        {
            if (c == '"' || c == '\\')
                out += '\\';
            if (static_cast<unsigned char>(c) < 0x20)
                continue;
            out += c;
        }
        return out + "\"";
    }

    void report(const std::string& name, const std::string& source, std::size_t bytes, Files::IStreamPtr stream)
    {
        std::ostringstream o;
        o << "{\"name\":" << json(name) << ",\"src\":" << json(source) << ",\"bytes\":" << bytes;
        try
        {
            const VFS::Path::Normalized path(name);
            Nif::NIFFile file(path);
            {
                Nif::Reader reader(file, nullptr);
                reader.parse(std::move(stream));
            }
            Stats s;
            count(file, s);
            o << ",\"ok\":true,\"shapes\":" << s.shapes << ",\"verts\":" << s.verts << ",\"tris\":" << s.tris
              << ",\"uvVerts\":" << s.uvVerts << ",\"colorVerts\":" << s.colorVerts
              << ",\"skinnedShapes\":" << s.skinnedShapes << ",\"collVerts\":" << s.collVerts
              << ",\"collTris\":" << s.collTris << ",\"particleVerts\":" << s.particleVerts
              << ",\"rotKeys\":" << s.rotKeys << ",\"transKeys\":" << s.transKeys << ",\"scaleKeys\":" << s.scaleKeys
              << ",\"floatKeys\":" << s.floatKeys << ",\"morphVerts\":" << s.morphVerts
              << ",\"skinWeights\":" << s.skinWeights << ",\"textures\":[";
            bool first = true;
            for (const auto& t : s.textures)
            {
                o << (first ? "" : ",") << json(t);
                first = false;
            }
            o << "]";
        }
        catch (const std::exception& e)
        {
            o << ",\"ok\":false,\"error\":" << json(e.what());
        }
        o << "}\n";
        std::fputs(o.str().c_str(), stdout);
    }

    bool isNif(const std::string& name)
    {
        return name.size() > 4 && normalized(name.substr(name.size() - 4)) == ".nif";
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: nifstat <archive.bsa | folder>...\n";
        return 1;
    }
    for (int i = 1; i < argc; ++i)
    {
        const std::filesystem::path arg(argv[i]);
        if (std::filesystem::is_directory(arg))
        {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(arg))
            {
                if (!entry.is_regular_file())
                    continue;
                const std::string rel = std::filesystem::relative(entry.path(), arg).generic_string();
                if (!isNif(rel))
                    continue;
                report(normalized(rel), "loose", entry.file_size(),
                    Files::openConstrainedFileStream(entry.path()));
            }
        }
        else
        {
            Bsa::BSAFile bsa;
            bsa.open(arg);
            for (const auto& f : bsa.getList())
            {
                const std::string name(f.name());
                if (isNif(name))
                    report(normalized(name), arg.filename().string(), f.mFileSize, bsa.getFile(&f));
            }
        }
    }
    return 0;
}
