#pragma once
// FogIdent.h - identity string compiled into every firmware image.
//
// Fog Console looks for this string inside a .bin file to learn which firmware it is (timer or remote) and which
// version, before the file is written to a device. `retain` keeps it through the linker's garbage collection even
// though no code refers to it.
//
// Format:  FOGFW|<kind>|<version>|       e.g.  FOGFW|timer|2.0.0|
#include "FogProto.h"

#define FOG_FW_IDENT(KIND) \
    extern "C" __attribute__((used, retain)) const char kFogFwIdent[] = "FOGFW|" KIND "|" FOG_FW_STRING "|"
