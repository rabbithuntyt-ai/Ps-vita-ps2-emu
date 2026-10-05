#pragma once

// OpenGL entry points for the GPU renderer. The renderer only uses the
// fixed-function OpenGL subset implemented by vitaGL (PlayStation Vita), so
// the very same code runs on the Vita and, through Mesa, in host tests.

#if defined(__vita__)
#include <vitaGL.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif
