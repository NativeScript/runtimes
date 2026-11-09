#include <TargetConditionals.h>

#ifdef EMBED_METADATA_SIZE

extern const unsigned char
#if TARGET_CPU_ARM64
    embedded_metadata[EMBED_METADATA_SIZE] = "NSMDSectionHeaderARM";
#else
    embedded_metadata[EMBED_METADATA_SIZE] = "NSMDSectionHeaderX86";
#endif

#endif  // EMBED_METADATA_SIZE
