#pragma once

// miniz's CMake build generates this otherwise-empty visibility header for
// static consumers. Spiral compiles the pinned sources directly into Engine.
#ifndef MINIZ_EXPORT
    #define MINIZ_EXPORT
#endif
