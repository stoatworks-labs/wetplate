#include "Shaders.h"

namespace wetplate::shaders
{
namespace
{
const char* const kVersion = "#version 410 core\n";

//---------------------------------------------------------------------------
// The vertex shader every pass shares.
//---------------------------------------------------------------------------
const char* const kVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// The model. A fragment, not a shader: no #version, no main. The expose and
// plate passes are each assembled around this one string, so there is one
// spectral rule and one curve in the plugin.
//---------------------------------------------------------------------------
const char* const kModel = R"(
uniform int Perturb;       //negative-control hooks; always 0 in the plugin

//sRGB transfer, both ways, the IEC 61966-2-1 piecewise form.
float srgbDecode( float v )
{
	return v <= 0.04045 ? v / 12.92 : pow( ( v + 0.055 ) / 1.055, 2.4 );
}
vec3 srgbDecode( vec3 v )
{
	return vec3( srgbDecode( v.r ), srgbDecode( v.g ), srgbDecode( v.b ) );
}
float srgbEncode( float v )
{
	v = clamp( v, 0.0, 1.0 );
	return v <= 0.0031308 ? v * 12.92 : 1.055 * pow( v, 1.0 / 2.4 ) - 0.055;
}
vec3 srgbEncode( vec3 v )
{
	return vec3( srgbEncode( v.r ), srgbEncode( v.g ), srgbEncode( v.b ) );
}

//Smits (1999): the reflectance of an RGB is a sum of seven basis spectra
//chosen by the order of its components. The plate's exposure to each basis
//under the chosen light is precomputed (Basis[]: white, cyan, magenta,
//yellow, red, green, blue), so the exposure of a pixel is the same sum of
//seven numbers. Pure red, green and blue come out as exactly Basis[4],
//Basis[5] and Basis[6]; white as exactly Basis[0], which is 1.
float plateExposure( vec3 rgb, float Basis[ 7 ] )
{
	float r = rgb.r, g = rgb.g, b = rgb.b;
	float e;
	if( r <= g && r <= b )
	{
		e = r * Basis[ 0 ];
		if( g <= b )
			e += ( g - r ) * Basis[ 1 ] + ( b - g ) * Basis[ 6 ];
		else
			e += ( b - r ) * Basis[ 1 ] + ( g - b ) * Basis[ 5 ];
	}
	else if( g <= r && g <= b )
	{
		e = g * Basis[ 0 ];
		if( r <= b )
			e += ( r - g ) * Basis[ 2 ] + ( b - r ) * Basis[ 6 ];
		else
			e += ( b - g ) * Basis[ 2 ] + ( r - b ) * Basis[ 4 ];
	}
	else
	{
		e = b * Basis[ 0 ];
		if( r <= g )
			e += ( r - b ) * Basis[ 3 ] + ( g - r ) * Basis[ 5 ];
		else
			e += ( g - b ) * Basis[ 3 ] + ( r - g ) * Basis[ 4 ];
	}
	//The negative control: a plate that sees like a luma meter.
	if( ( Perturb & 1 ) != 0 )
		e = dot( rgb, vec3( 0.2126, 0.7152, 0.0722 ) );
	return max( e, 0.0 );
}

//softplus, written so it neither overflows nor loses the small end: for
//u << 0 it is e^(Knee u) / Knee, for u >> 0 it is u.
float softplus( float u, float Knee )
{
	return max( u, 0.0 ) + log( 1.0 + exp( -Knee * abs( u ) ) ) / Knee;
}

//The characteristic curve, as coverage: 0 below the toe, 1 past the
//shoulder, a straight line of slope 1 / Latitude between.
float coverage( float logH, float Toe, float Latitude, float Knee )
{
	return clamp( ( softplus( logH - Toe, Knee ) - softplus( logH - Toe - Latitude, Knee ) ) / Latitude, 0.0, 1.0 );
}

//Coverage to density, on a coating of thickness h.
float density( float c, float h, float Fog, float Gamma, float Latitude )
{
	float g = Gamma * ( ( Perturb & 8 ) != 0 ? 0.8 : 1.0 );
	return h * ( Fog + g * Latitude * c );
}
)";

//---------------------------------------------------------------------------
// Pass 1: expose. One frame's contribution to the current bucket. The
// output raster is the input raster, so texel ( x, y ) is pixel ( x, y ):
// texelFetch, exact, whatever the host set the texture's filter to.
//---------------------------------------------------------------------------
const char* const kExposeBody = R"(
uniform sampler2D InputTexture;
uniform float Basis[ 7 ];  //plate exposure per Smits basis, white = 1
uniform float Dt;          //this frame's seconds

in vec2 uv;
out vec4 fragColor;

void main()
{
	vec3 rgb = texelFetch( InputTexture, ivec2( gl_FragCoord.xy ), 0 ).rgb;
	float e  = plateExposure( srgbDecode( rgb ), Basis );
	fragColor = vec4( e * Dt, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 2: sum. The window out of the buckets. Sixteen samplers, indexed by
// a constant-bounded loop so every index is a constant expression; a bucket
// past `Count`, or out of the window, has weight 0.
//---------------------------------------------------------------------------
const char* const kSumBody = R"(
uniform sampler2D Bucket[ 16 ];
uniform float Weight[ 16 ];  //1 / seconds in the window, or 0
uniform int Count;

in vec2 uv;
out vec4 fragColor;

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	float h = 0.0;
	for( int i = 0; i < 16; ++i )
		if( i < Count )
			h += texelFetch( Bucket[ i ], p, 0 ).r * Weight[ i ];
	fragColor = vec4( h, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 3: the coating. Thickness from drainage, the bare edge, dust and
// comets. Coordinates are y-DOWN fractions of the plate: p = ( uv.x,
// 1 - uv.y ), so row 0 is the top of the picture whatever GL's origin is.
//---------------------------------------------------------------------------
const char* const kCoatingBody = R"(
uniform vec2 Origin;        //the pour corner, y-down fractions
uniform vec2 Drain;         //( +-1, +-1 ): from the pour corner to the drain corner
uniform float Variation;    //V in h = 1 + V ( sqrt( s ) - 2/3 )
uniform float BareEdge;     //fraction of the shorter side the pour never reached
uniform vec2 Size;          //pixels
uniform int DefectCount;
uniform int Seed;

in vec2 uv;
out vec4 fragColor;

//PCG output mix: exact in 32 bits, the same on every driver.
uint hashInt( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
float hashUnit( uint h )
{
	return float( h >> 8u ) * ( 1.0 / 16777216.0 );
}

//Value noise along an edge: one hashed value per cell, linear between.
float edgeNoise( float along, uint edge )
{
	float x  = along * 14.0;
	float i  = floor( x );
	float t  = x - i;
	uint a   = uint( Seed ) * 65537u + edge * 4099u;
	float n0 = hashUnit( hashInt( a + uint( int( i ) + 1000 ) ) );
	float n1 = hashUnit( hashInt( a + uint( int( i ) + 1001 ) ) );
	return mix( n0, n1, t * t * ( 3.0 - 2.0 * t ) );
}

void main()
{
	vec2 p = vec2( uv.x, 1.0 - uv.y );

	//Drainage: s along the pour-to-drain diagonal, 0 at the pour corner and
	//1 at the drain corner. Jeffreys: thickness grows as the square root of
	//the distance drained past. The mean of sqrt( s ) over 0..1 is 2/3, so a
	//flat pour and a varied one hold the same silver.
	float s = clamp( ( ( p.x - Origin.x ) * Drain.x + ( p.y - Origin.y ) * Drain.y ) * 0.5, 0.0, 1.0 );
	float profile = ( Perturb & 4 ) != 0 ? s - 0.5 : sqrt( s ) - 2.0 / 3.0;
	float h = 1.0 + Variation * profile;

	//The bare edge. Distance in from each side in units of the shorter
	//side, against an inset that wanders along the edge and is deeper at
	//the corners the pour is least likely to have reached.
	float shortSide = min( Size.x, Size.y );
	vec2 px = p * Size;
	float mask = 1.0;
	if( BareEdge > 0.0 )
	{
		float dLeft   = px.x / shortSide;
		float dRight  = ( Size.x - px.x ) / shortSide;
		float dTop    = px.y / shortSide;
		float dBottom = ( Size.y - px.y ) / shortSide;
		float iLeft   = BareEdge * ( 0.35 + 0.65 * edgeNoise( p.y, 1u ) );
		float iRight  = BareEdge * ( 0.35 + 0.65 * edgeNoise( p.y, 2u ) );
		float iTop    = BareEdge * ( 0.35 + 0.65 * edgeNoise( p.x, 3u ) );
		float iBottom = BareEdge * ( 0.35 + 0.65 * edgeNoise( p.x, 4u ) );
		float soft    = 0.25 * BareEdge + 1.5 / shortSide;
		mask *= smoothstep( iLeft - soft, iLeft + soft, dLeft );
		mask *= smoothstep( iRight - soft, iRight + soft, dRight );
		mask *= smoothstep( iTop - soft, iTop + soft, dTop );
		mask *= smoothstep( iBottom - soft, iBottom + soft, dBottom );
	}

	//Dust and comets. A speck is a disc the silver bath never wetted; a
	//comet is a speck with a tail running down the drain, where the bath
	//flowed round it and thinned. Positions are fractions of the plate, so
	//a plate looks the same at any raster; sizes are in pixels of the
	//shorter side so a speck stays a speck.
	float defect = 0.0;
	vec2 drainPx = normalize( Drain * Size );
	for( int i = 0; i < 48; ++i )
	{
		if( i >= DefectCount )
			break;
		uint base = uint( Seed ) * 7919u + uint( i ) * 4u + 17u;
		vec2 c    = vec2( hashUnit( hashInt( base ) ), hashUnit( hashInt( base + 1u ) ) );
		float r   = ( 1.2 + 3.0 * hashUnit( hashInt( base + 2u ) ) ) * shortSide / 540.0;
		float k   = hashUnit( hashInt( base + 3u ) );
		vec2 q    = px - c * Size;
		float d   = length( q );
		defect    = max( defect, 1.0 - smoothstep( r - 1.0, r + 1.0, d ) );
		if( k > 0.55 )
		{
			//A comet: the tail length is a fraction of the plate, its
			//width tapers from the speck's radius to nothing.
			float len = ( 0.04 + 0.10 * k ) * shortSide;
			float t   = dot( q, drainPx );
			float n   = length( q - t * drainPx );
			if( t > 0.0 && t < len )
			{
				float taper = 1.0 - t / len;
				float width = r * 1.6 * taper + 0.5;
				defect      = max( defect, 0.85 * taper * ( 1.0 - smoothstep( width - 1.0, width + 1.0, n ) ) );
			}
		}
	}

	fragColor = vec4( h * mask * ( 1.0 - defect ), 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 4: resample. One bucket into a bucket of another size.
//---------------------------------------------------------------------------
const char* const kResampleBody = R"(
uniform sampler2D Source;
uniform vec2 HalfTexel;  //half a SOURCE texel

in vec2 uv;
out vec4 fragColor;

void main()
{
	//Half a texel in from the edge so GL_LINEAR at the boundary does not
	//take half its weight from the clamp. On a flat field this is exact.
	vec2 q = clamp( uv, HalfTexel, vec2( 1.0 ) - HalfTexel );
	fragColor = vec4( texture( Source, q ).r, 0.0, 0.0, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 5: the plate, straight to the host.
//---------------------------------------------------------------------------
const char* const kPlateBody = R"(
uniform sampler2D Exposure;  //H, the window's mean plate exposure
uniform sampler2D Coating;   //h times the masks
uniform sampler2D Source;    //the host's input, for Mix
uniform vec2 MaxUV;
uniform float Speed;         //2 ^ Sensitivity stops
uniform float Toe;
uniform float Latitude;
uniform float Knee;
uniform float Fog;
uniform float Gamma;
uniform int Plate;           //0 tintype, 1 ambrotype, 2 negative
uniform vec3 SilverNeutral;  //linear RGB of the silver (or the light) at Tone 0
uniform vec3 SilverWarm;     //and at Tone 1
uniform vec3 PlateColour;    //what is behind the silver
uniform float Tone;
uniform float VignetteLoss;  //fraction of the light lost at the corner
uniform float MixAmount;
uniform int Probe;           //1: write ( H, coating, D, 1 ) raw

in vec2 uv;
out vec4 fragColor;

void main()
{
	ivec2 p = ivec2( gl_FragCoord.xy );
	float H = texelFetch( Exposure, p, 0 ).r;
	float h = texelFetch( Coating, p, 0 ).r;

	//The lens: light falls off toward the corners. A fraction of the
	//exposure, so it goes through the curve like any other exposure.
	vec2 fromCentre = uv * 2.0 - 1.0;
	float r2        = dot( fromCentre, fromCentre ) * 0.5;//1 at the corners
	float exposure  = H * Speed * ( 1.0 - VignetteLoss * r2 );

	float logH = log( max( exposure, 1e-12 ) ) / log( 10.0 );
	float c    = coverage( logH, Toe, Latitude, Knee );
	float D    = density( c, h, Fog, Gamma, Latitude );

	if( Probe == 1 )
	{
		fragColor = vec4( H, h, D, 1.0 );
		return;
	}

	//The silver intercepts a = 1 - 10^-D of the light.
	float a = 1.0 - exp2( -D * 3.321928094887362 );

	vec3 silver = mix( SilverNeutral, SilverWarm, Tone );
	vec3 linear;
	if( Plate == 2 )
		linear = silver * ( 1.0 - a );//a negative on a light box: transmits
	else
		linear = mix( PlateColour, silver, a );//silver by reflection over black

	//Alpha is 1 whatever the source's was: a plate is opaque. Resolume's
	//demo clips carry alpha, and an effect that passed it through would
	//come out transparent wherever the clip is.
	vec3 plate = srgbEncode( linear );
	if( MixAmount >= 1.0 )
	{
		fragColor = vec4( plate, 1.0 );
		return;
	}
	vec3 source = texture( Source, uv * MaxUV ).rgb;
	fragColor   = vec4( mix( source, plate, MixAmount ), 1.0 );
}
)";

std::string assemble( const char* body, bool withModel )
{
	std::string s = kVersion;
	if( withModel )
		s += kModel;
	s += body;
	return s;
}
} // namespace

std::string Vertex()
{
	return assemble( kVertexBody, false );
}
std::string Expose()
{
	return assemble( kExposeBody, true );
}
std::string Sum()
{
	return assemble( kSumBody, false );
}
std::string Coating()
{
	//The coating needs Perturb and nothing else from the model; the library
	//is small, and one library is worth more than a second declaration.
	return assemble( kCoatingBody, true );
}
std::string Resample()
{
	return assemble( kResampleBody, false );
}
std::string Plate()
{
	return assemble( kPlateBody, true );
}

} // namespace wetplate::shaders
