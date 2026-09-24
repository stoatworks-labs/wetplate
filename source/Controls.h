#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	`CFFGLPluginManager::SetParamInfo` clamps a STANDARD default into 0..1
	*before* returning, and `SetParamRange` can only be called afterwards, so
	a parameter declared in seconds cannot declare a default in seconds. Every
	slider here is therefore a plain 0..1 float and the conversions live in
	this one file, which the plugin and the harness both use.

	Every mapping a check needs to hit exactly has an inverse. Neutral
	positions land on their value EXACTLY in binary: Sensitivity 0.5 is 0
	stops (0.5 x 6 - 3), Development 0.5 is the plate's stated gamma, Coating
	Variation 0 is a flat coating (h = 1 + 0 x anything), and Exposure at a
	multiple of 1/8 is a power of two of seconds.

	Options are mapped by INDEX. An option parameter's range reads back 0..1
	from the SDK whatever its element count, so nothing outside this file
	should reason from the range. `FF_TYPE_INTEGER` (Buckets) holds its real
	value.
*/
namespace wetplate::controls
{

/// Exposure: the window the plate integrates, 1/16 s to 16 s, geometric.
/// 0.5 is exactly 1 s, and every power of two lands exactly: 0.375 is
/// exactly 0.5 s. That matters: a window of 0.50000002 s puts its bucket
/// grid a frame late by the sixth bucket, which the harness measured.
double ExposureSeconds( float value );
float ExposureParam( double seconds );

/// Sensitivity: -3 to +3 stops on the plate's exposure, linear. 0.5 is 0.
float SensitivityStops( float value );
float SensitivityParam( float stops );

/// Development: the straight line's slope, density per log10 H.
/// 0.5 is the plate's stated gamma of 1.2; 0 is 0.6 and 1 is 2.4, geometric.
float DevelopmentGamma( float value );
float DevelopmentParam( float gamma );

/// Development also lifts the fog: 0.004 density at 0, 0.030 at 1, linear.
/// A fog of 0.065 read as a milky mid grey over every black on the demo
/// clips; a tintype's blacks are the plate, not the fog.
float DevelopmentFog( float value );

/// Coating Variation: V in h( s ) = 1 + V ( sqrt( s ) - 2/3 ), 0 to 1.2,
/// linear. V = 1.2 runs the coating from 0.2 at the pour corner to 1.4 at the
/// drain corner.
float CoatingVariation( float value );
float CoatingVariationParam( float v );

/// Bare Edge: how far in from the edge the pour never reached, as a fraction
/// of the shorter side, 0 to 0.12, linear. 0 is exactly none.
float BareEdgeFraction( float value );

/// Defects: how many dust specks and comets, 0 to 48, linear, rounded.
int DefectCount( float value );

/// Tone: 0 is neutral silver on the plate, 1 is fully warm, linear.
float Tone( float value );

/// Vignette: exposure falloff at the corner, 0 to 0.85 of the light lost,
/// linear. 0 is exactly none.
float VignetteLoss( float value );

/// Option counts, and names in their menu order.
constexpr int kPlateCount = 3;
const char* PlateName( int index );
constexpr int kLightCount = 3;
const char* LightName( int index );
constexpr int kModeCount = 2;
const char* ModeName( int index );
constexpr int kPourCount = 4;
const char* PourName( int index );

/// Buckets: the ring's length, a real integer.
constexpr int kMinBuckets = 2;
constexpr int kMaxBuckets = 16;

/// An option's value to its index, rounded and clamped.
int OptionIndex( float value, int count );

/// The drain axis for a Pour corner: the unit direction from the pour corner
/// to the opposite one, in a frame whose x runs right and y runs DOWN. `s`
/// along the drain is the projection of ( x / W, y / H ) from the pour
/// corner onto ( dx, dy ), divided by the projection of the far corner, so
/// that s is 0 at the pour corner and 1 at the drain corner.
void PourDirection( int pour, float& dx, float& dy );
void PourOrigin( int pour, float& ox, float& oy );

} // namespace wetplate::controls
