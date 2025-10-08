// DebugMetrics.cpp
#include "debug/DebugMetrics.h"

DebugMetrics& DebugMetrics::instance() {
    static DebugMetrics inst;
    return inst;
}
