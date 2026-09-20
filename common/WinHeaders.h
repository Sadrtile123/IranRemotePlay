#pragma once
// Include FIRST before any Windows header. Standalone Asio requires winsock2.h
// to be included by itself; plain windows.h drags in the old winsock.h and
// breaks the Asio build. NOMINMAX keeps std::min/max usable.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
