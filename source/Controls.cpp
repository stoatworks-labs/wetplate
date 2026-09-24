#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace wetplate::controls
{
namespace
{
float clamp01( float v )
{
	return std::clamp( v, 0.0f, 1.0f );
}
} // namespace

double ExposureSeconds( float value )
{
	return std::exp2( 8.0 * static_cast< double >( clamp01( value ) ) - 4.0 );
}

float ExposureParam( double seconds )
{
	return clamp01( static_cast< float >( ( std::log2( std::max( seconds, 0.0625 ) ) + 4.0 ) / 8.0 ) );
}

float SensitivityStops( float value )
{
	return clamp01( value ) * 6.0f - 3.0f;
}

float SensitivityParam( float stops )
{
	return clamp01( ( stops + 3.0f ) / 6.0f );
}

float DevelopmentGamma( float value )
{
	return 1.2f * std::exp2( 2.0f * clamp01( value ) - 1.0f );
}

float DevelopmentParam( float gamma )
{
	return clamp01( ( std::log2( std::max( gamma, 0.6f ) / 1.2f ) + 1.0f ) / 2.0f );
}

float DevelopmentFog( float value )
{
	return 0.004f + 0.026f * clamp01( value );
}

float CoatingVariation( float value )
{
	return 1.2f * clamp01( value );
}

float CoatingVariationParam( float v )
{
	return clamp01( v / 1.2f );
}

float BareEdgeFraction( float value )
{
	return 0.12f * clamp01( value );
}

int DefectCount( float value )
{
	return static_cast< int >( std::lround( 48.0f * clamp01( value ) ) );
}

float Tone( float value )
{
	return clamp01( value );
}

float VignetteLoss( float value )
{
	return 0.85f * clamp01( value );
}

const char* PlateName( int index )
{
	static const char* const names[ kPlateCount ] = { "Tintype", "Ambrotype", "Negative" };
	return names[ std::clamp( index, 0, kPlateCount - 1 ) ];
}

const char* LightName( int index )
{
	static const char* const names[ kLightCount ] = { "Daylight", "Tungsten", "Flash" };
	return names[ std::clamp( index, 0, kLightCount - 1 ) ];
}

const char* ModeName( int index )
{
	static const char* const names[ kModeCount ] = { "Continuous", "Take" };
	return names[ std::clamp( index, 0, kModeCount - 1 ) ];
}

const char* PourName( int index )
{
	static const char* const names[ kPourCount ] = { "Top Left", "Top Right", "Bottom Left", "Bottom Right" };
	return names[ std::clamp( index, 0, kPourCount - 1 ) ];
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

void PourOrigin( int pour, float& ox, float& oy )
{
	switch( std::clamp( pour, 0, kPourCount - 1 ) )
	{
	default:
	case 0: ox = 0.0f; oy = 0.0f; break;
	case 1: ox = 1.0f; oy = 0.0f; break;
	case 2: ox = 0.0f; oy = 1.0f; break;
	case 3: ox = 1.0f; oy = 1.0f; break;
	}
}

void PourDirection( int pour, float& dx, float& dy )
{
	float ox, oy;
	PourOrigin( pour, ox, oy );
	//From the pour corner to the opposite one, in a y-down frame.
	dx = ox > 0.5f ? -1.0f : 1.0f;
	dy = oy > 0.5f ? -1.0f : 1.0f;
}

} // namespace wetplate::controls
