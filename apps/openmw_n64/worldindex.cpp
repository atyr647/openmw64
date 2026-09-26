#include "worldindex.hpp"

#include <algorithm>
#include <unordered_map>

#include <components/esm/defs.hpp>

#include "datafiles.hpp"

namespace OMW64
{
    namespace
    {
        // Object types whose MODL is a static, unskinned mesh. Creatures and
        // NPCs are skinned and animated, which this viewer does not do yet.
        bool isPlaceableObject(std::uint32_t type)
        {
            switch (type)
            {
                case ESM::REC_ACTI:
                case ESM::REC_ALCH:
                case ESM::REC_APPA:
                case ESM::REC_ARMO:
                case ESM::REC_BOOK:
                case ESM::REC_CLOT:
                case ESM::REC_CONT:
                case ESM::REC_DOOR:
                case ESM::REC_INGR:
                case ESM::REC_LIGH:
                case ESM::REC_LOCK:
                case ESM::REC_MISC:
                case ESM::REC_PROB:
                case ESM::REC_REPA:
                case ESM::REC_STAT:
                case ESM::REC_WEAP:
                    return true;
                default:
                    return false;
            }
        }
    }

    void WorldIndex::addObject(std::string_view id, std::string_view model)
    {
        if (id.empty() || model.empty())
            return;
        // Model paths repeat a lot (many objects share one mesh), so each
        // distinct path is stored once in the pool.
        const std::string path = normalizePath(model);
        const std::uint32_t pathHash = hashPath(path);
        auto [it, inserted] = mPoolLookup.try_emplace(pathHash, static_cast<std::uint32_t>(mPathPool.size()));
        if (inserted)
        {
            mPathPool += path;
            mPathPool += '\0';
        }
        mModels.push_back({ hashPath(normalizePath(id)), it->second });
    }

    void WorldIndex::scan(ESM::ESMReader& esm, const Progress& progress)
    {
        mInteriors.clear();
        mModels.clear();
        mPathPool.clear();
        mPoolLookup.clear();

        const std::size_t fileSize = std::max<std::size_t>(esm.getFileSize(), 1);
        std::size_t nextReport = 0;

        while (esm.hasMoreRecs())
        {
            const std::size_t offset = esm.getFileOffset();
            if (progress && offset >= nextReport)
            {
                progress(static_cast<float>(offset) / static_cast<float>(fileSize));
                nextReport = offset + fileSize / 64;
            }

            const ESM::NAME name = esm.getRecName();
            esm.getRecHeader();
            const std::uint32_t type = name.toInt();

            if (type == ESM::REC_CELL)
            {
                ESM::Cell cell;
                bool isDeleted = false;
                // Reads NAME/DATA/AMBI and remembers where the references
                // start, then skips them -- exactly what OpenMW itself does.
                cell.load(esm, isDeleted, true);
                if (!isDeleted && !cell.isExterior())
                    mInteriors.push_back(std::move(cell));
                continue;
            }

            if (!isPlaceableObject(type))
            {
                esm.skipRecord();
                continue;
            }

            std::string id;
            std::string model;
            while (esm.hasMoreSubs())
            {
                esm.getSubName();
                switch (esm.retSubName().toInt())
                {
                    case ESM::SREC_NAME:
                        id = esm.getHString();
                        break;
                    case ESM::fourCC("MODL"):
                        model = esm.getHString();
                        break;
                    default:
                        esm.skipHSub();
                        break;
                }
            }
            addObject(id, model);
        }

        std::sort(mInteriors.begin(), mInteriors.end(),
            [](const ESM::Cell& a, const ESM::Cell& b) { return a.mName < b.mName; });
        // Stable, so that for duplicate ids the last record read wins below.
        std::stable_sort(mModels.begin(), mModels.end(),
            [](const ModelEntry& a, const ModelEntry& b) { return a.mIdHash < b.mIdHash; });
        mPathPool.shrink_to_fit();
        mPoolLookup = {}; // only needed while scanning
        if (progress)
            progress(1.f);
    }

    std::string_view WorldIndex::modelFor(std::string_view id) const
    {
        const std::uint32_t hash = hashPath(normalizePath(id));
        auto it = std::upper_bound(
            mModels.begin(), mModels.end(), hash, [](std::uint32_t h, const ModelEntry& e) { return h < e.mIdHash; });
        if (it == mModels.begin() || (it - 1)->mIdHash != hash)
            return {};
        return std::string_view(mPathPool.c_str() + (it - 1)->mPathOffset);
    }
}
