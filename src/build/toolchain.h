/**
 * @file toolchain.h
 * */
#pragma once

#include "nyangine/base/base_basic.h"

#if OS_WINDOWS
#include "build/on_windows/toolchain.h"
#else
#include "build/on_linux/toolchain.h"
#endif
