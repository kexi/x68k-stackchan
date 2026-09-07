// SPDX-License-Identifier: MIT
#ifndef X68K_CORE_VIDEO_VISUAL_DAMAGE_H
#define X68K_CORE_VIDEO_VISUAL_DAMAGE_H

#include <cstdint>

namespace x68k
{
// 所有しないCore1専用通知先。width=0は全面。未登録なら追跡費用を払わない。
// VRAMの生ポインタをホストから更新/restoreする側はall()相当を明示する。
struct VisualDamage
{
    void* context = nullptr;
    void (*changed)(void*, std::int32_t, std::int32_t, std::int32_t, std::int32_t) = nullptr;

    void rect(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height) const
    {
        const bool connected = changed != nullptr;
        if (connected)
        {
            changed(context, x, y, width, height);
        }
    }
    void all() const
    {
        rect(0, 0, 0, 0);
    }
};
}  // namespace x68k
#endif
