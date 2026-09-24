/**
 * Wetplate — browser demo.
 *
 * A collodion wet plate, blue-blind and seconds long. The one idea, from
 * `source/Model.h` and `AGENTS.md`: two properties of the plate make the look
 * and both are in the plate, not in a grade. It sees only ultraviolet and
 * blue, so reds and skin go dark and a blue sky blows to white; and it is
 * slow, so whatever moved is a ghost in proportion to how long it stayed.
 * Then it was hand-poured: thicker toward the drain corner, bare where the
 * pour never reached, marked by dust the silver bath ran round.
 *
 * This plugin is TEMPORAL. Unlike toner or repousse, its picture depends on
 * state carried across frames: a ring of `Buckets` R32F bucket sums, each
 * holding Σ( exposure × dt ) over the frames it took, with per-slot `start`
 * and `seconds` kept in double on the CPU; a take that integrates from an
 * event and then holds. So the two halves of this page are not equally
 * faithful, and the split is uglier here than on the siblings:
 *
 *   The shaders are the plugin's. The seven GLSL bodies below — the vertex
 *   shader, the `kModel` library (sRGB, Smits' rule, the curve) and the five
 *   pass bodies — are `kVertexBody`, `kModel`, `kExposeBody`, `kSumBody`,
 *   `kCoatingBody`, `kResampleBody` and `kPlateBody` from
 *   `source/Shaders.cpp`, copied across unedited and assembled the way the
 *   plugin assembles them (`assemble` below is Shaders.cpp's `assemble`:
 *   version + optional model + body). `demo/tools/check_shaders.py` compares
 *   all seven character for character and `tools/verify.sh` runs it.
 *
 *   ONE OF THEM DOES NOT RUN HERE. The sum pass indexes its sixteen samplers
 *   with a loop variable, `Bucket[ i ]`, which desktop GL 4.10 allows and
 *   GLSL ES 3.00 forbids ("array index for samplers must be constant integral
 *   expressions", section 4.1.7.1 of the ES spec). The kit's rule is that a
 *   shader which will not compile is SAID, not edited into something that
 *   does, so `SUM` is carried verbatim and checked, the page tries to compile
 *   it on every load and reports the compiler's refusal on the line under the
 *   canvas, and the window is summed by a pass of the page's own: the
 *   one-line `PAGE_SUM` shader drawn once per bucket, in the plugin's order,
 *   with GL_ONE / GL_ONE blending into the same R32F exposure buffer. That is
 *   the plugin's `h += texelFetch( Bucket[ i ] ).r * Weight[ i ]` as a
 *   sequential single-precision sum in the same order; what can differ is one
 *   rounding per term, where a GPU compiler may fuse the plugin's multiply-add
 *   and the blender rounds the product before the add. It is stated on the
 *   page.
 *
 *   The CPU half is a PORT — of `Controls.cpp` (every slider to its unit),
 *   the constants of `Model.h`, the seven weights per light in `Spectral.h`
 *   (copied as numbers, not recomputed), and `Wetplate::ProcessOpenGL`: the
 *   clock, the ring's bookkeeping (which cell a frame lands in, when the ring
 *   advances, what each slot holds in seconds), the window rule and its
 *   weights, the take's edge trigger, arming and end, the coating's cache key,
 *   the resample-and-swap on a resize, and the pass order — line for line, in
 *   JavaScript doubles as the C++ keeps them in double, rounded to float where
 *   the plugin hands a float uniform over. Nothing checks a port but a reader.
 *   `wttest --ghost`, `--bucket`, `--take`, `--resize` and the rest check the
 *   C++ originals and have no idea this page exists.
 *
 * ------------------------------------------------------------- the buffers
 *
 * The plugin's buckets are R32F with LINEAR filtering (read texel for texel by
 * texelFetch every frame, but between texels by the resample pass on a
 * resize); its exposure sum and coating R32F Nearest. The page allocates
 * exactly those through the kit's PassBuffer. Rendering into a float texture
 * is an extension in WebGL2 (EXT_color_buffer_float); ADDING into one by
 * blending — which is how the expose pass accumulates a frame into its bucket
 * — is another (EXT_float_blend); filtering one is a third
 * (OES_texture_float_linear). The page refuses to start without any of them
 * rather than render a plausible wrong plate.
 *
 * ------------------------------------------------------------- the clock
 *
 * The plugin reads the host's clock and votes on its unit (readout's scheme),
 * falling back to a wall clock for a host that never calls SetTime. Here the
 * clock is the kit's: `time` in declared seconds, accumulated from frame
 * deltas while playing, +1/60 on Step, 0 on Restart. No vote runs. The
 * plugin's own rules then apply unchanged: dt is now − last clamped to
 * [1/240, 1/4] s, a nominal 1/60 on the first frame; which bucket the frame
 * goes in is floor( t / width + 1e-9 ) on a grid anchored at 0 (Continuous)
 * or at the take. A paused page renders only when a control moves, and each
 * such frame is worth 1/240 s to the plate, as a paused host's is.
 *
 * ONE thing is the page's and not the plugin's: Restart. The kit's Restart
 * sends the clock to 0, and the plugin has no rule for a clock that runs
 * backwards — a slot that opened at 40 s satisfies `start ≥ now − Exposure`
 * for the next 40 s and the old exposure would linger on the plate. The
 * plugin resets its ring in InitGL, when the host instantiates it, and that
 * is what a backward jump is taken to mean here: the ring, the take and the
 * clock start again, as on a fresh instance. Said in the disclosure.
 *
 * ------------------------------------------------------------- what is missing
 *
 * **Take is a button under the canvas.** It is FF_TYPE_EVENT in the plugin
 * and the kit has no event control. A press is delivered as the plugin's
 * SetFloatParameter delivers it — 1.0 then 0.0, edge-triggered on the crossing
 * of 0.5 — and consumed on the next rendered frame. **Buckets is a
 * dropdown**: FF_TYPE_INTEGER 2..16 in the plugin, and the kit has no integer
 * control, so — as copperlist, galvo, teletext and toner did — it is a
 * dropdown of the fifteen values and the element index converts back. **The
 * About block is absent**, as on every page in this suite. The harness-only
 * `Perturb` and `Probe` uniforms are set to what the shipped plugin sets them
 * to: 0.
 *
 * And what every page in this suite is not: this is the plugin's shaders and a
 * port of its C++, not the plugin. No Resolume, no composition, no FFGL, and
 * GLSL ES 3.00 in a browser rather than desktop GL 4.1 core.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, GLError, bindTexture } from './vendor/gl.js';

//---------------------------------------------------------------------------
// Shaders — verbatim from source/Shaders.cpp. Do not edit here. None of the
// bodies carries a #version line: Shaders.cpp's `assemble` prepends
// "#version 410 core\n" and, for expose, coating and plate, the model library.
// `assemble` below is that function.
//---------------------------------------------------------------------------

const VERTEX_BODY = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const MODEL = `
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
`;

const EXPOSE_BODY = `
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
`;

const SUM_BODY = `
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
`;

const COATING_BODY = `
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
`;

const RESAMPLE_BODY = `
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
`;

const PLATE_BODY = `
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
`;

const assemble = (body, withModel) => '#version 410 core\n' + (withModel ? MODEL : '') + body;
const VERTEX = assemble(VERTEX_BODY, false);
const EXPOSE = assemble(EXPOSE_BODY, true);
const SUM = assemble(SUM_BODY, false);
const COATING = assemble(COATING_BODY, true);
const RESAMPLE = assemble(RESAMPLE_BODY, false);
const PLATE = assemble(PLATE_BODY, true);

//---------------------------------------------------------------------------
// The page's own sum pass. NOT the plugin's shader: see the header. One
// bucket times its weight, drawn once per bucket with GL_ONE / GL_ONE into
// the exposure buffer, so the buffer ends up holding the same sequential
// single-precision sum the plugin's `kSumBody` computes in a register.
//---------------------------------------------------------------------------
const PAGE_SUM = `#version 410 core
uniform sampler2D Bucket;
uniform float Weight;  //1 / seconds in the window, or 0

in vec2 uv;
out vec4 fragColor;

void main()
{
	ivec2 p   = ivec2( gl_FragCoord.xy );
	fragColor = vec4( texelFetch( Bucket, p, 0 ).r * Weight, 0.0, 0.0, 1.0 );
}
`;

//===========================================================================
// Model.h, ported. The plate as numbers.
//===========================================================================

/// log10 H where the straight line meets the fog level, with white at 0.
const K_TOE = -1.1;
/// log10 H from the toe to the shoulder.
const K_LATITUDE = 1.2;
/// How sharply the toe and the shoulder bend, per log10 unit.
const K_KNEE = 6.0;

/// The buckets: at most this many, whatever `Buckets` asks for.
const K_MAX_BUCKETS = 16;

/// What a frame is worth in seconds when the host's clock has not moved yet,
/// or has moved implausibly.
const K_NOMINAL_FRAME = 1.0 / 60.0;
const K_MIN_FRAME_DELTA = 1.0 / 240.0;
const K_MAX_FRAME_DELTA = 0.25;

/// Presentation colours, linear RGB. Neutral and warm ends of the silver, the
/// plate behind it, and the light behind a negative.
const K_TINTYPE_SILVER_NEUTRAL = [0.80, 0.80, 0.80];
const K_TINTYPE_SILVER_WARM = [0.86, 0.72, 0.52];
const K_TINTYPE_PLATE = [0.020, 0.017, 0.014];
const K_AMBROTYPE_SILVER_NEUTRAL = [0.84, 0.86, 0.90];
const K_AMBROTYPE_SILVER_WARM = [0.88, 0.80, 0.66];
const K_AMBROTYPE_PLATE = [0.012, 0.012, 0.018];
const K_NEGATIVE_LIGHT_NEUTRAL = [1.0, 1.0, 1.0];
const K_NEGATIVE_LIGHT_WARM = [1.0, 0.90, 0.72];

/// The harness-only hooks, at what the shipped plugin sets them to.
const PERTURB = 0;
const PROBE = 0;

/// Wetplate.cpp's kGridSlack: a tiny slack on a grid comparison, so a frame
/// that lands on a cell boundary to within double rounding is counted on the
/// side it means.
const K_GRID_SLACK = 1e-9;

//===========================================================================
// Spectral.h, copied. The plate exposure of each of Smits' seven basis
// reflectances under each light, white normalised to exactly 1. These are the
// numbers the plugin compiles, as `tools/spectral_weights.py` wrote them --
// copied, not recomputed. Order: white, cyan, magenta, yellow, red, green,
// blue.
//===========================================================================

const SPECTRAL_WEIGHTS = [
  { light: 'Daylight', kelvin: 6504.0, w: [1.0, 0.963185049, 0.98380056, 0.0198556533, 0.073701063, 0.0162116239, 0.9802007] },
  { light: 'Tungsten', kelvin: 2856.0, w: [1.0, 0.964396771, 0.956788309, 0.0483211701, 0.0609575768, 0.043508591, 0.951691792] },
  { light: 'Flash', kelvin: 5500.0, w: [1.0, 0.963184111, 0.98125561, 0.022650139, 0.0720941419, 0.0187768184, 0.977402235] },
];

//===========================================================================
// Controls.cpp, ported. What a host parameter means.
//
// The plugin stores every host value as a `float` and each law takes that
// float; the float laws compute in float (`1.2f * std::exp2( 2.0f * v - 1.0f )`)
// and the one double law, ExposureSeconds, widens the float first. The
// page's values are doubles from a slider, so each is rounded through
// Math.fround -- the same 24-bit value the plugin holds -- and the float laws
// round every step through fround too. exp2 is Math.pow( 2, x ) in double
// rounded to float, which can differ from a float exp2 by an ULP; nothing on
// the page depends on that ULP.
//===========================================================================

const f = Math.fround;
const clamp01f = (value) => Math.min(Math.max(f(value), 0.0), 1.0);
const lround = (value) => (value < 0 ? -Math.round(-value) : Math.round(value));

/// Exposure: the window the plate integrates, 1/16 s to 16 s, geometric, in
/// DOUBLE. 0.5 is exactly 1 s, and every multiple of 1/8 is a power of two
/// exactly: 0.375 is exactly 0.5 s.
const exposureSeconds = (value) => Math.pow(2.0, 8.0 * clamp01f(value) - 4.0);
const exposureParam = (seconds) => clamp01f((Math.log2(Math.max(seconds, 0.0625)) + 4.0) / 8.0);

/// Sensitivity: -3 to +3 stops on the plate's exposure, linear. 0.5 is 0.
const sensitivityStops = (value) => f(f(clamp01f(value) * 6.0) - 3.0);
const sensitivityParam = (stops) => clamp01f((stops + 3.0) / 6.0);

/// Development: the straight line's slope, density per log10 H. 0.5 is the
/// plate's stated gamma of 1.2; 0 is 0.6 and 1 is 2.4, geometric.
const developmentGamma = (value) => f(1.2 * f(Math.pow(2.0, f(f(2.0 * clamp01f(value)) - 1.0))));

/// Development also lifts the fog: 0.004 density at 0, 0.030 at 1, linear.
const developmentFog = (value) => f(0.004 + f(0.026 * clamp01f(value)));

/// Coating Variation: V in h( s ) = 1 + V ( sqrt( s ) - 2/3 ), 0 to 1.2, linear.
const coatingVariation = (value) => f(1.2 * clamp01f(value));

/// Bare Edge: how far in from the edge the pour never reached, as a fraction
/// of the shorter side, 0 to 0.12, linear. 0 is exactly none.
const bareEdgeFraction = (value) => f(0.12 * clamp01f(value));

/// Defects: how many dust specks and comets, 0 to 48, linear, rounded.
const defectCount = (value) => lround(f(48.0 * clamp01f(value)));

/// Tone: 0 is neutral silver on the plate, 1 is fully warm, linear.
const toneOf = (value) => clamp01f(value);

/// Vignette: exposure falloff at the corner, 0 to 0.85 of the light lost.
const vignetteLoss = (value) => f(0.85 * clamp01f(value));

/// Option counts, and names in their menu order.
const PLATE_NAMES = ['Tintype', 'Ambrotype', 'Negative'];
const LIGHT_NAMES = ['Daylight', 'Tungsten', 'Flash'];
const MODE_NAMES = ['Continuous', 'Take'];
const POUR_NAMES = ['Top Left', 'Top Right', 'Bottom Left', 'Bottom Right'];

/// Buckets: the ring's length, a real integer.
const K_MIN_BUCKETS = 2;
const K_MAX_BUCKETS_CONTROL = 16;

/// An option's value to its index, rounded and clamped.
const optionIndex = (value, count) => Math.min(Math.max(lround(f(value)), 0), count - 1);

/// The pour corner, in a frame whose x runs right and y runs DOWN.
function pourOrigin(pour) {
  switch (Math.min(Math.max(pour, 0), POUR_NAMES.length - 1)) {
    default:
    case 0: return [0.0, 0.0];
    case 1: return [1.0, 0.0];
    case 2: return [0.0, 1.0];
    case 3: return [1.0, 1.0];
  }
}

/// From the pour corner to the opposite one, in a y-down frame.
function pourDirection(pour) {
  const [ox, oy] = pourOrigin(pour);
  return [ox > 0.5 ? -1.0 : 1.0, oy > 0.5 ? -1.0 : 1.0];
}

//===========================================================================
// The renderer: Wetplate::InitGL and Wetplate::ProcessOpenGL, in their order.
// The clock and the ring's bookkeeping in double before any GL call; then the
// buffers (every allocation before anything binds a texture); then expose,
// sum, coating (cached), plate.
//===========================================================================

/// What the line under the canvas reports. Filled by the renderer.
const telemetry = {
  ticked: false,
  now: 0, dt: 0, cell: 0, current: -1,
  bucketCount: 0, held: 0, windowSeconds: 0, exposureSeconds: 0, bucketSeconds: 0,
  mode: 0, takeArmed: false, takeExposing: false, takeCount: 0,
  accumulate: true, width: 0, height: 0,
  restarts: 0,
  sumVerdict: '',
};

/// The Take event, module-level so the button under the canvas can reach it.
/// `takeWas` and `takePending` are Wetplate's members of the same names.
const take = {
  takePending: false,
  takeWas: 0.0,
  /// Wetplate::SetFloatParameter for PT_TAKE: an event arrives as 1.0 on
  /// press and 0.0 on release; the take is the press. Edge-triggered, so a
  /// host restating 1.0 does not fire it again.
  set(value) {
    value = f(value);
    if (value >= 0.5 && this.takeWas < 0.5) this.takePending = true;
    this.takeWas = value;
  },
};

function createRenderer(gl, quad) {
  // The three extensions the plate cannot do without. The kit checks the
  // first two when asked (needFloat, needFloatBlend); the third is asked for
  // here because the buckets are filtered only on a resize, and a plate that
  // resized fine on one machine and went black on another would be exactly
  // the failure nobody notices.
  if (!gl.getExtension('OES_texture_float_linear')) {
    throw new GLError('OES_texture_float_linear is missing. The plugin resamples its float buckets with linear filtering when the raster changes, and without it that read would silently return zero.');
  }

  const program = (fragment, label) => new Program(gl, VERTEX, fragment, label);
  const exposeShader = program(EXPOSE, 'expose');
  const coatingShader = program(COATING, 'coating');
  const resampleShader = program(RESAMPLE, 'resample');
  const plateShader = program(PLATE, 'plate');
  const pageSumShader = program(PAGE_SUM, 'page-sum');

  // The plugin's own sum shader, tried and reported, never used: see the
  // header. If a browser ever accepts it the page still runs its own pass,
  // so the picture does not depend on which browser is looking.
  try {
    program(SUM, 'sum').dispose();
    telemetry.sumVerdict = 'this browser accepted the plugin’s sum shader (the page still sums with its own pass)';
  } catch (error) {
    const first = String(error.message).split('\n').find((line) => line.startsWith('ERROR')) ?? error.message;
    telemetry.sumVerdict = `the plugin’s sum shader is refused by this browser’s compiler — ${first.trim()}`;
  }

  // The buffers, as the plugin's PassBuffer::Ensure( …, GL_R32F, sampling ):
  // buckets and scratch Linear, exposure and coating Nearest.
  const linear = { filter: 'linear' };
  const nearest = { filter: 'nearest' };
  const buckets = [];
  for (let i = 0; i < K_MAX_BUCKETS; i += 1) buckets.push(new PassBuffer(gl, linear));
  let scratch = new PassBuffer(gl, linear);
  const exposure = new PassBuffer(gl, nearest);
  const coating = new PassBuffer(gl, nearest);

  // PassBuffer::Ensure allocates AND clears; the kit's ensure() only
  // allocates (WebGL zero-fills new storage, but the plugin does not rely on
  // that and neither does this). Returns whether it reallocated.
  const ensure = (buffer, width, height) => {
    const was = buffer.texture !== null && buffer.width === width && buffer.height === height && buffer.internalFormat === gl.R32F;
    buffer.ensure(width, height, gl.R32F);
    if (!was) buffer.clearTo(0.0, 0.0, 0.0, 0.0);
    return !was;
  };
  const isValid = (buffer) => buffer.texture !== null;

  //--- Wetplate's members ---------------------------------------------------
  const ring = [];
  for (let i = 0; i < K_MAX_BUCKETS; i += 1) ring.push({ start: -1.0, seconds: 0.0 });
  const needClear = new Array(K_MAX_BUCKETS).fill(false);
  let bucketCount = 0;
  let current = -1;
  let cell = -1;

  let modeWas = -1;
  let takeArmed = false;
  let takeExposing = false;
  let takeStart = 0.0;

  let lastWidth = 0;
  let lastHeight = 0;
  let coatingKey = '';

  let lastNow = -1.0;

  const weights = new Float32Array(K_MAX_BUCKETS);
  const basis = new Float32Array(7);

  /// Wetplate::resetRing: every bucket in use is cleared on the GPU and
  /// forgets its seconds. Called before anything binds a texture.
  const resetRing = () => {
    for (let i = 0; i < K_MAX_BUCKETS; i += 1) {
      ring[i] = { start: -1.0, seconds: 0.0 };
      needClear[i] = true;
    }
    current = -1;
    cell = -1;
  };

  /// Wetplate::InitGL's state reset (the GL objects are made above, once).
  const initGL = () => {
    bucketCount = 0;
    current = -1;
    cell = -1;
    for (let i = 0; i < K_MAX_BUCKETS; i += 1) ring[i] = { start: -1.0, seconds: 0.0 };
    // takePending is deliberately left alone: a press that arrived before the
    // GL context existed is still a press.
    modeWas = -1;
    takeArmed = false;
    takeExposing = false;
    lastWidth = lastHeight = 0;
    coatingKey = '';
  };
  initGL();

  /// Wetplate::rescaleBuckets: carry every bucket across to a new raster,
  /// resampled, instead of letting a reallocation clear it. The C++ swaps the
  /// GL ids behind two PassBuffers; here the two objects change places.
  const rescaleBuckets = (width, height) => {
    // Every allocation first, then the passes: allocating unbinds the active
    // texture unit, and a resample that ran between the two would read
    // nothing on the frame that mattered.
    ensure(scratch, width, height);

    for (let i = 0; i < bucketCount; i += 1) {
      const bucket = buckets[i];
      if (!isValid(bucket) || (bucket.width === width && bucket.height === height)) continue;
      if (ring[i].start < 0.0) continue; // empty: a fresh allocation is the right answer, below

      scratch.bind();
      resampleShader.use();
      bindTexture(gl, 0, bucket.texture);
      resampleShader.setSampler('Source', 0);
      resampleShader.set('HalfTexel', f(0.5 / bucket.width), f(0.5 / bucket.height));
      gl.disable(gl.BLEND);
      quad.draw();
      bindTexture(gl, 0, null);

      buckets[i] = scratch;
      scratch = bucket;
      // The old bucket is now `scratch`, at the old size; the next ensure
      // reallocates it at the new size for the next bucket. That
      // reallocation clears, which is fine: it is scratch.
      ensure(scratch, width, height);
    }
  };

  return {
    render({ input, params, width: vpW, height: vpH, time }) {
      const p = (id) => params.get(id);
      const width = input.width;
      const height = input.height;

      //------------------------------------------------------------------
      // The clock. Everything that reads it -- which bucket a frame goes
      // in, what a frame is worth, whether a take is over -- is reduced in
      // double here; nothing absolute crosses into GLSL.
      //
      // The page's one addition: the kit's Restart sends its clock to 0,
      // which the plugin (whose host clock never runs backwards) has no rule
      // for. A backward jump is taken as a fresh instance: InitGL's reset.
      //------------------------------------------------------------------
      if (lastNow >= 0.0 && time < lastNow) {
        initGL();
        lastNow = -1.0;
        telemetry.restarts += 1;
      }
      const now = time;
      let dt = K_NOMINAL_FRAME;
      if (lastNow >= 0.0) dt = Math.min(Math.max(now - lastNow, K_MIN_FRAME_DELTA), K_MAX_FRAME_DELTA);
      lastNow = now;

      //------------------------------------------------------------------
      // What the controls say.
      //------------------------------------------------------------------
      const plate = optionIndex(p('plate'), PLATE_NAMES.length);
      const light = optionIndex(p('light'), LIGHT_NAMES.length);
      const gamma = developmentGamma(p('development'));
      const fog = developmentFog(p('development'));
      const speed = f(Math.pow(2.0, sensitivityStops(p('sensitivity'))));
      const exposureS = exposureSeconds(p('exposure'));
      const mode = optionIndex(p('mode'), MODE_NAMES.length);
      const wantBuckets = Math.min(Math.max(lround(f(integerValue('buckets', p('buckets')))), K_MIN_BUCKETS), K_MAX_BUCKETS_CONTROL);
      const pour = optionIndex(p('pour'), POUR_NAMES.length);
      const variation = coatingVariation(p('coatingVar'));
      const bareEdge = bareEdgeFraction(p('bareEdge'));
      const defects = defectCount(p('defects'));
      const tone = toneOf(p('tone'));
      const vignette = vignetteLoss(p('vignette'));
      const mixAmount = Math.min(Math.max(f(p('mix')), 0.0), 1.0);

      //------------------------------------------------------------------
      // The ring's bookkeeping, all of it before any GL call, so that every
      // clear it decides on happens before anything binds a texture.
      //
      // Continuous: the window is the last Exposure seconds, held as
      // Buckets bucket sums on a grid of Exposure / Buckets seconds anchored
      // at host time 0. A bucket is in the window while it opened no
      // earlier than Exposure seconds ago, which in steady state is all of
      // them. Take: the grid is anchored at the take, frames go in until
      // Exposure seconds have passed, and then the plate is developed and
      // held.
      //------------------------------------------------------------------
      if (wantBuckets !== bucketCount) {
        // A structural change: start again. Slots past the new count are
        // released below.
        resetRing();
        bucketCount = wantBuckets;
      }
      if (mode !== modeWas) {
        // Switching to Take caps the lens; switching back opens it again.
        resetRing();
        takeArmed = false;
        takeExposing = false;
        modeWas = mode;
      }
      if (mode === 1 && take.takePending) {
        resetRing();
        takeStart = now;
        takeArmed = true;
        takeExposing = true;
        telemetry.takeCount += 1;
      }
      take.takePending = false;

      const bucketSeconds = exposureS / bucketCount;
      let origin = 0.0;
      let accumulate = true;
      if (mode === 1) {
        origin = takeStart;
        if (takeExposing && now - takeStart >= exposureS - K_GRID_SLACK && (PERTURB & 32) === 0) takeExposing = false;
        accumulate = takeArmed && takeExposing;
      }

      const oneFrame = (PERTURB & 2) !== 0;
      if (accumulate) {
        const wantCell = Math.floor((now - origin) / bucketSeconds + K_GRID_SLACK);
        if (wantCell !== cell || current < 0 || oneFrame) {
          current = (current + 1) % bucketCount;
          needClear[current] = true;
          ring[current] = { start: -1.0, seconds: 0.0 };
          ring[current].start = oneFrame ? now : origin + wantCell * bucketSeconds;
          cell = wantCell;
        }
        ring[current].seconds += dt;
      }

      // The window's weights: 1 / seconds for a bucket in it, 0 otherwise.
      let windowSeconds = 0.0;
      const inWindow = new Array(K_MAX_BUCKETS).fill(false);
      let held = 0;
      for (let i = 0; i < bucketCount; i += 1) {
        const b = ring[i];
        if (b.start < 0.0 || b.seconds <= 0.0) continue;
        if (oneFrame) inWindow[i] = i === current;
        else if (mode === 1) inWindow[i] = true;
        else inWindow[i] = b.start >= now - exposureS - K_GRID_SLACK;
        if (inWindow[i]) {
          windowSeconds += b.seconds;
          held += 1;
        }
      }
      weights.fill(0.0);
      for (let i = 0; i < bucketCount; i += 1) {
        weights[i] = inWindow[i] && windowSeconds > 0.0 ? f(1.0 / windowSeconds) : 0.0;
      }

      //------------------------------------------------------------------
      // Buffers. Every allocation happens here, before anything binds a
      // texture. A resize carries the buckets across, resampled: the
      // photofinish trap, guarded by --resize in the plugin's harness.
      //------------------------------------------------------------------
      const resized = lastWidth !== 0 && (lastWidth !== width || lastHeight !== height);
      if (resized && (PERTURB & 16) === 0) rescaleBuckets(width, height);
      lastWidth = width;
      lastHeight = height;

      for (let i = 0; i < K_MAX_BUCKETS; i += 1) {
        if (i >= bucketCount) {
          buckets[i].dispose();
          continue;
        }
        const was = isValid(buckets[i]) && buckets[i].width === width && buckets[i].height === height;
        ensure(buckets[i], width, height);
        if (!was) needClear[i] = false; // freshly allocated, so already cleared
      }
      ensure(exposure, width, height);
      ensure(coating, width, height);

      for (let i = 0; i < bucketCount; i += 1) {
        if (needClear[i]) {
          buckets[i].clearTo(0.0, 0.0, 0.0, 0.0);
          needClear[i] = false;
        }
      }

      //------------------------------------------------------------------
      // 1. Expose: this frame into the current bucket, added.
      //------------------------------------------------------------------
      if (accumulate) {
        const w = SPECTRAL_WEIGHTS[light].w;
        for (let i = 0; i < 7; i += 1) basis[i] = w[i];
        buckets[current].bind();
        exposeShader.use();
        bindTexture(gl, 0, input.texture);
        exposeShader.setSampler('InputTexture', 0);
        exposeShader.setArray('Basis', basis, 1);
        exposeShader.set('Dt', f(dt));
        exposeShader.setInt('Perturb', PERTURB);

        gl.enable(gl.BLEND);
        gl.blendEquation(gl.FUNC_ADD);
        gl.blendFunc(gl.ONE, gl.ONE);
        quad.draw();
        gl.disable(gl.BLEND);
        bindTexture(gl, 0, null);
      }

      //------------------------------------------------------------------
      // 2. Sum: the window, weighted, to H. The PAGE's pass, not the
      // plugin's (see the header): the exposure buffer cleared, then bucket
      // i times Weight[ i ] added for i = 0 .. Count - 1, in that order,
      // which is the plugin's loop with the accumulator in the blender.
      //------------------------------------------------------------------
      exposure.clearTo(0.0, 0.0, 0.0, 0.0);
      pageSumShader.use();
      pageSumShader.setSampler('Bucket', 0);
      gl.enable(gl.BLEND);
      gl.blendEquation(gl.FUNC_ADD);
      gl.blendFunc(gl.ONE, gl.ONE);
      for (let i = 0; i < bucketCount; i += 1) {
        bindTexture(gl, 0, buckets[i].texture);
        pageSumShader.set('Weight', weights[i]);
        quad.draw();
      }
      gl.disable(gl.BLEND);
      bindTexture(gl, 0, null);

      //------------------------------------------------------------------
      // 3. The coating, only when something it depends on has moved. The
      // key's spelling is JavaScript's, not std::to_string's; its job -- to
      // change when and only when an input changes -- is the same.
      //------------------------------------------------------------------
      {
        const key = `${width}x${height}/${pour}/${variation}/${bareEdge}/${defects}/${PERTURB}`;
        if (key !== coatingKey) {
          const [ox, oy] = pourOrigin(pour);
          const [dx, dy] = pourDirection(pour);

          coating.bind();
          coatingShader.use();
          coatingShader.set('Origin', ox, oy);
          coatingShader.set('Drain', dx, dy);
          coatingShader.set('Variation', variation);
          coatingShader.set('BareEdge', bareEdge);
          coatingShader.set('Size', f(width), f(height));
          coatingShader.setInt('DefectCount', defects);
          coatingShader.setInt('Seed', 1);
          coatingShader.setInt('Perturb', PERTURB);
          gl.disable(gl.BLEND);
          quad.draw();
          coatingKey = key;
        }
      }

      //------------------------------------------------------------------
      // 4. The plate, straight to the host. The host's viewport is the whole
      // canvas, and the input texture is exactly its raster, so MaxUV is 1.
      //------------------------------------------------------------------
      {
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, vpW, vpH);

        plateShader.use();
        bindTexture(gl, 0, exposure.texture);
        bindTexture(gl, 1, coating.texture);
        bindTexture(gl, 2, input.texture);

        const silverNeutral = plate === 0 ? K_TINTYPE_SILVER_NEUTRAL : plate === 1 ? K_AMBROTYPE_SILVER_NEUTRAL : K_NEGATIVE_LIGHT_NEUTRAL;
        const silverWarm = plate === 0 ? K_TINTYPE_SILVER_WARM : plate === 1 ? K_AMBROTYPE_SILVER_WARM : K_NEGATIVE_LIGHT_WARM;
        const plateColour = plate === 0 ? K_TINTYPE_PLATE : plate === 1 ? K_AMBROTYPE_PLATE : K_NEGATIVE_LIGHT_NEUTRAL;

        plateShader.setSampler('Exposure', 0);
        plateShader.setSampler('Coating', 1);
        plateShader.setSampler('Source', 2);
        plateShader.set('MaxUV', 1.0, 1.0);
        plateShader.set('Speed', speed);
        plateShader.set('Toe', f(K_TOE));
        plateShader.set('Latitude', f(K_LATITUDE));
        plateShader.set('Knee', f(K_KNEE));
        plateShader.set('Fog', fog);
        plateShader.set('Gamma', gamma);
        plateShader.setInt('Plate', plate);
        plateShader.set('SilverNeutral', silverNeutral[0], silverNeutral[1], silverNeutral[2]);
        plateShader.set('SilverWarm', silverWarm[0], silverWarm[1], silverWarm[2]);
        plateShader.set('PlateColour', plateColour[0], plateColour[1], plateColour[2]);
        plateShader.set('Tone', tone);
        plateShader.set('VignetteLoss', vignette);
        plateShader.set('MixAmount', mixAmount);
        plateShader.setInt('Probe', PROBE);
        plateShader.setInt('Perturb', PERTURB);
        gl.disable(gl.BLEND);
        quad.draw();

        // Unbind so nothing reads a framebuffer's own texture next frame.
        bindTexture(gl, 2, null);
        bindTexture(gl, 1, null);
        bindTexture(gl, 0, null);
      }

      telemetry.ticked = true;
      telemetry.now = now;
      telemetry.dt = dt;
      telemetry.cell = cell;
      telemetry.current = current;
      telemetry.bucketCount = bucketCount;
      telemetry.held = held;
      telemetry.windowSeconds = windowSeconds;
      telemetry.exposureSeconds = exposureS;
      telemetry.bucketSeconds = bucketSeconds;
      telemetry.mode = mode;
      telemetry.takeArmed = takeArmed;
      telemetry.takeExposing = takeExposing;
      telemetry.accumulate = accumulate;
      telemetry.width = width;
      telemetry.height = height;
    },
  };
}

//===========================================================================
// The controls, read out of Wetplate::Wetplate(). Same names, same groups,
// same order, same defaults, same dropdown elements. Absent: Take, which is
// the button under the canvas (FF_TYPE_EVENT), and the About block.
//===========================================================================

/// FF_TYPE_INTEGER is exempt from the 0..1 clamp, so the plugin stores
/// Buckets as the integer itself, 2..16. The kit has no integer control, so
/// -- as copperlist, galvo, teletext and toner did -- it is a dropdown of every
/// value in the plugin's range; `integerValue` turns the dropdown's index
/// back into the integer.
const INTEGER_RANGES = {
  buckets: [K_MIN_BUCKETS, K_MAX_BUCKETS_CONTROL],
};
const INTEGER_ELEMENTS = {};
for (const [id, [low, high]] of Object.entries(INTEGER_RANGES)) {
  INTEGER_ELEMENTS[id] = [];
  for (let v = low; v <= high; v += 1) INTEGER_ELEMENTS[id].push(String(v));
}
function integerValue(id, index) {
  const [low, high] = INTEGER_RANGES[id];
  return Math.min(high, Math.max(low, low + Math.round(index)));
}
const integerIndex = (id, value) => value - INTEGER_RANGES[id][0];

const integer = (id, name, value, group, hint) => ({ id, name, type: 'option', elements: INTEGER_ELEMENTS[id], default: integerIndex(id, value), group, hint });
const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });

const secondsText = (s) => (s >= 1 ? `${s % 1 === 0 ? s.toFixed(0) : s.toFixed(2)} s` : `1/${(1 / s).toFixed(0)} s`);

const demo = mountDemo({
  name: 'Wetplate',
  pluginId: 'WT01',
  kind: 'effect',
  tagline:
    'A collodion wet plate, blue-blind and seconds long. The clip exposes a plate that sees only blue and ultraviolet — reds and skin go dark, skies go white, from a sensitivity curve integrated against each pixel’s spectrum — over a sliding window of seconds held as a ring of bucket sums, so whatever moved is a ghost with density exactly the fraction of the window it stayed. The plate is hand-poured: thicker toward the drain corner, bare where the pour never reached, marked by dust the silver bath ran round; shown as a tintype, an ambrotype or a glass negative. Set Mode to Take and press Take under the picture for a capped lens and one developed plate. The shaders here are the plugin’s own; the ring, the window and the control laws are a port of its C++.',
  repo: 'https://github.com/stoatworks-labs/wetplate',
  page: 'https://stoatworks-labs.com/software/wetplate/',

  // The stock sentence says "same maths", which is only most of the truth
  // here: the shaders are the plugin's, the state they run on is a port, and
  // one pass is the page's because the plugin's will not compile here.
  blurb:
    'It is Wetplate’s own GLSL — the expose, coating, resample and plate passes and the model library they share — ported from the repository to WebGL2, with the CPU half (the clock, the ring of bucket sums and its window, the take, every control’s law and the plate’s spectral weights) ported to JavaScript by hand; nothing checks that port but a reader. The plugin’s sum pass will not compile in a browser’s GLSL, so the window is summed by a pass of this page’s own — said below. It runs on generated clips in this page, with the plugin’s own parameters and no install.',

  // Every buffer is R32F, as in the plugin, and the expose pass ADDS into
  // its bucket by blending: an exposure quantised to 8 bits, or a frame that
  // overwrote its bucket instead of adding, would be a plausible wrong plate.
  needFloat: true,
  needFloatBlend: true,

  params: [
    opt('plate', 'Plate', PLATE_NAMES, 0, 'Plate',
      'What the silver is seen on: a tintype (silver over black japanned iron, warm), an ambrotype (silver over black glass, cooler) or a glass negative on a light box.'),
    opt('light', 'Light', LIGHT_NAMES, 0, 'Plate',
      'The light the plate is exposed under: Planckian at 6504 K, 2856 K or 5500 K. It changes the balance between colours, never the overall exposure — white exposes to exactly 1 under each.'),
    std('development', 'Development', 0.5, 'Plate', {
      display: (v) => `γ ${developmentGamma(v).toFixed(2)}, fog ${developmentFog(v).toFixed(3)}`,
      hint: 'The characteristic curve’s slope, density per log10 exposure: 0.6 at 0, the plate’s stated 1.2 at the default, 2.4 at 1, geometric. Development also lifts the fog, 0.004 to 0.030 density.',
    }),
    std('sensitivity', 'Sensitivity', sensitivityParam(0.0), 'Plate', {
      display: (v) => `${sensitivityStops(v) >= 0 ? '+' : ''}${sensitivityStops(v).toFixed(2)} stops`,
      hint: '−3 to +3 stops on the plate’s exposure, linear; 0.5 is exactly 0. Where white lands on the curve: at 0 stops a daylight white sits on the shoulder, a red at 7% of white below the toe.',
    }),

    std('exposure', 'Exposure', exposureParam(0.5), 'Exposure', {
      display: (v) => secondsText(exposureSeconds(v)),
      hint: 'The window the plate integrates, 1/16 s to 16 s, geometric: every multiple of 1/8 on the slider is a power of two of seconds exactly (0.375 is 0.5 s, 0.5 is 1 s). A change keeps the ring: the grid changes width under the old slots, which fall out of the window by the same rule.',
    }),
    opt('mode', 'Mode', MODE_NAMES, 0, 'Exposure',
      'Continuous: a sliding window of the last Exposure seconds. Take: the lens is capped until the Take button is pressed, the plate exposes for Exposure seconds from the press, and then it is developed and held. Switching resets the ring.'),
    integer('buckets', 'Buckets', 8, 'Exposure',
      'How many bucket sums the window is held in, 2 to 16: the window is exact to one bucket’s width and no better, and memory is this many frames whatever the exposure. FF_TYPE_INTEGER in the plugin; a dropdown of the same fifteen values here. A change resets the ring.'),

    opt('pour', 'Pour', POUR_NAMES, 0, 'Coating',
      'The corner the collodion was poured from. The coating drains toward the opposite corner, and the comets’ tails run that way.'),
    std('coatingVar', 'Coating Var', 0.35, 'Coating', {
      display: (v) => `V ${coatingVariation(v).toFixed(2)}: ${(1 - 2 * coatingVariation(v) / 3).toFixed(2)} to ${(1 + coatingVariation(v) / 3).toFixed(2)}`,
      hint: 'V in h = 1 + V( √s − 2/3 ) along the drain, 0 to 1.2: thin-film drainage, thickness growing as the square root of the distance drained past. The mean is 1 whatever V, so a flat pour and a varied one hold the same silver.',
    }),
    std('bareEdge', 'Bare Edge', 0.3, 'Coating', {
      display: (v) => `${(100 * bareEdgeFraction(v)).toFixed(1)}% of the short side`,
      hint: 'How far in from each side the pour never reached, as a fraction of the shorter side, 0 to 12%, wandering along the edge from a seeded value noise. 0 is exactly none.',
    }),
    std('defects', 'Defects', 0.25, 'Coating', {
      display: (v) => `${defectCount(v)} specks and comets`,
      hint: 'How many dust specks and comets, 0 to 48. A speck is a disc the silver bath never wetted; a comet is a speck with a tail down the drain. Seeded at 1; there is no seed control.',
    }),

    std('tone', 'Tone', 0.45, 'Output', {
      hint: 'The silver (or the negative’s light) from neutral at 0 to fully warm at 1, linear.',
    }),
    std('vignette', 'Vignette', 0.35, 'Output', {
      display: (v) => `${(100 * vignetteLoss(v)).toFixed(0)}% lost at the corner`,
      hint: 'The lens: exposure falloff toward the corners, 0 to 85% of the light lost at the corner. A fraction of the exposure, so it goes through the curve like any other exposure.',
    }),
    std('mix', 'Mix', 1.0, 'Output', {
      hint: 'The plate against the input. Alpha is 1 whatever the source’s was, including under Mix: a plate is opaque.',
    }),
  ],

  // The scene and the lights move, which is what a slow plate shows; the bars
  // are the spectral claim you can read off (red dark, green darker, blue
  // white); the alpha clip shows the opaque plate.
  sources: ['scene', 'spot', 'bars', 'grid', 'alpha', 'ramp', 'detail'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    'Two seconds': { exposure: exposureParam(2.0) },
    'Sixteen seconds': { exposure: exposureParam(16.0) },
    'Two buckets': { buckets: integerIndex('buckets', 2) },
    'Ambrotype': { plate: 1 },
    'Glass negative': { plate: 2, tone: 0.2 },
    'Tungsten': { light: 1 },
    'Take mode (then press Take)': { mode: 1, exposure: exposureParam(2.0) },
    'Clean plate': { coatingVar: 0, bareEdge: 0, defects: 0, vignette: 0 },
    'Heavy pour, bottom right': { pour: 3, coatingVar: 1.0, bareEdge: 0.6, defects: 0.8 },
    'Overexposed, hard': { sensitivity: sensitivityParam(1.5), development: 0.85 },
  },

  differences: [
    'The CPU half of this plugin is a PORT, not the plugin’s own code. Wetplate keeps its state across frames in C++: a ring of Buckets bucket sums with each slot’s start and seconds in double; which grid cell a frame lands in and when the ring advances; the window rule and its sixteen weights; the take’s edge trigger, arming and end; the coating’s cache key; the resample-and-swap that carries the buckets across a resize; and every slider’s law in Controls.cpp, with the seven spectral weights per light copied as numbers from Spectral.h rather than recomputed. All of that is ported here line for line, in JavaScript doubles as the plugin keeps them, rounded to float where the plugin hands a float uniform over. Nothing checks a port but a reader; the repository’s wttest --ghost, --bucket, --take and --resize check the C++ against the model’s statement and have never heard of this page.',
    'One of the plugin’s six shaders does not run here. The sum pass indexes its sixteen bucket samplers with a loop variable, Bucket[ i ], which desktop GL 4.10 allows and GLSL ES 3.00 forbids. The kit’s rule is that such a shader is said, not edited: its text is carried unedited in plugin.js and held to the C++ by demo/tools/check_shaders.py, the page tries to compile it on every load and reports the compiler’s verdict on the line under the picture, and the window is summed by a pass of this page’s own — one bucket times its weight, drawn once per bucket in the plugin’s order with additive blending into the same R32F exposure buffer. That is the plugin’s sequential single-precision sum with the accumulator in the blender; what can differ is a rounding per term, where a GPU compiler may fuse the plugin’s multiply-add and the blender rounds the product before the add.',
    'The other five shaders are not a port. The vertex shader, the model library and the expose, coating, resample and plate bodies are the plugin’s own GLSL, assembled as Shaders.cpp assembles them, and check_shaders.py fails the repository’s verify script if a character of any of the seven bodies drifts.',
    'The buffers are the plugin’s: R32F buckets with linear filtering (read texel for texel every frame, between texels only by the resample pass on a resize), an R32F exposure sum and an R32F coating, both Nearest. The expose pass ADDS a frame into its bucket by GL_ONE / GL_ONE blending, as the plugin does. WebGL2 needs EXT_color_buffer_float to render into a float texture, EXT_float_blend to blend into one and OES_texture_float_linear to filter one; the page refuses to start without any of the three rather than fall back to 8 bits or overwrite instead of add.',
    'The clock is the kit’s, in declared seconds; the plugin’s unit vote and its wall-clock fallback never run. Everything downstream is the plugin’s rule: dt is the frame delta clamped to 1/240–1/4 s, a nominal 1/60 on the first frame, so a paused page — which renders only when a control moves — adds 1/240 s to the plate per rendered frame, as a paused host does. Restart is the page’s one addition: the kit sends its clock to 0, the plugin has no rule for a clock that runs backwards, and a backward jump is taken here as a fresh instance (InitGL’s reset of the ring and the take). Step adds exactly 1/60 s.',
    'Take is FF_TYPE_EVENT in the plugin. The kit has no control for an event, so it is the button under the picture rather than a row in the inspector. A press is delivered as the plugin’s SetFloatParameter delivers it — 1.0 then 0.0, edge-triggered on the crossing of 0.5, consumed on the next rendered frame — and in Continuous mode it does nothing, as in the plugin. In embed mode there is no button, so Take mode shows a capped lens.',
    'Buckets is FF_TYPE_INTEGER in the plugin, 2 to 16, with a real range. The kit has no integer control, so it is a dropdown of the same fifteen values.',
    'The plugin stores each host value as a float; the page’s sliders are doubles, so every value is rounded through Math.fround before its law is applied, and the defaults are the plugin’s float defaults. The float laws round each step to float as the C++ computes them; exp2 is Math.pow(2, x) rounded, which can differ from a float exp2 by an ULP.',
    'Output alpha is 1, on purpose: a plate is opaque, and the plate pass writes alpha 1 whatever the source’s was, including under Mix. So there is no backdrop menu here; on the transparency clip the transparent area exposes as black, which is what a premultiplied source hands the plugin.',
    'The coating’s cache key is spelled by JavaScript, not std::to_string; it changes when and only when a raster, the pour, the variation, the bare edge or the defect count changes, which is its whole job.',
    'The harness-only Perturb and Probe uniforms are set to what the shipped plugin sets them to, 0. The six negative controls and the raw H readout wttest drives through them are not on this page. The About block is absent, as on every page in this suite.',
    'The plugin’s proof — pure red, green and blue exposing in the ratio of the computed weights; a moving square’s smear reading (frames covered)/n per column; the bucketed window against the box mean within one bucket’s edge; a grey wedge through the stated curve; density along the drain axis as a + b√s; a take that holds bit-identical from the frame Exposure seconds after the event; a resize that carries the exposure across — is an offline harness in the repository, at two rasters. Nothing on this page measures anything; the line under the picture reports what the ported ring is doing.',
  ],

  createRenderer,
});

// For a driven check (demo/tools and the release's pixel comparison): the
// kit's state and redraw, so a script can pause, set the clock to n / 60 and
// render one frame at a time, as `wttest --pipe --fps 60` clocks its frames.
window.__wetplateDemo = demo;

//---------------------------------------------------------------------------
// Under the canvas: the Take button — the plugin's own FF_TYPE_EVENT control,
// which the kit's inspector cannot draw — and a line reporting what the
// ported ring is doing. Skipped in embed mode, where there is no reader and
// no button.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const row = document.createElement('div');
    row.className = 'transport';
    const button = document.createElement('button');
    button.type = 'button';
    button.className = 'btn';
    button.textContent = 'Take';
    button.id = 'take';
    button.title = 'The plugin’s Take event: in Take mode, uncap the lens, expose for Exposure seconds from now, then develop and hold. In Continuous mode it does nothing.';
    button.addEventListener('click', () => {
      // Wetplate::SetFloatParameter for PT_TAKE, as a host delivers an
      // event: 1.0 on press, 0.0 on release. The next ProcessOpenGL
      // consumes it. A paused page needs that frame asked for.
      take.set(1.0);
      take.set(0.0);
      if (!demo.state.playing) demo.redraw();
    });
    const label = document.createElement('span');
    label.className = 'transport__field';
    label.textContent = 'Take — the plugin’s event control, a button here because the inspector has no event row. Set Mode to Take first.';
    row.append(button, label);

    const line = document.createElement('p');
    line.className = 'stage__status';
    const verdict = document.createElement('p');
    verdict.className = 'stage__status';
    stage.append(row, line, verdict);
    setInterval(() => {
      if (!telemetry.ticked) return;
      const t = telemetry;
      const modeText = t.mode === 1
        ? (t.takeExposing ? 'Take: exposing' : t.takeArmed ? 'Take: developed and held' : 'Take: lens capped, waiting for Take')
        : 'Continuous';
      line.textContent =
        `${modeText}. Ring: ${t.held} of ${t.bucketCount} buckets in the window, ${t.windowSeconds.toFixed(3)} s of a ${secondsText(t.exposureSeconds)} exposure `
        + `(${t.bucketSeconds.toFixed(4)} s a bucket); cell ${t.cell}, slot ${t.current}; clock ${t.now.toFixed(3)} s, this frame ${t.dt.toFixed(4)} s`
        + `${t.accumulate ? '' : ', not exposing'}; ${t.width} × ${t.height}; ${t.takeCount} take${t.takeCount === 1 ? '' : 's'}`
        + `${t.restarts ? `, ${t.restarts} restart${t.restarts === 1 ? '' : 's'}` : ''}.`;
      verdict.textContent = `Sum pass: ${t.sumVerdict}.`;
    }, 250);
  }
}
