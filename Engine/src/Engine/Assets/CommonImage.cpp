#include "Engine/Assets/CommonImage.h"

#include "spng.h"
#include "zlib.h"

#include <algorithm>
#include <array>
#include <climits>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

#include "jpeglib.h"
#include "jerror.h"

namespace Engine
{
    namespace
    {
        constexpr u64 MaximumPngChunkBytes = 16ull * 1024ull * 1024ull;
        constexpr u64 MinimumPngChunkCacheBytes = 64ull * 1024ull;
        constexpr u64 MaximumPngChunkCacheBytes = 32ull * 1024ull * 1024ull;
        constexpr u64 JpegWorkingSlackBytes = 16ull * 1024ull * 1024ull;
        constexpr int MaximumJpegScans = 64;

        bool IsPng(std::span<const u8> source)
        {
            constexpr std::array<u8, 8> signature { 137, 80, 78, 71, 13, 10, 26, 10 };
            return source.size() >= signature.size() && std::equal(signature.begin(), signature.end(), source.begin());
        }

        bool IsJpeg(std::span<const u8> source)
        {
            return source.size() >= 3 && source[0] == 0xff && source[1] == 0xd8 && source[2] == 0xff;
        }

        u64 SaturatingMultiply(u64 value, u64 factor)
        {
            return factor != 0 && value > std::numeric_limits<u64>::max() / factor
                ? std::numeric_limits<u64>::max() : value * factor;
        }

        u64 SaturatingAdd(u64 value, u64 addend)
        {
            return value > std::numeric_limits<u64>::max() - addend ? std::numeric_limits<u64>::max() : value + addend;
        }

        bool ExceedsDimensionPolicy(u64 width, u64 height, const CommonImageLimits& limits)
        {
            return width == 0 || height == 0 || width > limits.MaximumWidth || height > limits.MaximumHeight;
        }

        bool ExceedsOutputPolicy(u64 width, u64 height, const CommonImageLimits& limits)
        {
            const u64 pixels = width * height;
            return pixels > limits.MaximumPixels || pixels > std::numeric_limits<u64>::max() / 4
                || pixels * 4 > limits.MaximumOutputBytes;
        }

        enum class PngEnd { AtIend, MissingIend, TrailingData };

        PngEnd FindPngEnd(std::span<const u8> source)
        {
            size_t offset = 8;
            while (source.size() - offset >= 12)
            {
                const u64 length = (static_cast<u64>(source[offset]) << 24) | (static_cast<u64>(source[offset + 1]) << 16)
                    | (static_cast<u64>(source[offset + 2]) << 8) | source[offset + 3];
                const u64 next = offset + 12 + length;
                if (next > source.size())
                    return PngEnd::MissingIend;
                const bool isIend = std::memcmp(source.data() + offset + 4, "IEND", 4) == 0;
                offset = static_cast<size_t>(next);
                if (isIend)
                    return offset == source.size() ? PngEnd::AtIend : PngEnd::TrailingData;
            }
            return PngEnd::MissingIend;
        }

        // libspng stops inflating once the last row is written, so surplus compressed
        // data would go unseen. The zlib stream must end exactly at the byte count the
        // header implies; this pass aborts as soon as that count is exceeded.
        u64 ExpectedPngRawBytes(const spng_ihdr& header)
        {
            const u64 channels = header.color_type == SPNG_COLOR_TYPE_TRUECOLOR ? 3
                : header.color_type == SPNG_COLOR_TYPE_GRAYSCALE_ALPHA ? 2
                : header.color_type == SPNG_COLOR_TYPE_TRUECOLOR_ALPHA ? 4 : 1;
            const u64 bitsPerPixel = channels * header.bit_depth;
            auto subimage = [bitsPerPixel](u64 width, u64 height)
            {
                return width == 0 || height == 0 ? 0 : SaturatingMultiply(height, 1 + (SaturatingMultiply(width, bitsPerPixel) + 7) / 8);
            };
            if (header.interlace_method == SPNG_INTERLACE_NONE)
                return subimage(header.width, header.height);
            constexpr std::array<std::array<u64, 4>, 7> passes { { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 },
                { 2, 0, 4, 4 }, { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } } };
            u64 total = 0;
            for (const auto& pass : passes)
            {
                const u64 width = header.width > pass[0] ? (header.width - pass[0] + pass[2] - 1) / pass[2] : 0;
                const u64 height = header.height > pass[1] ? (header.height - pass[1] + pass[3] - 1) / pass[3] : 0;
                total = SaturatingAdd(total, subimage(width, height));
            }
            return total;
        }

