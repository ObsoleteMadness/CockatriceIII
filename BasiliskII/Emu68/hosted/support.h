/*
 *  support.h - Hosted wrap of upstream support.h
 *
 *  Quoted includes from upstream/include/support.h resolve siblings in that
 *  directory first, so they would pick the GCC SIMD register pins in
 *  upstream/include/RegLock.h. Include the Clang-safe pins and define the
 *  guard before include_next so that sibling include is skipped.
 */

#ifndef EMU68_HOSTED_SUPPORT_H
#define EMU68_HOSTED_SUPPORT_H

#include "RegLock.h"
#include "A64.h"

#include "../upstream/include/support.h"

#endif /* EMU68_HOSTED_SUPPORT_H */
