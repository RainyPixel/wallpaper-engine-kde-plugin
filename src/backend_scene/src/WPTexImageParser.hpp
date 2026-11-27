#pragma once
#include "Interface/IImageParser.h"
#include "Fs/VFS.h"

namespace wallpaper
{

class WPTexImageParser : public IImageParser {
public:
    WPTexImageParser(fs::VFS* vfs): m_vfs(vfs) {}
    virtual ~WPTexImageParser() = default;

    std::shared_ptr<Image> Parse(const std::string&) override;
    ImageHeader            ParseHeader(const std::string&) override;
    void                   AddGeneratedImage(std::shared_ptr<Image>);

private:
    fs::VFS*                                                m_vfs;
    std::unordered_map<std::string, std::shared_ptr<Image>> m_generated_images;
};
} // namespace wallpaper
