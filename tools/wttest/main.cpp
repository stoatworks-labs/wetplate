/**
	wttest -- render Wetplate offline, and read the plate back out of it.

	Every check here drives the REAL plugin class through a headless GL
	context on a synthetic 60 fps clock, and measures the answer out of the
	picture it made:

		wttest --out /tmp/frame.png     a picture, on the moving test card
		wttest --list                   every parameter, its kind and default
		wttest --spectral               pure red, green and blue expose the
		                                plate in the ratio of the computed
		                                weights, under each light; reds and
		                                greens near nothing, blue near white
		wttest --ghost                  a square moving v px/frame over a
		                                window of n frames leaves a smear of
		                                w + v ( n - 1 ) columns, each exposed
		                                (frames covered) / n; a static square
		                                gets full exposure; black stays 0
		wttest --bucket                 the bucketed window is the exact
		                                per-frame box mean to within one
		                                bucket's edge, and is exactly what the
		                                stated rule says
		wttest --curve                  a grey wedge maps through the stated
		                                characteristic curve: fog, gamma,
		                                Dmax, and Development moving them
		wttest --drainage               density along the drain axis is
		                                a + b sqrt( s ), with a and b what the
		                                coating law says
		wttest --take                   in Take mode the plate stops changing
		                                exactly Exposure seconds after the event
		wttest --resize                 a resize mid-exposure carries the
		                                exposure so far across, both ways
		wttest --negative               every check above can FAIL
		wttest --names                  nothing the host will truncate
		wttest --bench                  the render cost and the state held
		wttest --dump-shaders DIR       the exact GLSL the plugin compiles
		wttest --pipe                   raw frames in, raw frames out

	The control laws and the curve are stated HERE, from their definitions
	(Controls.h's comments and Model.h), and read as constants from the same
	headers the plugin compiles: a constant typed wrong there is a failed
	check against the picture, not an agreement. AGENTS.md has one line per
	check on where each tolerance comes from.
*/

#include "Controls.h"
#include "Model.h"
#include "Shaders.h"
#include "Spectral.h"
#include "Wetplate.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model    = wetplate::model;
namespace controls = wetplate::controls;
namespace spectral = wetplate::spectral;

int g_checks   = 0;
int g_failures = 0;

constexpr double kU = 5.9604644775390625e-8;//2^-24, half a float ulp at 1

/// Density read out of the Negative view through the sRGB encode: the
/// exp2 (3 ULP, GLSL 4.10 s8.2), the encode's pow (~9 ULP) and the sum of
/// the curve's terms account for about 1e-6 in density; this is ten times
/// that, and never fitted to a number this machine printed.
constexpr double kDensityRead = 1e-5;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// The model, stated from its definition, in double.
//---------------------------------------------------------------------------
double softplus( double u )
{
	return std::max( u, 0.0 ) + std::log( 1.0 + std::exp( -model::kKnee * std::fabs( u ) ) ) / model::kKnee;
}
double coverage( double logH )
{
	const double c = ( softplus( logH - model::kToe ) - softplus( logH - model::kToe - model::kLatitude ) ) / model::kLatitude;
	return std::clamp( c, 0.0, 1.0 );
}
double densityOf( double logH, double gamma, double fog, double h = 1.0 )
{
	return h * ( fog + gamma * model::kLatitude * coverage( logH ) );
}

/// sRGB, both ways, in double, with NO clamp on the way in: the wedges feed
/// float textures above 1 and the plugin's decode has no clamp either.
double srgbEncode( double v )
{
	return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow( v, 1.0 / 2.4 ) - 0.055;
}
double srgbDecode( double v )
{
	return v <= 0.04045 ? v / 12.92 : std::pow( ( v + 0.055 ) / 1.055, 2.4 );
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first.
//---------------------------------------------------------------------------
using Picture = std::vector< float >;

Picture flat( int W, int H, double r, double g, double b )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ]     = static_cast< float >( r );
		p[ i + 1 ] = static_cast< float >( g );
		p[ i + 2 ] = static_cast< float >( b );
		p[ i + 3 ] = 1.0f;
	}
	return p;
}
Picture flat( int W, int H, double level )
{
	return flat( W, H, level, level, level );
}

void paint( Picture& p, int W, int H, int x0, int y0, int x1, int y1, double r, double g, double b )
{
	for( int y = std::max( 0, y0 ); y < std::min( H, y1 ); ++y )
		for( int x = std::max( 0, x0 ); x < std::min( W, x1 ); ++x )
		{
			float* px = p.data() + ( static_cast< size_t >( y ) * W + x ) * 4;
			px[ 0 ]   = static_cast< float >( r );
			px[ 1 ]   = static_cast< float >( g );
			px[ 2 ]   = static_cast< float >( b );
		}
}
void paint( Picture& p, int W, int H, int x0, int y0, int x1, int y1, double level )
{
	paint( p, W, H, x0, y0, x1, y1, level, level, level );
}

