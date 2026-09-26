#include "datafiles.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

#include <dir.h>

#include <components/debug/debuglog.hpp>

namespace OMW64
{
    namespace
    {
        // Flashcart SD card first (real Morrowind data), then the ROM
        // filesystem (test data built into the ROM).
        const char* const sRoots[] = {
            "sd:/Data Files/",
            "sd:/Morrowind/Data Files/",
            "sd:/morrowind/",
            "rom:/",
        };

        const char* const sPreferredContent[] = { "Morrowind.esm", "morrowind.esm" };

        bool hasSuffix(std::string_view name, std::string_view suffix)
        {
            if (name.size() < suffix.size())
                return false;
            for (std::size_t i = 0; i < suffix.size(); ++i)
                if (std::tolower(static_cast<unsigned char>(name[name.size() - suffix.size() + i])) != suffix[i])
                    return false;
            return true;
        }

        std::vector<std::string> listDir(const std::string& dir)
        {
            std::vector<std::string> names;
            dir_t entry;
            // libdragon wants the directory without the trailing slash, except for the root.
            std::string path = dir;
            if (path.size() > 1 && path.back() == '/' && path[path.size() - 2] != ':')
                path.pop_back();
            int err = dir_findfirst(path.c_str(), &entry);
            while (err == 0)
            {
                if (entry.d_type == DT_REG)
                    names.emplace_back(entry.d_name);
                err = dir_findnext(path.c_str(), &entry);
            }
            std::sort(names.begin(), names.end());
            return names;
        }

        bool fileExists(const std::string& path)
        {
            FILE* f = std::fopen(path.c_str(), "rb");
            if (f == nullptr)
                return false;
            std::fclose(f);
            return true;
        }
    }

    std::string normalizePath(std::string_view path)
    {
        std::string result(path);
        for (char& c : result)
        {
            if (c == '/')
                c = '\\';
            else
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return result;
    }

    std::uint32_t hashPath(std::string_view normalized)
    {
        // FNV-1a
        std::uint32_t h = 2166136261u;
        for (char c : normalized)
        {
            h ^= static_cast<unsigned char>(c);
            h *= 16777619u;
        }
        return h;
    }

    bool DataFiles::tryRoot(const std::string& root)
    {
        const std::vector<std::string> names = listDir(root);
        std::string content;
        for (const char* preferred : sPreferredContent)
            if (std::find(names.begin(), names.end(), preferred) != names.end())
                content = preferred;
        if (content.empty())
            for (const std::string& name : names)
                if (hasSuffix(name, ".esm") || hasSuffix(name, ".omwgame"))
                {
                    content = name;
                    break;
                }
        if (content.empty())
        {
            // Some filesystems cannot list directories; probe the usual name.
            if (!fileExists(root + "Morrowind.esm"))
                return false;
            content = "Morrowind.esm";
        }

        mRoot = root;
        mContentFile = content;

        std::vector<std::string> bsas;
        for (const std::string& name : names)
            if (hasSuffix(name, ".bsa"))
                bsas.push_back(name);
        // Morrowind's own archives in load order, then anything else.
        const char* const order[] = { "morrowind.bsa", "tribunal.bsa", "bloodmoon.bsa" };
        std::stable_sort(bsas.begin(), bsas.end(), [&](const std::string& a, const std::string& b) {
            auto rank = [&](const std::string& n) {
                const std::string l = normalizePath(n);
                for (std::size_t i = 0; i < std::size(order); ++i)
                    if (l == order[i])
                        return static_cast<int>(i);
                return static_cast<int>(std::size(order));
            };
            return rank(a) < rank(b);
        });

        for (const std::string& name : bsas)
        {
            auto archive = std::make_unique<Bsa::BSAFile>();
            try
            {
                archive->open(root + name);
            }
            catch (const std::exception& e)
            {
                Log(Debug::Error) << "Failed to open " << name << ": " << e.what();
                continue;
            }
            const auto archiveIndex = static_cast<std::uint16_t>(mArchives.size());
            const Bsa::BSAFile::FileList& files = archive->getList();
            mIndex.reserve(mIndex.size() + files.size());
            for (std::size_t i = 0; i < files.size(); ++i)
                mIndex.push_back(
                    { hashPath(normalizePath(files[i].name())), archiveIndex, static_cast<std::uint32_t>(i) });
            mArchives.push_back(std::move(archive));
            mArchiveNames.push_back(name);
        }

        // Later archives override earlier ones: keep the last entry per hash.
        std::stable_sort(
            mIndex.begin(), mIndex.end(), [](const Entry& a, const Entry& b) { return a.mHash < b.mHash; });
        return true;
    }

    bool DataFiles::init()
    {
        for (const char* root : sRoots)
            if (tryRoot(root))
                return true;
        return false;
    }

    const DataFiles::Entry* DataFiles::find(std::string_view path) const
    {
        const std::string normalized = normalizePath(path);
        const std::uint32_t hash = hashPath(normalized);
        auto it = std::upper_bound(
            mIndex.begin(), mIndex.end(), hash, [](std::uint32_t h, const Entry& e) { return h < e.mHash; });
        // Walk back over entries with this hash, newest archive first.
        while (it != mIndex.begin())
        {
            --it;
            if (it->mHash != hash)
                break;
            const auto& file = mArchives[it->mArchive]->getList()[it->mFile];
            if (normalizePath(file.name()) == normalized)
                return &*it;
        }
        return nullptr;
    }

    Files::IStreamPtr DataFiles::open(std::string_view path) const
    {
        std::string loose = mRoot;
        for (char c : path)
            loose += (c == '\\') ? '/' : c;
        auto stream = std::make_unique<std::ifstream>(loose, std::ios::binary);
        if (stream->is_open())
            return stream;

        if (const Entry* entry = find(path))
            return mArchives[entry->mArchive]->getFile(&mArchives[entry->mArchive]->getList()[entry->mFile]);
        return nullptr;
    }

    bool DataFiles::exists(std::string_view path) const
    {
        if (find(path) != nullptr)
            return true;
        std::string loose = mRoot;
        for (char c : path)
            loose += (c == '\\') ? '/' : c;
        return fileExists(loose);
    }
}
