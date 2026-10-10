#include "Engine/Assets/GltfBounds.h"

#include "Engine/Core/Base.h"

#include "cgltf.h"

#include <limits>

namespace Engine
{
    namespace
    {
        bool CheckedMultiply(u64 left, u64 right, u64& product)
        {
            if (left != 0 && right > std::numeric_limits<u64>::max() / left)
                return false;
            product = left * right;
            return true;
        }

        bool CheckedAdd(u64 left, u64 right, u64& sum)
        {
            if (right > std::numeric_limits<u64>::max() - left)
                return false;
            sum = left + right;
            return true;
        }

        // The number of bytes a run of `count` elements starting `offset` bytes into a view
        // touches: offset + stride * (count - 1) + elementSize. False on overflow or count 0.
        bool RequiredRunBytes(u64 offset, u64 stride, u64 count, u64 elementSize, u64& required)
        {
            if (count == 0)
                return false;
            u64 span = 0;
            return CheckedMultiply(stride, count - 1, span) && CheckedAdd(offset, span, span)
                && CheckedAdd(span, elementSize, required);
        }

        bool Reject(std::string& error, const char* reason)
        {
            error = std::string("glTF structure is invalid: ") + reason;
            return false;
        }
    }

    bool ValidateGltfBufferBounds(const cgltf_data& data, std::string& error)
    {
        // A bufferView byteStride is specified as 4..252 for vertex data.
        constexpr u64 kMaximumViewStride = 252;
        for (cgltf_size index = 0; index < data.buffer_views_count; ++index)
        {
            const cgltf_buffer_view& view = data.buffer_views[index];
            u64 end = 0;
            if (!view.buffer || !CheckedAdd(view.offset, view.size, end) || end > view.buffer->size)
                return Reject(error, "a bufferView exceeds its buffer");
            if (view.stride != 0 && (view.stride < 4 || view.stride > kMaximumViewStride))
                return Reject(error, "a bufferView byteStride is outside 4..252");
        }
        for (cgltf_size index = 0; index < data.accessors_count; ++index)
        {
            const cgltf_accessor& accessor = data.accessors[index];
            const u64 elementSize = cgltf_calc_size(accessor.type, accessor.component_type);
            if (accessor.type == cgltf_type_invalid || accessor.component_type == cgltf_component_type_invalid
                || elementSize == 0)
                return Reject(error, "an accessor has an unknown type");
            if (accessor.count == 0)
                return Reject(error, "an accessor has a zero count");
            if (accessor.buffer_view)
            {
                u64 required = 0;
                if (accessor.stride < elementSize
                    || !RequiredRunBytes(accessor.offset, accessor.stride, accessor.count, elementSize, required)
                    || required > accessor.buffer_view->size)
                    return Reject(error, "an accessor exceeds its bufferView");
            }
            if (!accessor.is_sparse)
                continue;
            const cgltf_accessor_sparse& sparse = accessor.sparse;
            const u64 indexSize = cgltf_component_size(sparse.indices_component_type);
            if (!sparse.indices_buffer_view || !sparse.values_buffer_view
                || (sparse.indices_component_type != cgltf_component_type_r_8u
                    && sparse.indices_component_type != cgltf_component_type_r_16u
                    && sparse.indices_component_type != cgltf_component_type_r_32u)
                || sparse.count == 0 || sparse.count > accessor.count)
                return Reject(error, "a sparse accessor is malformed");
            // cgltf steps the sparse values array by the base accessor stride, not by the packed
            // element size, so an interleaved base view would read past the values.
            if (accessor.stride != elementSize)
                return Reject(error, "a sparse accessor requires a tightly packed base");
            u64 indicesRequired = 0;
            u64 valuesRequired = 0;
            if (!RequiredRunBytes(sparse.indices_byte_offset, indexSize, sparse.count, indexSize, indicesRequired)
                || !RequiredRunBytes(sparse.values_byte_offset, elementSize, sparse.count, elementSize, valuesRequired)
                || indicesRequired > sparse.indices_buffer_view->size
                || valuesRequired > sparse.values_buffer_view->size)
                return Reject(error, "a sparse accessor exceeds its bufferViews");
        }
        error.clear();
        return true;
    }
}
