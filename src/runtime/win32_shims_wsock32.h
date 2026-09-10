// src/runtime/win32_shims_wsock32.h — generic Winsock 1.1/2 ordinal shims
// (WSOCK32.dll / WS2_32.dll): byte-order conversion, WSAStartup/WSACleanup,
// gethostname, and the correct inet_addr/inet_ntoa bodies — no socket-table
// state, no protocol-specific literal. See win32_shims_wsock32.cpp for the
// exact ordinal-by-ordinal split rationale versus what stays d2vita-side.
#pragma once
namespace d2rt { class Bridge; }

void win32_shims_wsock32_install(d2rt::Bridge& br);
