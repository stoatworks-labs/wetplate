#pragma once

#include <string>

/**
	The five passes.

	1. **expose** -- picture size, R32F, ADDED into the current bucket with
	   GL_ONE / GL_ONE blending. The host's frame, sRGB-decoded to linear,
	   through Smits' RGB-to-spectrum rule with the seven basis exposures
	   for the chosen light (Spectral.h), times the frame's seconds: what
	   this frame added to the plate.

	2. **sum** -- picture size, R32F. The window: every bucket in it, each
	   times its weight (one over the seconds in the window, or 0 for a
	   bucket that has fallen out of it). The result is H, the plate's mean
	   exposure over the window, with white at exactly 1.

	3. **coating** -- picture size, R32F, rendered only when its inputs
	   change. The coating thickness h from the drainage law, times the mask
	   of what the pour reached (the bare edge) and what the silver bath did
	   not (dust and comets).

	4. **resample** -- one bucket into a bucket of another size, bilinear.
	   Runs only on a resize, once per bucket, so that a raster change
	   mid-exposure carries the exposure so far across instead of clearing
	   it.

	5. **plate** -- to the host. H through Sensitivity and the vignette,
	   log10, the characteristic curve, density scaled by the coating; then
	   the plate: silver over black (tintype, ambrotype) or a negative on a
	   light box, toned, sRGB-encoded, and mixed with the source.

	The curve and the spectral rule are one GLSL library, `kModel`, compiled
	into the expose and plate passes. Each shader is assembled at run time
	from it, so `wttest --dump-shaders DIR` writes out exactly the strings
	the plugin compiles, and that is what `tools/verify.sh` hands to glslc.
*/
namespace wetplate::shaders
{

std::string Vertex();
std::string Expose();
std::string Sum();
std::string Coating();
std::string Resample();
std::string Plate();

} // namespace wetplate::shaders
