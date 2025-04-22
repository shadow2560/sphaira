#include "yati/nx/ncm.hpp"
#include "defines.hpp"
#include <memory>

namespace sphaira::ncm {
namespace {

} // namespace

auto GetAppId(const NcmContentMetaKey& key) -> u64 {
    if (key.type == NcmContentMetaType_Patch) {
        return key.id ^ 0x800;
    } else if (key.type == NcmContentMetaType_AddOnContent) {
        return (key.id ^ 0x1000) & ~0xFFF;
    } else {
        return key.id;
    }
}

Result Delete(NcmContentStorage* cs, const NcmContentId *content_id) {
    bool has;
    R_TRY(ncmContentStorageHas(cs, std::addressof(has), content_id));
    if (has) {
        R_TRY(ncmContentStorageDelete(cs, content_id));
    }
    R_SUCCEED();
}

Result Register(NcmContentStorage* cs, const NcmContentId *content_id, const NcmPlaceHolderId *placeholder_id) {
    R_TRY(Delete(cs, content_id));
    return ncmContentStorageRegister(cs, content_id, placeholder_id);
}

} // namespace sphaira::ncm