float at( const std::vector< float >& img, int W, int r, int c, int ch = 0 )
{
	return img[ ( static_cast< size_t >( r ) * W + c ) * 4 + ch ];
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Wetplate::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Wetplate& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Wetplate::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range; an integer's range is real.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Wetplate& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Wetplate& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Wetplate& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

/// Every control a check can move, as the sliders the plugin sees. The
/// defaults here are the CLEAN plate: a negative on a neutral light, a flat
/// pour with no bare edge and no defects, no vignette, the stated gamma, no
/// sensitivity change, Continuous. Each check moves the one thing it
/// measures.
struct Knobs
{
	int plate         = 2;//Negative
	int light         = 0;//Daylight
	float development = 0.5f;
	float sensitivity = 0.5f;
	double exposure   = 1.0;
	int mode          = 0;
	int buckets       = 8;
	int pour          = 0;
	float variation   = 0.0f;
	float bareEdge    = 0.0f;
	float defects     = 0.0f;
	float tone        = 0.0f;
	float vignette    = 0.0f;
	float mix         = 1.0f;
};

void apply( Wetplate& p, const Knobs& k )
{
	set( p, "Plate", static_cast< float >( k.plate ) );
	set( p, "Light", static_cast< float >( k.light ) );
	set( p, "Development", k.development );
	set( p, "Sensitivity", k.sensitivity );
	set( p, "Exposure", controls::ExposureParam( k.exposure ) );
	set( p, "Mode", static_cast< float >( k.mode ) );
	set( p, "Buckets", static_cast< float >( k.buckets ) );
	set( p, "Pour", static_cast< float >( k.pour ) );
	set( p, "Coating Var", k.variation );
	set( p, "Bare Edge", k.bareEdge );
	set( p, "Defects", k.defects );
	set( p, "Tone", k.tone );
	set( p, "Vignette", k.vignette );
	set( p, "Mix", k.mix );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Wetplate plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;
	/// Render into an RGBA32F framebuffer rather than RGBA8. The physics
	/// checks read float so their tolerances can be float-derived.
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip or the composition changes size: hand
	/// the SAME instance a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	bool renderAt( int frame )
	{
		//A synthetic clock, and it has to be synthetic: left to the wall
		//clock the harness renders a hundred frames in a few milliseconds
		//and nothing is ever in the window. The unit is declared, not
		//inferred.
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( static_cast< double >( frame ) / fps );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %d\n", frame );
		return ok;
	}

	bool render( int frame, const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	bool render( int frame, const Picture& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return renderAt( frame );
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	std::vector< float > readBackFloat()
	{
		std::vector< float > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( quiet )
		return ok ? 0 : 1;
	va_list args;
	va_start( args, format );
	std::printf( "   %-4s ", verdict( ok ) );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
	return ok ? 0 : 1;
}

/// The density at a pixel of the Negative view at Tone 0: the light is
/// exactly ( 1, 1, 1 ), so the encoded output is T = 10^-D.
double densityAt( const std::vector< float >& out, int W, int r, int c )
{
	const double T = srgbDecode( static_cast< double >( at( out, W, r, c ) ) );
	return -std::log10( std::max( T, 1e-30 ) );
}

//---------------------------------------------------------------------------
// --spectral
//---------------------------------------------------------------------------
int runSpectral( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "spectral: pure red, green, blue and white at radiance 1, %dx%d, probe H\n", W, H );
	int failures = 0;

	//Four patches, one per column band; the input is float so 1.0 decodes
	//to exactly 1.0 and each patch's exposure is exactly one basis weight.
	const int band = W / 4;
	Picture pic    = flat( W, H, 0.0 );
	paint( pic, W, H, 0 * band, 0, 1 * band, H, 1.0, 0.0, 0.0 );
	paint( pic, W, H, 1 * band, 0, 2 * band, H, 0.0, 1.0, 0.0 );
	paint( pic, W, H, 2 * band, 0, 3 * band, H, 0.0, 0.0, 1.0 );
	paint( pic, W, H, 3 * band, 0, 4 * band, H, 1.0, 1.0, 1.0 );

	const int frames = 20;
	//n frames of the same float value summed, then times 1 / seconds: each
	//add and the product may round by half an ULP, so n + 4 halves of an
	//ULP relative, doubled for the ratio's two operands.
	const double tolerance = 2.0 * ( frames + 4 ) * kU;

	for( int light = 0; light < spectral::kLightCount; ++light )
	{
		Session s;
		Knobs k;
		k.light = light;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeRaw );
		if( !s.begin( W, H ) )
			return 1;
		for( int f = 0; f < frames; ++f )
			if( !s.render( f, pic ) )
				return 1;
		const std::vector< float > out = s.readBackFloat();
		s.end();

		const int row = H / 2;
		double measured[ 4 ];
		for( int i = 0; i < 4; ++i )
			measured[ i ] = at( out, W, row, i * band + band / 2 );
		const spectral::Weights& w = spectral::kWeights[ light ];
		const double expected[ 4 ] = { w.w[ 4 ], w.w[ 5 ], w.w[ 6 ], w.w[ 0 ] };
		const char* names[ 4 ]     = { "red", "green", "blue", "white" };
		for( int i = 0; i < 3; ++i )
		{
			const double ratio = measured[ i ] / measured[ 3 ];
			const double want  = expected[ i ] / expected[ 3 ];
			failures += report( std::fabs( ratio - want ) <= tolerance * std::max( 1.0, want ), quiet,
			                    "%-8s %-5s / white = %.7f, computed weight %.7f (tolerance %.1e)", w.light, names[ i ], ratio, want, tolerance );
		}
		failures += report( std::fabs( measured[ 3 ] - 1.0 ) <= tolerance, quiet, "%-8s white exposes to %.7f (1 within %.1e)", w.light, measured[ 3 ], tolerance );
	}

	//The spec's claims, as inequalities on the computed weights: reds and
	//greens barely register, blue is nearly white. Coarse by design.
	const spectral::Weights& d = spectral::kWeights[ 0 ];
	failures += report( d.w[ 4 ] < 0.15 && d.w[ 5 ] < 0.15, quiet, "in daylight red exposes %.3f and green %.3f of white: both under 0.15", d.w[ 4 ], d.w[ 5 ] );
	failures += report( d.w[ 6 ] > 0.8, quiet, "in daylight blue exposes %.3f of white: over 0.8", d.w[ 6 ] );
	return failures;
}

//---------------------------------------------------------------------------
// --ghost
//---------------------------------------------------------------------------
int runGhost( int W, int H, int perturb, bool quiet = false )
{
	//Exposure 0.5 s in 6 buckets at 60 fps: 5 frames a bucket. Frame 59 is
	//the last frame of a bucket, so the window is exactly the last 30
	//frames, 30..59. The square moves 2 whole pixels a frame.
	const int n      = 30;
	const int wb     = 5;
	const int last   = 59;
	const int v      = 2;
	const int w      = 20;
	const int x0     = 10;
	const int rowTop = H / 4;
	const int sx     = W * 5 / 8;
	const int sy     = H / 2;
	if( !quiet )
		std::printf( "ghost: a %d px square at %d px/frame, a static square, over a %d-frame window in %d buckets, %dx%d\n", w, v, n, n / wb, W, H );
	if( x0 + v * last + w >= W || sx + w >= W || rowTop + w >= H || sy + w >= H )
	{
		std::printf( "   FAIL the raster is too small for the ghost's path\n" );
		++g_checks;
		++g_failures;
		return 1;
	}

	Session s;
	Knobs k;
	k.exposure = 0.5;
	k.buckets  = n / wb;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetProbeForTest( model::kProbeRaw );
	if( !s.begin( W, H ) )
		return 1;
	for( int f = 0; f <= last; ++f )
	{
		Picture pic = flat( W, H, 0.0 );
		const int x = x0 + v * f;
		paint( pic, W, H, x, rowTop, x + w, rowTop + w, 1.0 );
		paint( pic, W, H, sx, sy, sx + w, sy + w, 1.0 );
		if( !s.render( f, pic ) )
			return 1;
	}
	const std::vector< float > out = s.readBackFloat();
	int held   = 0;
	double sec = 0.0;
	s.plugin.WindowForTest( held, sec );
	s.end();

	int failures = 0;
	failures += report( held == n / wb && std::fabs( sec - n / 60.0 ) < 1e-9, quiet, "the window holds %d buckets, %.6f s (want %d, %.6f)", held, sec, n / wb, n / 60.0 );

	//n adds of e * dt then times 1 / seconds: ( n + 4 ) halves of an ULP.
	const double tolerance = ( n + 4 ) * kU;
	const int row          = rowTop + w / 2;
	const int first        = x0 + v * ( last - n + 1 );
	const int smear        = w + v * ( n - 1 );
	double worst           = 0.0;
	int bad                = 0;
	for( int c = first - 5; c < first + smear + 5; ++c )
	{
		int count = 0;
		for( int f = last - n + 1; f <= last; ++f )
		{
			const int x = x0 + v * f;
			if( c >= x && c < x + w )
				++count;
		}
		const double want = static_cast< double >( count ) / n;
		const double got  = at( out, W, row, c );
		worst             = std::max( worst, std::fabs( got - want ) );
		if( std::fabs( got - want ) > tolerance )
			++bad;
	}
	failures += report( bad == 0, quiet, "smear of %d columns (w + v ( n - 1 )): every column's exposure is (frames covered) / %d, worst %.2e (tolerance %.1e)", smear, n, worst, tolerance );
	const double staticGot = at( out, W, sy + w / 2, sx + w / 2 );
	failures += report( std::fabs( staticGot - 1.0 ) <= tolerance, quiet, "the static square reads %.7f (full exposure 1 within %.1e)", staticGot, tolerance );
	const double black = at( out, W, sy + w / 2, sx + w + 8 );
	failures += report( black == 0.0, quiet, "black reads exactly %g", black );
	return failures;
}

//---------------------------------------------------------------------------
// --bucket
//---------------------------------------------------------------------------
int runBucket( int W, int H, int perturb, bool quiet = false )
{
	//Exposure 0.5 s in 5 buckets at 60 fps: 6 frames a bucket. The square
	//moves one whole pixel a frame, so the exact box mean over the last 30
	//frames is a staircase every column of which the bucketing can get
	//wrong by at most the edge bucket's frames.
	const int n      = 30;
	const int wb     = 6;
	const int B      = n / wb;
	const int w      = 20;
	const int x0     = 4;
	const int rowTop = H / 3;
	const int f0     = 40;
	const int f1     = 70;
	if( !quiet )
		std::printf( "bucket: a %d px square at 1 px/frame, window %d frames in %d buckets, frames %d..%d, %dx%d\n", w, n, B, f0, f1, W, H );
	if( x0 + f1 + w >= W || rowTop + w >= H )
	{
		std::printf( "   FAIL the raster is too small for the square's path\n" );
		++g_checks;
		++g_failures;
		return 1;
	}

	Session s;
	Knobs k;
	k.exposure = 0.5;
	k.buckets  = B;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetProbeForTest( model::kProbeRaw );
	if( !s.begin( W, H ) )
		return 1;

	const double bound = static_cast< double >( wb - 1 ) / n;//one bucket's edge, less the frame it keeps
	const double floatTolerance = ( n + 4 ) * kU;
	double worstBox = 0.0, worstRule = 0.0;
	int badBox = 0, badRule = 0, badWindow = 0;
	const int row = rowTop + w / 2;
	for( int f = 0; f <= f1; ++f )
	{
		Picture pic = flat( W, H, 0.0 );
		paint( pic, W, H, x0 + f, rowTop, x0 + f + w, rowTop + w, 1.0 );
		if( !s.render( f, pic ) )
			return 1;
		if( f < f0 )
			continue;
		const std::vector< float > out = s.readBackFloat();

		//The plugin's window, from the stated rule: the grid is anchored at
		//time 0 with cells of wb frames; the current cell is f / wb; the
		//window is the last B cells, from ( f / wb - ( B - 1 ) ) * wb to f.
		const int oldest = ( f / wb - ( B - 1 ) ) * wb;
		const int m      = f - oldest + 1;
		int held         = 0;
		double sec       = 0.0;
		s.plugin.WindowForTest( held, sec );
		if( held != B || std::fabs( sec - m / 60.0 ) > 1e-9 )
		{
			if( badWindow == 0 && !quiet )
				std::printf( "   first window mismatch at frame %d: held %d, %.6f s; rule says %d buckets, %d frames (oldest %d)\n", f, held, sec, B, m, oldest );
			++badWindow;
		}

		for( int c = x0 + f - n - 2; c < x0 + f + w + 2; ++c )
		{
			int boxCount = 0, ruleCount = 0;
			for( int g = f - n + 1; g <= f; ++g )
			{
				const bool covered = c >= x0 + g && c < x0 + g + w;
				if( covered )
					++boxCount;
				if( covered && g >= oldest )
					++ruleCount;
			}
			const double box  = static_cast< double >( boxCount ) / n;
			const double rule = static_cast< double >( ruleCount ) / m;
			const double got  = at( out, W, row, c );
			worstBox          = std::max( worstBox, std::fabs( got - box ) );
			worstRule         = std::max( worstRule, std::fabs( got - rule ) );
			if( std::fabs( got - box ) > bound )
				++badBox;
			if( std::fabs( got - rule ) > floatTolerance )
				++badRule;
		}
	}
	s.end();

	int failures = 0;
	failures += report( badBox == 0, quiet, "against the exact 30-frame box mean: worst %.4f, bound %.4f (%d frames of one bucket's edge over %d)", worstBox, bound, wb - 1, n );
	failures += report( worstBox > 0.0, quiet, "the bucketing is real: the worst difference from the box is %.4f, not 0", worstBox );
	failures += report( badRule == 0, quiet, "against the stated bucket rule: worst %.2e (float tolerance %.1e)", worstRule, floatTolerance );
	failures += report( badWindow == 0, quiet, "the plugin's window length is what the rule says on every frame" );
	return failures;
}

//---------------------------------------------------------------------------
// --curve
//---------------------------------------------------------------------------
int runCurve( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "curve: a grey wedge on the negative, %dx%d, density read through the sRGB encode\n", W, H );
	int failures = 0;

	//41 steps of log10 H from -2.6 to +1.4, a float wedge fed as sRGB code
	//values (the plugin decodes), sampled at step centres.
	const int steps    = 41;
	const double lo    = -2.6;
	const double hi    = 1.4;
	const int stepW    = W / steps;
	if( stepW < 2 )
	{
		std::printf( "   FAIL the raster is too narrow for %d steps\n", steps );
		++g_checks;
		++g_failures;
		return 1;
	}
	std::vector< double > logH( steps );
	Picture pic = flat( W, H, 0.0 );
	for( int i = 0; i < steps; ++i )
	{
		logH[ i ]     = lo + ( hi - lo ) * i / ( steps - 1 );
		const double code = srgbEncode( std::pow( 10.0, logH[ i ] ) );
		paint( pic, W, H, i * stepW, 0, ( i + 1 ) * stepW, H, code );
	}

	const float developments[ 3 ] = { 0.0f, 0.5f, 1.0f };
	const int row                 = H / 2;
	for( float development : developments )
	{
		Session s;
		Knobs k;
		k.development = development;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		if( !s.begin( W, H ) )
			return 1;
		for( int f = 0; f < 8; ++f )
			if( !s.render( f, pic ) )
				return 1;
		const std::vector< float > out = s.readBackFloat();
		s.end();

		const double gamma = controls::DevelopmentGamma( development );
		const double fog   = controls::DevelopmentFog( development );

		//(a) Every step against the stated curve. The GLSL log and exp
		//carry 3 ULP each (s8.2), which at these magnitudes is under 1e-6 in
		//density; kDensityRead covers it ten times over.
		double worst = 0.0;
		std::vector< double > D( steps );
		for( int i = 0; i < steps; ++i )
		{
			D[ i ]            = densityAt( out, W, row, i * stepW + stepW / 2 );
			const double want = densityOf( logH[ i ], gamma, fog );
			worst             = std::max( worst, std::fabs( D[ i ] - want ) );
		}
		failures += report( worst <= kDensityRead, quiet, "Development %.1f (gamma %.2f): every step within %.2e of the stated curve (tolerance %.0e)", development, gamma, worst, kDensityRead );

		//(b) Out of the picture: the floor far under the toe, the ceiling
		//far over the shoulder, and the secant across the middle of the
		//line. The softplus leaves gamma L / Knee e^( -Knee d ) at distance
		//d from a bend; at the ends d = 1.5 and 1.3, under 1e-4, and the
		//secant 0.4 inside both bends lies in [ gamma ( 1 - 2 e^-2.4 ), gamma ].
		const double floorResidual   = gamma * model::kLatitude / model::kKnee * std::exp( -model::kKnee * ( model::kToe - lo ) ) + 2 * kDensityRead;
		const double ceilingResidual = gamma * model::kLatitude / model::kKnee * std::exp( -model::kKnee * ( hi - model::kToe - model::kLatitude ) ) + 2 * kDensityRead;
		failures += report( std::fabs( D[ 0 ] - fog ) <= floorResidual, quiet, "  floor %.5f = fog %.5f within %.1e", D[ 0 ], fog, floorResidual );
		failures += report( std::fabs( D[ steps - 1 ] - ( fog + gamma * model::kLatitude ) ) <= ceilingResidual, quiet, "  ceiling %.5f = fog + gamma L = %.5f within %.1e", D[ steps - 1 ], fog + gamma * model::kLatitude, ceilingResidual );

		//The secant: the two steps nearest 0.4 inside the toe and the
		//shoulder.
		const double xa = model::kToe + 0.4, xb = model::kToe + model::kLatitude - 0.4;
		int ia = 0, ib = 0;
		for( int i = 0; i < steps; ++i )
		{
			if( std::fabs( logH[ i ] - xa ) < std::fabs( logH[ ia ] - xa ) )
				ia = i;
			if( std::fabs( logH[ i ] - xb ) < std::fabs( logH[ ib ] - xb ) )
				ib = i;
		}
		const double da       = std::min( logH[ ia ] - model::kToe, model::kToe + model::kLatitude - logH[ ia ] );
		const double db       = std::min( logH[ ib ] - model::kToe, model::kToe + model::kLatitude - logH[ ib ] );
		const double shortfall = std::exp( -model::kKnee * da ) + std::exp( -model::kKnee * db );
		const double slope    = ( D[ ib ] - D[ ia ] ) / ( logH[ ib ] - logH[ ia ] );
		const double slack    = 2.0 * kDensityRead / ( logH[ ib ] - logH[ ia ] );
		failures += report( slope >= gamma * ( 1.0 - shortfall ) - slack && slope <= gamma + slack, quiet,
		                    "  secant %.4f between log H %.2f and %.2f, in [%.4f, %.4f]", slope, logH[ ia ], logH[ ib ], gamma * ( 1.0 - shortfall ) - slack, gamma + slack );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --drainage
//---------------------------------------------------------------------------
int runDrainage( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "drainage: a flat grey on the negative, density along each pour's drain axis, %dx%d\n", W, H );
	int failures = 0;

	const double x0    = model::kToe + model::kLatitude * 0.5;//the middle of the line
	const Picture pic  = flat( W, H, srgbEncode( std::pow( 10.0, x0 ) ) );
	const double V     = controls::CoatingVariation( 1.0f );
	const int samples  = 21;

	for( int pour = 0; pour < controls::kPourCount; ++pour )
	{
		std::vector< float > flatOut, variedOut;
		for( int pass = 0; pass < 2; ++pass )
		{
			Session s;
			Knobs k;
			k.pour      = pour;
			k.variation = pass == 0 ? 0.0f : 1.0f;
			apply( s.plugin, k );
			s.plugin.SetPerturbForTest( perturb );
			if( !s.begin( W, H ) )
				return 1;
			for( int f = 0; f < 4; ++f )
				if( !s.render( f, pic ) )
					return 1;
			( pass == 0 ? flatOut : variedOut ) = s.readBackFloat();
			s.end();
		}

		float ox, oy, dx, dy;
		controls::PourOrigin( pour, ox, oy );
		controls::PourDirection( pour, dx, dy );

		//Samples along the pour-to-drain diagonal at pixel centres, s from
		//the stated projection. D0 is the flat pour's density at the same
		//pixel, so the fit is of h alone.
		double sumS = 0, sumS2 = 0, sumR = 0, sumSR = 0;
		std::vector< double > sq( samples ), ratio( samples );
		for( int i = 0; i < samples; ++i )
		{
			const double t = ( i + 0.5 ) / samples;
			const int px   = static_cast< int >( std::lround( ( ox > 0.5f ? 1.0 - t : t ) * ( W - 1 ) ) );
			const int py   = static_cast< int >( std::lround( ( oy > 0.5f ? 1.0 - t : t ) * ( H - 1 ) ) );
			const double fx = ( px + 0.5 ) / W, fy = ( py + 0.5 ) / H;
			const double sVal = std::clamp( ( ( fx - ox ) * dx + ( fy - oy ) * dy ) * 0.5, 0.0, 1.0 );
			sq[ i ]           = std::sqrt( sVal );
			const double D0   = densityAt( flatOut, W, py, px );
			const double D    = densityAt( variedOut, W, py, px );
			ratio[ i ]        = D / D0;
			sumS += sq[ i ];
			sumS2 += sq[ i ] * sq[ i ];
			sumR += ratio[ i ];
			sumSR += sq[ i ] * ratio[ i ];
		}
		const double denom = samples * sumS2 - sumS * sumS;
		const double b     = ( samples * sumSR - sumS * sumR ) / denom;
		const double a     = ( sumR - b * sumS ) / samples;
		double residual    = 0.0;
		for( int i = 0; i < samples; ++i )
			residual = std::max( residual, std::fabs( ratio[ i ] - ( a + b * sq[ i ] ) ) );

		//h = D / D0 is a ratio of two densities each read to kDensityRead,
		//over a D0 near 0.75: 2 x 1e-5 / 0.75 = 2.7e-5 per point; 4e-5.
		const double D0mid    = densityOf( x0, controls::DevelopmentGamma( 0.5f ), controls::DevelopmentFog( 0.5f ) );
		const double readH    = 2.0 * kDensityRead / D0mid;
		const double fitSlack = 4.0 * readH;
		failures += report( residual <= 2.0 * readH, quiet, "%-12s h = a + b sqrt( s ): worst residual %.2e (tolerance %.1e)", controls::PourName( pour ), residual, 2.0 * readH );
		failures += report( std::fabs( a - ( 1.0 - 2.0 * V / 3.0 ) ) <= fitSlack && std::fabs( b - V ) <= fitSlack, quiet,
		                    "%-12s a = %.5f (1 - 2V/3 = %.5f), b = %.5f (V = %.5f), tolerance %.1e", controls::PourName( pour ), a, 1.0 - 2.0 * V / 3.0, b, V, fitSlack );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --take
//---------------------------------------------------------------------------
int runTake( int W, int H, int perturb, bool quiet = false )
{
	const int n    = 30;//Exposure 0.5 s at 60 fps
	const int fire = 20;
	const int last = 90;
	const int w    = 16;
	if( !quiet )
		std::printf( "take: Take mode, the event at frame %d, Exposure %d frames, a square moving 2 px/frame, %dx%d\n", fire, n, W, H );

	Session s;
	Knobs k;
	k.exposure = 0.5;
	k.mode     = 1;
	k.buckets  = 5;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetProbeForTest( model::kProbeRaw );
	if( !s.begin( W, H ) )
		return 1;

	const int takeIndex = indexOfParameter( s.plugin, "Take" );
	std::map< int, std::vector< float > > frames;
	bool blankBefore = true;
	int changes = 0, stills = 0;
	std::vector< float > previous;
	for( int f = 0; f <= last; ++f )
	{
		if( f == fire )
		{
			s.plugin.SetFloatParameter( static_cast< unsigned int >( takeIndex ), 1.0f );
			s.plugin.SetFloatParameter( static_cast< unsigned int >( takeIndex ), 0.0f );
		}
		Picture pic = flat( W, H, 0.0 );
		const int x = 4 + 2 * f;
		paint( pic, W, H, x, H / 3, x + w, H / 3 + w, 1.0 );
		if( !s.render( f, pic ) )
			return 1;
		const std::vector< float > out = s.readBackFloat();
		if( f < fire )
		{
			for( int y = 0; y < H && blankBefore; ++y )
				for( int c = 0; c < W; ++c )
					if( at( out, W, y, c ) != 0.0f )
					{
						blankBefore = false;
						break;
					}
		}
		else if( f > fire )
		{
			const bool same = out == previous;
			if( f <= fire + n - 1 )
				changes += same ? 0 : 1;
			else
			{
				stills += same ? 1 : 0;
				if( !same && !quiet )
					std::printf( "   frame %d differs from frame %d\n", f, f - 1 );
			}
		}
		previous = out;
	}
	s.end();

	int failures = 0;
	failures += report( blankBefore, quiet, "before the take the plate reads exactly 0 everywhere" );
	failures += report( changes == n - 1, quiet, "the plate changed on every frame of the exposure: %d of %d", changes, n - 1 );
	failures += report( stills == last - ( fire + n - 1 ), quiet, "from frame %d (the event + Exposure) on, %d of %d frames are bit-identical to the developed plate", fire + n, stills, last - ( fire + n - 1 ) );
	return failures;
}

//---------------------------------------------------------------------------
// --resize
//---------------------------------------------------------------------------
int runResize( int W, int H, int perturb, bool quiet = false )
{
	//Exposure 1 s in 4 buckets: 15 frames a bucket, and at frame 59 the
	//window is exactly frames 0..59. White for the first 30, then black;
	//the raster changes to 1.5x at frame 30 and back at frame 45. The mean
	//exposure at frame 59 is 30 / 60 = 0.5 whatever the raster did.
	const int W2 = W * 3 / 2, H2 = H * 3 / 2;
	if( !quiet )
		std::printf( "resize: 30 frames of white at %dx%d, a resize to %dx%d, black, a resize back, read at frame 59\n", W, H, W2, H2 );

	Session s;
	Knobs k;
	k.exposure = 1.0;
	k.buckets  = 4;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetProbeForTest( model::kProbeRaw );
	if( !s.begin( W, H ) )
		return 1;
	for( int f = 0; f < 60; ++f )
	{
		if( f == 30 )
			s.resize( W2, H2 );
		if( f == 45 )
			s.resize( W, H );
		if( !s.render( f, flat( s.width, s.height, f < 30 ? 1.0 : 0.0 ) ) )
			return 1;
	}
	const std::vector< float > out = s.readBackFloat();
	int held   = 0;
	double sec = 0.0;
	s.plugin.WindowForTest( held, sec );
	s.end();

	//60 adds and a product: 64 halves of an ULP; the two bilinear resamples
	//of a flat field add a couple more.
	const double tolerance = ( 60 + 8 ) * kU;
	double worst           = 0.0;
	for( int y = 0; y < H; ++y )
		for( int c = 0; c < W; ++c )
			worst = std::max( worst, std::fabs( at( out, W, y, c ) - 0.5 ) );
	int failures = 0;
	failures += report( held == 4 && std::fabs( sec - 1.0 ) < 1e-9, quiet, "the window holds %d buckets, %.6f s", held, sec );
	failures += report( worst <= tolerance, quiet, "every pixel reads the exposure so far, 0.5, worst %.2e (tolerance %.1e)", worst, tolerance );
	return failures;
}

//---------------------------------------------------------------------------
// --negative
//---------------------------------------------------------------------------
int runNegative( int W, int H )
{
	std::printf( "negative controls: each perturbation of the plugin's model must FAIL its check, %dx%d\n", W, H );
	struct Control
	{
		int bits;
		const char* what;
		int ( *check )( int, int, int, bool );
		const char* name;
	};
	const Control controls[] = {
		{ model::kPerturbLumaWeights, "weight by BT.709 luma instead of the plate", runSpectral, "--spectral" },
		{ model::kPerturbOneFrame, "integrate one frame instead of the window", runGhost, "--ghost" },
		{ model::kPerturbOneFrame, "integrate one frame instead of the window", runBucket, "--bucket" },
		{ model::kPerturbLinearDrain, "coating linear in s instead of sqrt( s )", runDrainage, "--drainage" },
		{ model::kPerturbGamma, "gamma x 0.8 in the shader", runCurve, "--curve" },
		{ model::kPerturbResizeClears, "a resize that clears the buckets", runResize, "--resize" },
		{ model::kPerturbTakeNeverEnds, "a take that never stops integrating", runTake, "--take" },
	};
	int failures = 0;
	for( const Control& c : controls )
	{
		const int before  = g_failures;
		const int checks  = g_checks;
		const int failed  = c.check( W, H, c.bits, true );
		g_failures        = before;
		g_checks          = checks;
		failures += report( failed > 0, false, "%-46s -> %s fails (%d of its checks)", c.what, c.name, failed );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --names
//---------------------------------------------------------------------------
int runNames()
{
	std::printf( "names: nothing the host will silently truncate; every name unique\n" );
	Wetplate plugin;
	int failures = 0;
	std::set< std::string > seen;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( p.index >= Wetplate::PT_ABOUT_FIRST )
			continue;
		failures += report( p.name.size() <= 16, false, "%-16s %2zu characters", p.name.c_str(), p.name.size() );
		failures += report( seen.insert( p.name ).second, false, "%-16s unique", p.name.c_str() );
	}
	failures += report( std::string( "SW Wetplate" ).size() <= 16, false, "display name 'SW Wetplate' is %zu characters", std::string( "SW Wetplate" ).size() );
	return failures;
}

//---------------------------------------------------------------------------
// The moving card, for --out, the sweep, the bench and a default --pipe: a
// row of colour patches (red, skin, yellow, green, cyan, blue, sky, white),
// a grey ramp, a white disc on an orbit, a static black square, and a
// drifting bar.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t  = static_cast< double >( frame ) / 60.0;
	const double cx = 0.72 + 0.14 * std::cos( 1.2 * t ), cy = 0.68 + 0.16 * std::sin( 1.2 * t );
	const double barX = std::fmod( 0.05 * t, 1.0 );
	const double patches[ 8 ][ 3 ] = {
		{ 0.80, 0.10, 0.10 }, { 0.88, 0.67, 0.55 }, { 0.90, 0.85, 0.15 }, { 0.15, 0.60, 0.20 },
		{ 0.20, 0.80, 0.85 }, { 0.15, 0.25, 0.85 }, { 0.53, 0.81, 0.92 }, { 0.95, 0.95, 0.95 },
	};
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double r = 0.40, g = 0.40, b = 0.40;
			//A row of patches.
			if( fy > 0.06 && fy < 0.30 )
			{
				const int i = std::clamp( static_cast< int >( ( fx - 0.04 ) / 0.115 ), 0, 7 );
				if( fx > 0.04 && fx < 0.96 && std::fmod( fx - 0.04, 0.115 ) < 0.105 )
				{
					r = patches[ i ][ 0 ];
					g = patches[ i ][ 1 ];
					b = patches[ i ][ 2 ];
				}
			}
			//A grey ramp.
			if( fy > 0.36 && fy < 0.46 && fx > 0.04 && fx < 0.96 )
				r = g = b = ( fx - 0.04 ) / 0.92;
			//A static black square.
			if( fx > 0.08 && fx < 0.28 && fy > 0.56 && fy < 0.92 )
				r = g = b = 0.02;
			//A white disc on an orbit.
			const double dx = ( fx - cx ) * width, dy = ( fy - cy ) * height;
			if( dx * dx + dy * dy < ( height * 0.06 ) * ( height * 0.06 ) )
				r = g = b = 0.98;
			//A drifting bar, in blue.
			if( std::fabs( fx - barX ) < 0.012 && fy > 0.5 )
			{
				r = 0.2;
				g = 0.3;
				b = 0.9;
			}
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ]           = static_cast< unsigned char >( std::lround( 255.0 * r ) );
			px[ 1 ]           = static_cast< unsigned char >( std::lround( 255.0 * g ) );
			px[ 2 ]           = static_cast< unsigned char >( std::lround( 255.0 * b ) );
			px[ 3 ]           = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( Wetplate& plugin, int width, int height, int frames, double fps, size_t& stateBytes )
{
	Session session;
	session.floatOutput = false;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.index < Wetplate::PT_ABOUT_FIRST && p.type != FF_TYPE_BUFFER && p.type != FF_TYPE_EVENT )
			session.plugin.SetFloatParameter( p.index, p.value );
	session.fps = fps;
	if( !session.begin( width, height ) )
		return -1.0;

	//The card is uploaded once and not per frame, so the upload is not in
	//the figure: a host's frame is already on the GPU. The plugin's cost
	//does not depend on what the frame holds -- every pass runs every frame
	//and the coating is cached whatever the picture does -- so a still is
	//an honest load. Uploading a 4K frame per frame had put 9 of 11 ms on
	//the clock.
	const std::vector< unsigned char > card = buildCard( width, height, 0 );
	const int warmup                        = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, card );
	glFinish();

	//The best of three runs. This machine's GPU is shared with whatever
	//else is rendering, and a run that lost the GPU for a few milliseconds
	//measures the contention, not the plugin; the minimum is the one
	//nothing else interrupted.
	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int frame = 0; frame < frames; ++frame )
			session.renderAt( warmup + run * frames + frame );
		glFinish();
		const auto end       = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration< double >( end - start ).count();
		best                 = std::min( best, seconds * 1000.0 / static_cast< double >( frames ) );
	}
	stateBytes = session.plugin.StateBytesForTest();
	session.end();
	return best;
}

