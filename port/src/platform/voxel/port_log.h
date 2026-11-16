/*
 * PS Vita logging shim for emerald-3ds voxel code.
 * From HctrRB/pokeemerald-vita-3ds (MIT; see LICENSE and NOTICE.md).
 */
#ifndef GUARD_PORT_LOG_H
#define GUARD_PORT_LOG_H

#include <stdio.h>
#include "../vita/vita_platform.h"

#ifndef PORT_LOG
#define PORT_LOG(...) VITA_LOG(__VA_ARGS__)
#endif

#endif // GUARD_PORT_LOG_H
