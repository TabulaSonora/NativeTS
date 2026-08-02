// The single translation unit that compiles miniaudio itself.
//
// It is kept alone in its own file so the library's warnings never mix with the player's, and so
// `ts::warnings` can be left off this target without weakening it anywhere else.

#define MINIAUDIO_IMPLEMENTATION

// Nothing here decodes or encodes: the engine produces the samples and the CLI writes the WAVs.
// Leaving the decoders out drops most of miniaudio's size and all of its file format code.
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE

#include <miniaudio.h>
