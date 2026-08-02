/**
 * @file Windows.h
 *
 * Case shim. Some vendored projects include <Windows.h> with a capital W, which is fine on the
 * case insensitive filesystems Windows normally uses. mingw-w64 ships the header as windows.h, so
 * on a case sensitive filesystem the cross compile fails to find it.
 *
 * Putting this directory on the include path fixes those projects without patching their sources.
 * */
#pragma once

#include <windows.h>
