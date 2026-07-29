/* Single translation unit that instantiates miniaudio's implementation.
 * Kept out of the engine sources so miniaudio's warnings and its very large
 * preprocessor output are compiled exactly once. */
#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include <miniaudio.h>
