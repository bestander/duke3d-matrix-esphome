#pragma once

namespace esphome {
namespace hub75_matrix {

// Build GRP → SD splash cache (see docs/duke3d-matrix.md). Skips if cache header matches current GRP size.
bool grp_title_splash_build_cache_if_needed(const char *grp_path, const char *cache_path);

}  // namespace hub75_matrix
}  // namespace esphome
