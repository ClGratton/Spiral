#pragma once

#include <string>

struct cgltf_data;

namespace Engine
{
    // Independent, overflow-checked bounds authority for a parsed glTF whose buffers are already
    // attached. cgltf 1.15 computes offset + stride * (count - 1) + elementSize (and the
    // bufferView/sparse equivalents) in unchecked size_t arithmetic, so a hostile count or
    // byteStride can wrap the sum to a small value, pass cgltf_validate, and then crash or read
    // outside the buffer inside cgltf_validate's own index scan or the unpack calls. Every
    // consumer must call this after attaching buffers and before cgltf_validate or any
    // accessor read. It checks that
    //   - every bufferView lies inside its buffer, and a byteStride is 0 or within 4..252;
    //   - every accessor has a known type, a nonzero count, a stride no smaller than its element
    //     and fits its bufferView (all sums checked);
    //   - every sparse accessor is tightly packed (cgltf steps sparse values by the base stride),
    //     has 1..count entries of unsigned indices, and fits its index and value bufferViews.
    // On failure `error` begins with "glTF structure is invalid" and names the violated rule.
    bool ValidateGltfBufferBounds(const cgltf_data& data, std::string& error);
}
