#pragma once

// Registration entry points for the benchmark groups.
//
// Cases register through explicit calls from main() rather than through
// self-registering static objects. Static registration would put the case list
// at the mercy of translation-unit initialisation order, and - worse for a
// harness whose whole job is reproducibility - would make the order in which
// cases run depend on the linker.
//
// Each `register*` call may consult `Runner::selected()` and skip building an
// expensive fixture that the current --filter would throw away. Each `report*`
// call runs AFTER `Runner::runAll()` and adds the metrics that are ratios of
// results rather than measurements of their own.

#include "Harness.hpp"

namespace voxl::bench {

void registerTerrainCases(Runner& runner);
void reportTerrainDerived(Runner& runner);

void registerMeshingCases(Runner& runner);
void reportMeshingDerived(Runner& runner);

void registerSubVoxelCases(Runner& runner);

void registerStorageCases(Runner& runner);

/// Guarded by `__has_include("world/LightEngine.hpp")`. Registers an
/// "unavailable" record when the header is not in the tree yet.
void registerLightingCases(Runner& runner);

/// Guarded by `__has_include("world/WorldSave.hpp")`. Registers an
/// "unavailable" record when the header is not in the tree yet.
void registerPersistenceCases(Runner& runner);

}  // namespace voxl::bench
