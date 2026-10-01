#include "policy_config.hpp"

namespace qs::windows::services::pipewire {

// 870af99c-171d-4f9e-af0d-e63df40c2bc9 ("CPolicyConfigClient")
const CLSID CLSID_PolicyConfigClient = {
    0x870af99c,
    0x171d,
    0x4f9e,
    {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}
};

// f8679f50-850a-41cf-9c72-430f290290c8 (IPolicyConfig)
const IID IID_IPolicyConfig = {
    0xf8679f50,
    0x850a,
    0x41cf,
    {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}
};

} // namespace qs::windows::services::pipewire
