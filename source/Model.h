#pragma once

/**
	The plate as numbers. The arithmetic is the `kModel` GLSL library in
	Shaders.cpp; the harness takes these constants and measures everything
	else out of the picture.

	**The characteristic curve.** Exposure H is the time-averaged plate
	exposure over the window, with white under the chosen light exposing to
	exactly 1 (see Spectral.h) and Sensitivity adding stops. With
	x = log10 H, the developed coverage is

	    c( x ) = ( sp( x - Toe ) - sp( x - Toe - Latitude ) ) / Latitude,
	    sp( u ) = ln( 1 + e^( Knee u ) ) / Knee,

	0 below the toe, 1 past the shoulder, a straight line of slope
	1 / Latitude between. Density is D = h ( Fog + Gamma Latitude c ), where
	h is the coating thickness at the pixel (1 on a flat pour) and Gamma is
	`Development`. The straight line's slope in D is Gamma to within
	Gamma ( e^( -Knee dt ) + e^( -Knee ds ) ), dt and ds the distances inside
	the toe and the shoulder; the extended line meets Fog at exactly x = Toe
	and Fog + Gamma Latitude at exactly x = Toe + Latitude.

	Wet collodion is a high-contrast, short-latitude material: Hertel,
	Skladnikiewitz and Schmidt (1997) show characteristic curves rising to a
	net density of 2 over about 1.5 log units for the iodide-only recipe.
	The numbers here are a choice inside that picture, made so that a red at
	7% of white's exposure (see Spectral.h) sits below the toe and a blue sky
	at 80% sits on the shoulder: reds black, skies white, from the plate.

	**Presentation.** The silver image is a fraction a = 1 - 10^( -D ) of the
	light intercepted. A tintype or ambrotype shows the silver by reflection
	over a black plate: mix( plate, silver, a ). A negative on a light box
	transmits 1 - a. Tone moves the silver (or the light) from neutral to
	warm.
*/
namespace wetplate::model
{

/// log10 H where the straight line meets the fog level, with white at 0.
constexpr double kToe = -1.1;

/// log10 H from the toe to the shoulder.
constexpr double kLatitude = 1.2;

/// How sharply the toe and the shoulder bend, per log10 unit.
constexpr double kKnee = 6.0;

/// The buckets: at most this many, whatever `Buckets` asks for. Sixteen
/// because the sum pass binds one sampler per bucket and sixteen texture
/// units per stage is what OpenGL 4.1 guarantees.
constexpr int kMaxBuckets = 16;

/// What a frame is worth in seconds when the host's clock has not moved
/// yet, or has moved implausibly.
constexpr double kNominalFrame  = 1.0 / 60.0;
constexpr double kMinFrameDelta = 1.0 / 240.0;
constexpr double kMaxFrameDelta = 0.25;

/// Presentation colours, linear RGB. Neutral and warm ends of the silver,
/// the plate behind it, and the light behind a negative.
constexpr float kTintypeSilverNeutral[ 3 ] = { 0.80f, 0.80f, 0.80f };
constexpr float kTintypeSilverWarm[ 3 ]    = { 0.86f, 0.72f, 0.52f };
constexpr float kTintypePlate[ 3 ]         = { 0.020f, 0.017f, 0.014f };
constexpr float kAmbrotypeSilverNeutral[ 3 ] = { 0.84f, 0.86f, 0.90f };
constexpr float kAmbrotypeSilverWarm[ 3 ]    = { 0.88f, 0.80f, 0.66f };
constexpr float kAmbrotypePlate[ 3 ]         = { 0.012f, 0.012f, 0.018f };
constexpr float kNegativeLightNeutral[ 3 ] = { 1.0f, 1.0f, 1.0f };
constexpr float kNegativeLightWarm[ 3 ]    = { 1.0f, 0.90f, 0.72f };

/// Negative-control hooks. Always 0 in the plugin. Each perturbs the
/// plugin's own model so that `wttest --negative` can show a check failing.
enum Perturb : int
{
	kPerturbLumaWeights   = 1,  ///< weight by BT.709 luma, not the plate: --spectral fails
	kPerturbOneFrame      = 2,  ///< integrate one frame, not the window: --ghost, --bucket fail
	kPerturbLinearDrain   = 4,  ///< coating linear in s, not sqrt( s ): --drainage fails
	kPerturbGamma         = 8,  ///< gamma x 0.8 in the shader: --curve fails
	kPerturbResizeClears  = 16, ///< a resize clears the buckets: --resize fails
	kPerturbTakeNeverEnds = 32, ///< Take keeps integrating past Exposure: --take fails
};

/// Probe hooks for the harness, 0 in the plugin: 1 makes the plate pass
/// write ( H, coating, D, 1 ) as raw floats instead of the picture.
enum Probe : int
{
	kProbeNone = 0,
	kProbeRaw  = 1,
};

} // namespace wetplate::model