        voidpf PngInflateAllocate(voidpf, uInt items, uInt size)
        {
            return std::calloc(items, size);
        }

        void PngInflateFree(voidpf, voidpf address)
        {
            std::free(address);
        }

        struct PngInflate
        {
            z_stream Stream {};
            bool Open = false;
            ~PngInflate()
            {
                if (Open)
                    inflateEnd(&Stream);
            }
        };

        bool PngIdatStreamIsExact(std::span<const u8> source, const spng_ihdr& header)
        {
            const u64 expected = ExpectedPngRawBytes(header);
            PngInflate inflater;
            inflater.Stream.zalloc = PngInflateAllocate;
            inflater.Stream.zfree = PngInflateFree;
            if (inflateInit(&inflater.Stream) != Z_OK)
                return false;
            inflater.Open = true;
            std::array<Bytef, 16384> scratch;
            u64 produced = 0;
            bool ended = false;
            size_t offset = 8;
            while (source.size() - offset >= 12)
            {
                const u64 length = (static_cast<u64>(source[offset]) << 24) | (static_cast<u64>(source[offset + 1]) << 16)
                    | (static_cast<u64>(source[offset + 2]) << 8) | source[offset + 3];
                if (length > source.size() - offset - 12)
                    break;
                const bool isIdat = std::memcmp(source.data() + offset + 4, "IDAT", 4) == 0;
                if (isIdat)
                {
                    if (ended)
                        return false;
                    inflater.Stream.next_in = const_cast<Bytef*>(source.data() + offset + 8);
                    inflater.Stream.avail_in = static_cast<uInt>(length);
                    for (;;)
                    {
                        inflater.Stream.next_out = scratch.data();
                        inflater.Stream.avail_out = static_cast<uInt>(scratch.size());
                        const int result = inflate(&inflater.Stream, Z_NO_FLUSH);
                        produced += scratch.size() - inflater.Stream.avail_out;
                        if (produced > expected)
                            return false;
                        if (result == Z_STREAM_END)
                        {
                            ended = true;
                            break;
                        }
                        if (result != Z_OK && result != Z_BUF_ERROR)
                            return false;
                        if (inflater.Stream.avail_out != 0)
                            break;
                    }
                    if (ended && inflater.Stream.avail_in != 0)
                        return false;
                }
                offset += 12 + static_cast<size_t>(length);
            }
            return ended && produced == expected;
        }

        struct PngContext
        {
            spng_ctx* Value = nullptr;
            ~PngContext() { spng_ctx_free(Value); }
        };

