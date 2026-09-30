// Host stand-ins for the parts of the Embody Mode code that need the robot (tests/sim):
// the asset store reads files from $SIM_ASSETS, JPEG decoding is not simulated.
#include <apps/app_embody_mode/asset_store.h>
#include <hal/utils/jpeg_to_image/jpeg_decoder.h>
#include <cstdlib>

namespace embody {

bool AssetStore::validName(const std::string& name)
{
    return !name.empty() && name.find("..") == std::string::npos && name.front() != '/';
}

std::string AssetStore::path(const std::string& name) const
{
    const char* dir = std::getenv("SIM_ASSETS");
    return validName(name) ? std::string(dir ? dir : ".") + "/" + name : std::string();
}

}  // namespace embody

namespace jpeg_dec {
std::shared_ptr<LvglAllocatedImage> decode_to_lvgl(const uint8_t*, size_t) { return nullptr; }
}  // namespace jpeg_dec