int runBench( Wetplate& plugin, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "3840x2160 ", 3840, 2160 },
	};

	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides, the card uploaded once.\n\n", frames );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame   state held\n" );
	for( const Size& size : sizes )
	{
		size_t bytes    = 0;
		const double ms = benchAt( plugin, size.width, size.height, frames, fps, bytes );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%          %6.1f MB\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0, static_cast< double >( bytes ) / 1048576.0 );
	}
	std::printf( "\nState is ( Buckets + 2 ) R32F frames: the buckets, the exposure sum and the\n"
	             "coating, colour textures only. The SDK's FBO attaches a depth renderbuffer to\n"
	             "each that the plugin never uses and this does not count. Whatever the\n"
	             "settings above were, they are what was measured; --set measures another.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace shaders = wetplate::shaders;
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", shaders::Vertex() },     { "expose.frag", shaders::Expose() },     { "sum.frag", shaders::Sum() },
		{ "coating.frag", shaders::Coating() }, { "resample.frag", shaders::Resample() }, { "plate.frag", shaders::Plate() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//
// A STANDARD parameter ramps linearly between cues. An option, a boolean
// and an integer STEP: they hold the last cue at or before the frame,
// because there is nothing between Tintype and Ambrotype to ramp through.
// An event fires on its cue frame only: 1 on that frame, 0 on every other,
// which is what a host's button press looks like to the plugin.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame, unsigned int type )
{
	if( track.empty() )
		return 0.0f;
	if( type == FF_TYPE_EVENT )
	{
		for( const auto& cue : track )
			if( cue.first == frame )
				return cue.second;
		return 0.0f;
	}
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( type != FF_TYPE_STANDARD )
				return frame == b.first ? b.second : a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"wttest -- render and measure the Wetplate collodion plate\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/wetplate.png)\n"
		"  --size WxH          raster (default 1280x720); --width N / --height N also accepted\n"
		"  --frames N          frames to render before reading back (default 90)\n"
		"  --fps N             synthetic frame rate driving the clock (default 60)\n"
		"  --source card|flat|white|black   what to feed (card moves); --level V for flat\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options,\n"
		"                      the integer for Buckets). Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --spectral          pure R, G, B expose in the ratio of the computed weights, each light\n"
		"  --ghost             a moving square leaves a smear exposed (frames covered) / n\n"
		"  --bucket            the bucketed window is the box mean within one bucket's edge\n"
		"  --curve             a grey wedge maps through the stated characteristic curve\n"
		"  --drainage          density along the drain axis is a + b sqrt( s )\n"
		"  --take              the plate stops changing exactly Exposure after the Take\n"
		"  --resize            a resize mid-exposure carries the exposure across\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed plate (bits in Model.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --names             nothing the host will silently truncate\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K, and the state held\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/wetplate.png";
	std::string scriptPath;
	std::string dumpDir;
	std::string source = "card";
	double level   = 0.5;
	int width      = 1280;
	int height     = 720;
	int frames     = 90;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--spectral", "--ghost", "--bucket", "--curve", "--drainage", "--take", "--resize", "--negative" };
	const std::set< std::string > offline  = { "--names" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			source = argv[ ++i ];
		else if( argument == "--level" && hasNext )
			level = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--width" && hasNext )
			width = std::atoi( argv[ ++i ] );
		else if( argument == "--height" && hasNext )
			height = std::atoi( argv[ ++i ] );
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Wetplate plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		bool needGL = false;
		for( const std::string& check : checks )
		{
			if( check == "--names" )
			{
				runNames();
				std::printf( "\n" );
			}
			else
				needGL = true;
		}

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--spectral" )
						runSpectral( width, height, perturb );
					else if( check == "--ghost" )
						runGhost( width, height, perturb );
					else if( check == "--bucket" )
						runBucket( width, height, perturb );
					else if( check == "--curve" )
						runCurve( width, height, perturb );
					else if( check == "--drainage" )
						runDrainage( width, height, perturb );
					else if( check == "--take" )
						runTake( width, height, perturb );
					else if( check == "--resize" )
						runResize( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	Session session;
	session.floatOutput = false;
	session.fps         = fps;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}
	session.plugin.SetPerturbForTest( perturb );

	if( wantBench )
		return finish( runBench( session.plugin, frames < 40 ? 60 : frames, fps ) );

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		struct Automation
		{
			unsigned int index;
			unsigned int type;
			Track track;
		};
		std::vector< Automation > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation.push_back( { static_cast< unsigned int >( index ), session.plugin.GetParamType( static_cast< unsigned int >( index ) ), entry.second } );
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider
			//would, and an event is a press.
			for( const Automation& a : automation )
				session.plugin.SetFloatParameter( a.index, valueAt( a.track, index, a.type ) );

			const bool rendered = index != failRender && session.render( index, frame );
			if( !rendered )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	for( int frame = 0; frame < frames; ++frame )
	{
		bool ok = false;
		if( source == "card" )
			ok = session.render( frame, buildCard( width, height, frame ) );
		else if( source == "flat" )
			ok = session.render( frame, flat( width, height, level ) );
		else if( source == "white" )
			ok = session.render( frame, flat( width, height, 1.0 ) );
		else if( source == "black" )
			ok = session.render( frame, flat( width, height, 0.0 ) );
		else
		{
			std::fprintf( stderr, "unknown --source %s\n", source.c_str() );
			return finish( 2 );
		}
		if( !ok )
			return finish( 1 );
	}

	const std::vector< unsigned char > image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