        bool DecodePng(std::span<const u8> source, const CommonImageLimits& limits,
            DecodedCommonImage& output, std::string& error)
        {
            PngContext context;
            context.Value = spng_ctx_new(0);
            if (!context.Value)
            {
                error = "PNG decoder allocation failed";
                return false;
            }
            // libspng 0.7.4 refuses to raise SPNG_CHUNK_COUNT_LIMIT, so its default of
            // 1000 chunks stays in force.
            const u64 cacheBytes = std::clamp(limits.MaximumOutputBytes, MinimumPngChunkCacheBytes, MaximumPngChunkCacheBytes);
            const u64 chunkBytes = std::min({ cacheBytes, MaximumPngChunkBytes, limits.MaximumSourceBytes });
            if (spng_set_image_limits(context.Value, limits.MaximumWidth, limits.MaximumHeight) != SPNG_OK
                || spng_set_chunk_limits(context.Value, static_cast<size_t>(chunkBytes), static_cast<size_t>(cacheBytes)) != SPNG_OK
                || spng_set_crc_action(context.Value, SPNG_CRC_ERROR, SPNG_CRC_ERROR) != SPNG_OK)
            {
                error = "PNG decoder limits are invalid";
                return false;
            }
            if (spng_set_png_buffer(context.Value, source.data(), source.size()) != SPNG_OK)
            {
                error = "PNG input is malformed";
                return false;
            }
            spng_ihdr header {};
            const int headerResult = spng_get_ihdr(context.Value, &header);
            if (headerResult == SPNG_EUSER_WIDTH || headerResult == SPNG_EUSER_HEIGHT)
            {
                error = "PNG dimensions exceed policy";
                return false;
            }
            if (headerResult != SPNG_OK)
            {
                error = std::string("PNG header is malformed: ") + spng_strerror(headerResult);
                return false;
            }
            if (ExceedsDimensionPolicy(header.width, header.height, limits))
            {
                error = "PNG dimensions exceed policy";
                return false;
            }
            if (ExceedsOutputPolicy(header.width, header.height, limits))
            {
                error = "PNG pixel output exceeds policy";
                return false;
            }
            if (!PngIdatStreamIsExact(source, header))
            {
                error = "PNG decode failed: IDAT stream does not match the image size";
                return false;
            }
            const u64 pixels = static_cast<u64>(header.width) * header.height;
            std::vector<u8> candidate;
            try
            {
                candidate.resize(static_cast<size_t>(pixels * 4));
            }
            catch (const std::bad_alloc&)
            {
                error = "PNG output allocation failed";
                return false;
            }
            size_t decodedSize = 0;
            const int sizeResult = spng_decoded_image_size(context.Value, SPNG_FMT_RGBA8, &decodedSize);
            const int decodeResult = sizeResult == SPNG_OK && decodedSize == candidate.size()
                ? spng_decode_image(context.Value, candidate.data(), candidate.size(), SPNG_FMT_RGBA8, SPNG_DECODE_TRNS) : sizeResult;
            if (sizeResult != SPNG_OK || decodedSize != candidate.size() || decodeResult != SPNG_OK)
            {
                error = std::string("PNG decode failed: ") + spng_strerror(decodeResult);
                return false;
            }
            const int finishResult = spng_decode_chunks(context.Value);
            if (finishResult != SPNG_OK)
            {
                error = std::string("PNG decode failed: ") + spng_strerror(finishResult);
                return false;
            }
            const PngEnd end = FindPngEnd(source);
            if (end != PngEnd::AtIend)
            {
                error = end == PngEnd::MissingIend ? "PNG is missing IEND" : "PNG has trailing data after IEND";
                return false;
            }
            DecodedCommonImage replacement;
            replacement.Source = CommonImageSource::Png;
            replacement.Width = header.width;
            replacement.Height = header.height;
            replacement.Rgba8 = std::move(candidate);
            output = std::move(replacement);
            return true;
        }

        // libjpeg reports through longjmp, so every frame between a setjmp and the
        // longjmp below (the Read/Decode phase functions, libjpeg, and the callbacks)
        // holds only trivially destructible locals. Owning objects live in the
        // non-jumping DecodeJpeg caller.
        struct JpegErrorManager
        {
            jpeg_error_mgr Base;
            std::jmp_buf Jump;
            int LibraryCode;
            char Message[JMSG_LENGTH_MAX];
        };

        struct JpegSession
        {
            jpeg_decompress_struct Info;
            JpegErrorManager Error;
            jpeg_progress_mgr Progress;
            u32 Width;
            u32 Height;
            bool PolicyFailure;
        };

        JpegErrorManager& ErrorOf(j_common_ptr info)
        {
            return *reinterpret_cast<JpegErrorManager*>(info->err);
        }

