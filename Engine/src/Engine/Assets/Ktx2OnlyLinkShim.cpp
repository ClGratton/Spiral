#include <ktx.h>
#include <ktxint.h>
#include <texture1.h>

// KTX-Software v4.4.2's common texture factory retains one reference to the
// KTX1 constructor even when its CMake KTX1 option is disabled. Spiral never
// calls that factory and deliberately excludes texture1.c; satisfy the pinned
// private symbol with an explicit rejection instead of admitting KTX1 code.
extern "C" KTX_error_code ktxTexture1_constructFromStreamAndHeader(
    ktxTexture1* texture, ktxStream* stream, KTX_header* header,
    ktxTextureCreateFlags createFlags)
{
    (void) texture;
    (void) stream;
    (void) header;
    (void) createFlags;
    return KTX_UNSUPPORTED_TEXTURE_TYPE;
}
