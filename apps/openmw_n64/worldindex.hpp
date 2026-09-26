#ifndef OPENMW_N64_WORLDINDEX_HPP
#define OPENMW_N64_WORLDINDEX_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <components/esm3/esmreader.hpp>
#include <components/esm3/loadcell.hpp>

namespace OMW64
{
    // Everything the viewer keeps from one pass over the content file.
    //
    // OpenMW's ESMStore loads every record into memory, which needs far more
    // than the N64's 8 MB. This keeps only what drawing a cell needs: the
    // interior cells (with the file position of their references, which
    // ESM::Cell::load already records) and a compact object-id -> model map.
    class WorldIndex
    {
    public:
        using Progress = std::function<void(float fraction)>;

        void scan(ESM::ESMReader& esm, const Progress& progress);

        const std::vector<ESM::Cell>& interiors() const { return mInteriors; }

        // Model path ("x\\y.nif", relative to meshes\) for an object id, or
        // empty if the object has no model or is not a supported type.
        std::string_view modelFor(std::string_view id) const;

        std::size_t objectCount() const { return mModels.size(); }

    private:
        struct ModelEntry
        {
            std::uint32_t mIdHash;
            std::uint32_t mPathOffset;
        };

        void addObject(std::string_view id, std::string_view model);

        std::vector<ESM::Cell> mInteriors;
        std::vector<ModelEntry> mModels; // sorted by mIdHash after scan()
        std::string mPathPool; // NUL-separated, de-duplicated model paths
        std::unordered_map<std::uint32_t, std::uint32_t> mPoolLookup; // path hash -> pool offset
    };
}

#endif
