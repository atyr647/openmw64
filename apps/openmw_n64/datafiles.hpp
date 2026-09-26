#ifndef OPENMW_N64_DATAFILES_HPP
#define OPENMW_N64_DATAFILES_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <components/bsa/bsafile.hpp>
#include <components/files/istreamptr.hpp>

namespace OMW64
{
    // A tiny stand-in for OpenMW's VFS::Manager: loose files under the data
    // directory win over files packed in BSA archives, like in the real game.
    class DataFiles
    {
    public:
        // Looks for a "Data Files" directory on the flashcart SD card, then in
        // the ROM filesystem. Returns false if none has a content file.
        bool init();

        const std::string& root() const { return mRoot; }
        const std::string& contentFile() const { return mContentFile; }
        const std::vector<std::string>& archives() const { return mArchiveNames; }

        // Opens a game file such as "meshes\\f\\furn_chair.nif". Returns
        // nullptr when it exists neither as a loose file nor in an archive.
        Files::IStreamPtr open(std::string_view path) const;
        bool exists(std::string_view path) const;

    private:
        struct Entry
        {
            std::uint32_t mHash;
            std::uint16_t mArchive;
            std::uint32_t mFile;
        };

        bool tryRoot(const std::string& root);
        const Entry* find(std::string_view path) const;

        std::string mRoot;
        std::string mContentFile;
        std::vector<std::string> mArchiveNames;
        std::vector<std::unique_ptr<Bsa::BSAFile>> mArchives;
        std::vector<Entry> mIndex; // sorted by mHash
    };

    // Lower-case, backslash-separated form used for all lookups.
    std::string normalizePath(std::string_view path);
    std::uint32_t hashPath(std::string_view normalized);
}

#endif
