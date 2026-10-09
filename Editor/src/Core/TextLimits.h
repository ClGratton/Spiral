#pragma once

#include <cstddef>
#include <string_view>

namespace SpiralEditor
{
    // The longest prefix of `text` that is at most `maximumBytes` long and does not end inside a UTF-8
    // sequence, so a truncated label never carries a dangling lead or continuation byte. Text that is not
    // valid UTF-8 still yields a prefix within the bound, possibly shorter than the bound.
    inline std::string_view TruncateUtf8(std::string_view text, size_t maximumBytes)
    {
        if (text.size() <= maximumBytes)
            return text;
        size_t cut = maximumBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
            --cut;
        return text.substr(0, cut);
    }
}