        void JpegErrorExit(j_common_ptr info)
        {
            JpegErrorManager& error = ErrorOf(info);
            error.LibraryCode = info->err->msg_code;
            (*info->err->format_message)(info, error.Message);
            std::longjmp(error.Jump, 1);
        }

        void JpegEmitMessage(j_common_ptr info, int level)
        {
            // Negative levels are corrupt-data warnings; they are failures here.
            if (level < 0)
                JpegErrorExit(info);
        }

        void JpegProgress(j_common_ptr info)
        {
            if (info->is_decompressor && reinterpret_cast<j_decompress_ptr>(info)->input_scan_number > MaximumJpegScans)
            {
                JpegErrorManager& error = ErrorOf(info);
                error.LibraryCode = 0;
                std::snprintf(error.Message, sizeof(error.Message), "%s", "JPEG scan count exceeds policy");
                std::longjmp(error.Jump, 1);
            }
        }

        bool FailJpegPolicy(JpegSession& session, const char* message)
        {
            session.PolicyFailure = true;
            std::snprintf(session.Error.Message, sizeof(session.Error.Message), "%s", message);
            return false;
        }

        // Parses the header and enforces every policy that does not need pixels.
        // Nothing sized by the untrusted dimensions is allocated before this
        // returns true; jpeg_start_decompress has not been called.
        bool ReadJpegHeader(JpegSession& session, std::span<const u8> source, const CommonImageLimits& limits)
        {
            if (setjmp(session.Error.Jump))
                return false;
            jpeg_create_decompress(&session.Info);
            jpeg_mem_src(&session.Info, source.data(), static_cast<unsigned long>(source.size()));
            if (jpeg_read_header(&session.Info, TRUE) != JPEG_HEADER_OK)
                return FailJpegPolicy(session, "JPEG header is malformed");

            jpeg_decompress_struct& info = session.Info;
            if (ExceedsDimensionPolicy(info.image_width, info.image_height, limits))
                return FailJpegPolicy(session, "JPEG dimensions exceed policy");
            if (info.arith_code)
                return FailJpegPolicy(session, "JPEG arithmetic coding is unsupported");
            if (info.data_precision != 8)
                return FailJpegPolicy(session, "JPEG sample precision is unsupported");
            const bool supportedColor = (info.jpeg_color_space == JCS_GRAYSCALE && info.num_components == 1)
                || ((info.jpeg_color_space == JCS_YCbCr || info.jpeg_color_space == JCS_RGB) && info.num_components == 3);
            if (!supportedColor)
                return FailJpegPolicy(session, "JPEG color space is unsupported");

            info.out_color_space = JCS_EXT_RGBA;
            jpeg_calc_output_dimensions(&info);
            if (ExceedsDimensionPolicy(info.output_width, info.output_height, limits))
                return FailJpegPolicy(session, "JPEG dimensions exceed policy");
            if (ExceedsOutputPolicy(info.output_width, info.output_height, limits))
                return FailJpegPolicy(session, "JPEG pixel output exceeds policy");

            // Multi-scan streams keep every DCT coefficient in virtual arrays. With at
            // most three components that is at most six bytes per padded pixel, below
            // this budget, which jmemnobs.c enforces by failing instead of creating
            // a backing-store temp file.
            const u64 workingBudget = SaturatingAdd(SaturatingMultiply(limits.MaximumOutputBytes, 2), JpegWorkingSlackBytes);
            info.mem->max_memory_to_use = static_cast<long>(std::min<u64>(workingBudget, static_cast<u64>(LONG_MAX)));

            session.Width = info.output_width;
            session.Height = info.output_height;
            info.progress = &session.Progress;
            return true;
        }

