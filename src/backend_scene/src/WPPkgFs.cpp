#include "WPPkgFs.hpp"
#include "Utils/Logging.h"
#include "Fs/LimitedBinaryStream.h"
#include "Fs/CBinaryStream.h"
#include <vector>

using namespace wallpaper;
using namespace wallpaper::fs;

namespace
{
bool ReadSizedString(IBinaryStream& f, std::string& result) {
    if (f.Size() - f.Tell() < 4) return false;
    const i32 len = f.ReadInt32();
    if (len < 0 || len > f.Size() - f.Tell()) return false;
    result.resize(static_cast<usize>(len));
    return len == 0 || f.Read(result.data(), result.size()) == result.size();
}
} // namespace

std::unique_ptr<WPPkgFs> WPPkgFs::CreatePkgFs(std::string_view pkgpath) {
    auto ppkg = fs::CreateCBinaryStream(pkgpath);
    if (! ppkg) return nullptr;

    auto&       pkg = *ppkg;
    std::string ver;
    if (! ReadSizedString(pkg, ver)) return nullptr;
    LOG_INFO("pkg version: %s", ver.data());

    std::vector<PkgFile> pkgfiles;
    if (pkg.Size() - pkg.Tell() < 4) return nullptr;
    i32 entryCount = pkg.ReadInt32();
    if (entryCount < 0 || entryCount > (pkg.Size() - pkg.Tell()) / 12) return nullptr;
    for (i32 i = 0; i < entryCount; i++) {
        std::string path;
        if (! ReadSizedString(pkg, path) || pkg.Size() - pkg.Tell() < 8) return nullptr;
        path       = "/" + path;
        idx offset = pkg.ReadInt32();
        idx length = pkg.ReadInt32();
        pkgfiles.push_back({ path, offset, length });
    }
    auto pkgfs       = std::unique_ptr<WPPkgFs>(new WPPkgFs());
    pkgfs->m_pkgPath = pkgpath;
    idx headerSize   = pkg.Tell();
    for (auto& el : pkgfiles) {
        if (el.offset < 0 || el.length < 0 || el.offset > pkg.Size() - headerSize ||
            el.length > pkg.Size() - headerSize - el.offset)
            return nullptr;
        el.offset += headerSize;
        pkgfs->m_files.insert({ el.path, el });
    }
    return pkgfs;
}

bool WPPkgFs::Contains(std::string_view path) const { return m_files.count(std::string(path)) > 0; }

std::shared_ptr<IBinaryStream> WPPkgFs::Open(std::string_view path) {
    auto pkg = fs::CreateCBinaryStream(m_pkgPath);
    if (! pkg) return nullptr;
    if (Contains(path)) {
        const auto& file = m_files.at(std::string(path));
        return std::make_shared<LimitedBinaryStream>(pkg, file.offset, file.length);
    }
    return nullptr;
}

std::shared_ptr<IBinaryStreamW> WPPkgFs::OpenW(std::string_view) { return nullptr; }