        bool DecodeJpegPixels(JpegSession& session, u8* pixels)
        {
            if (setjmp(session.Error.Jump))
                return false;
            jpeg_decompress_struct& info = session.Info;
            jpeg_start_decompress(&info);
            if (info.output_width != session.Width || info.output_height != session.Height || info.output_components != 4)
                return FailJpegPolicy(session, "JPEG output layout changed after header validation");
            const size_t stride = static_cast<size_t>(session.Width) * 4;
            while (info.output_scanline < info.output_height)
            {
                JSAMPROW row = pixels + static_cast<size_t>(info.output_scanline) * stride;
                if (jpeg_read_scanlines(&info, &row, 1) != 1)
                    return FailJpegPolicy(session, "JPEG decode stalled before the last scanline");
            }
            jpeg_finish_decompress(&info);
            if (info.src->bytes_in_buffer != 0)
                return FailJpegPolicy(session, "JPEG has trailing data after end of image");
            return true;
        }

        const char* JpegHeaderFailureText(int libraryCode)
        {
            switch (libraryCode)
            {
            case JERR_EMPTY_IMAGE:
            case JERR_IMAGE_TOO_BIG:
                return "JPEG dimensions exceed policy: ";
            case JERR_SOF_UNSUPPORTED:
                return "JPEG coding process is unsupported: ";
            default:
                return "JPEG header is malformed: ";
            }
        }

        const char* JpegDecodeFailureText(int libraryCode)
        {
            switch (libraryCode)
            {
            case JERR_NO_BACKING_STORE:
            case JERR_OUT_OF_MEMORY:
            case JERR_BAD_ALLOC_CHUNK:
                return "JPEG memory budget exceeds policy: ";
            case JERR_NOT_COMPILED:
                return "JPEG coding process is unsupported: ";
            default:
                return "JPEG decode failed: ";
            }
        }

        std::string JpegFailureText(const JpegSession& session, const char* (*classify)(int))
        {
            if (session.PolicyFailure || session.Error.LibraryCode == 0)
                return session.Error.Message;
            return std::string(classify(session.Error.LibraryCode)) + session.Error.Message;
        }

        struct JpegDecompressor
        {
            JpegSession* Session = nullptr;
            ~JpegDecompressor() { jpeg_destroy_decompress(&Session->Info); }
        };

        bool DecodeJpeg(std::span<const u8> source, const CommonImageLimits& limits,
            DecodedCommonImage& output, std::string& error)
        {
            if (source.size() > std::numeric_limits<unsigned long>::max())
            {
                error = "image source exceeds policy";
                return false;
            }
            JpegSession session {};
            session.Info.err = jpeg_std_error(&session.Error.Base);
            session.Error.Base.error_exit = JpegErrorExit;
            session.Error.Base.emit_message = JpegEmitMessage;
            session.Progress.progress_monitor = JpegProgress;
            JpegDecompressor decompressor { &session };

            if (!ReadJpegHeader(session, source, limits))
            {
                error = JpegFailureText(session, JpegHeaderFailureText);
                return false;
            }
            std::vector<u8> candidate;
            try
            {
                candidate.resize(static_cast<size_t>(session.Width) * session.Height * 4);
            }
            catch (const std::bad_alloc&)
            {
                error = "JPEG output allocation failed";
                return false;
            }
            if (!DecodeJpegPixels(session, candidate.data()))
            {
                error = JpegFailureText(session, JpegDecodeFailureText);
                return false;
            }
            DecodedCommonImage replacement;
            replacement.Source = CommonImageSource::Jpeg;
            replacement.Width = session.Width;
            replacement.Height = session.Height;
            replacement.Rgba8 = std::move(candidate);
            output = std::move(replacement);
            return true;
        }
    }

    bool DecodeCommonImage(std::span<const u8> source, const CommonImageLimits& limits,
        DecodedCommonImage& output, std::string& error)
    {
        error.clear();
        if (source.empty())
        {
            error = "image source is empty";
            return false;
        }
        if (source.size() > limits.MaximumSourceBytes)
        {
            error = "image source exceeds policy";
            return false;
        }
        if (IsJpeg(source))
            return DecodeJpeg(source, limits, output, error);
        if (IsPng(source))
            return DecodePng(source, limits, output, error);
        error = "image content magic is unsupported";
        return false;
    }
}
