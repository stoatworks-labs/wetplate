#include "Wetplate.h"

#include "Controls.h"
#include "Diag.h"
#include "Model.h"
#include "Shaders.h"
#include "Spectral.h"

#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace ffglex;
using namespace wetplate;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Wetplate >,                                   // Create method
	"WT01",                                                      // Plugin unique ID of maximum length 4.
	"SW Wetplate",                                               // Plugin name
	2,                                                           // API major version number
	1,                                                           // API minor version number
	0,                                                           // Plugin major version number
	1,                                                           // Plugin minor version number
	FF_EFFECT,                                                   // Plugin type
	"A collodion wet plate, blue-sensitive and seconds long.\n\nThe clip exposes a plate that sees only blue and ultraviolet, integrated over an exposure of seconds: reds and skin go dark, skies go white, and whatever moved is a ghost in proportion to how long it stayed. The plate is hand-poured, thicker toward the drain corner, bare where the pour never reached, marked by dust the silver bath ran around; shown as a tintype, an ambrotype or a glass negative.",// Plugin description
	"Wetplate FFGL effect"                                       // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// Wall clock, for hosts that never call SetTime. Steady rather than system,
/// so nothing here moves when the machine's clock is corrected.
double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

/// glGetString returns nullptr when there is no current context, and feeding
/// that to std::string is undefined behaviour.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// A tiny slack on a grid comparison, so a frame that lands on a cell
/// boundary to within double rounding is counted on the side it means.
constexpr double kGridSlack = 1e-9;
} // namespace

//---------------------------------------------------------------------------
Wetplate::Wetplate()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The plate integrates over host time: which frames are in the window is
	//a function of the clock, so a re-render of the same composition must
	//expose the same plate the same way.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults. A tintype in daylight, developed to the plate's stated
	// gamma, a half-second exposure held in eight buckets, poured from the
	// top left with a modest drain, a little bare edge and a few defects,
	// slightly warm, a little vignette.
	//---------------------------------------------------------------------
	params[ PT_PLATE ]       = 0.0f;//Tintype
	params[ PT_LIGHT ]       = 0.0f;//Daylight
	params[ PT_DEVELOPMENT ] = 0.5f;
	params[ PT_SENSITIVITY ] = controls::SensitivityParam( 0.0f );

	params[ PT_EXPOSURE ] = controls::ExposureParam( 0.5 );
	params[ PT_MODE ]     = 0.0f;//Continuous
	params[ PT_TAKE ]     = 0.0f;
	params[ PT_BUCKETS ]  = 8.0f;

	params[ PT_POUR ]              = 0.0f;//Top Left
	params[ PT_COATING_VARIATION ] = 0.35f;
	params[ PT_BARE_EDGE ]         = 0.3f;
	params[ PT_DEFECTS ]           = 0.25f;

	params[ PT_TONE ]     = 0.45f;
	params[ PT_VIGNETTE ] = 0.35f;
	params[ PT_MIX ]      = 1.0f;

	//---------------------------------------------------------------------
	// Declaration. Every ranged FF_TYPE_STANDARD parameter is a plain 0..1
	// float: SetParamInfo clamps a STANDARD default into 0..1 *before* a
	// range can be attached (SDK b1afaf9). The conversions live in
	// Controls.cpp. Option lists are in their natural order and not sorted.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int id, const char* name, int count, const char* ( *nameAt )( int ) ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameAt( i ), static_cast< float >( i ) );
	};

	declareOptions( PT_PLATE, "Plate", controls::kPlateCount, controls::PlateName );
	declareOptions( PT_LIGHT, "Light", controls::kLightCount, controls::LightName );
	SetParamInfof( PT_DEVELOPMENT, "Development", FF_TYPE_STANDARD );
	SetParamInfof( PT_SENSITIVITY, "Sensitivity", FF_TYPE_STANDARD );

	SetParamInfof( PT_EXPOSURE, "Exposure", FF_TYPE_STANDARD );
	declareOptions( PT_MODE, "Mode", controls::kModeCount, controls::ModeName );
	SetParamInfo( PT_TAKE, "Take", FF_TYPE_EVENT, false );
	SetParamInfo( PT_BUCKETS, "Buckets", FF_TYPE_INTEGER, params[ PT_BUCKETS ] );
	SetParamRange( PT_BUCKETS, static_cast< float >( controls::kMinBuckets ), static_cast< float >( controls::kMaxBuckets ) );

	declareOptions( PT_POUR, "Pour", controls::kPourCount, controls::PourName );
	SetParamInfof( PT_COATING_VARIATION, "Coating Var", FF_TYPE_STANDARD );
	SetParamInfof( PT_BARE_EDGE, "Bare Edge", FF_TYPE_STANDARD );
	SetParamInfof( PT_DEFECTS, "Defects", FF_TYPE_STANDARD );

	SetParamInfof( PT_TONE, "Tone", FF_TYPE_STANDARD );
	SetParamInfof( PT_VIGNETTE, "Vignette", FF_TYPE_STANDARD );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	for( FFUInt32 i = PT_PLATE; i <= PT_SENSITIVITY; ++i )
		SetParamGroup( i, "Plate" );
	for( FFUInt32 i = PT_EXPOSURE; i <= PT_BUCKETS; ++i )
		SetParamGroup( i, "Exposure" );
	for( FFUInt32 i = PT_POUR; i <= PT_DEFECTS; ++i )
		SetParamGroup( i, "Coating" );
	for( FFUInt32 i = PT_TONE; i <= PT_MIX; ++i )
		SetParamGroup( i, "Output" );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Wetplate effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Wetplate::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = shaders::Vertex();
	struct
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &exposeShader, shaders::Expose(), "expose" },
		{ &sumShader, shaders::Sum(), "sum" },
		{ &coatingShader, shaders::Coating(), "coating" },
		{ &resampleShader, shaders::Resample(), "resample" },
		{ &plateShader, shaders::Plate(), "plate" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( vertex, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Wetplate: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Wetplate: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	bucketCount = 0;
	current     = -1;
	cell        = -1;
	for( Bucket& b : ring )
		b = Bucket();
	//takePending is deliberately left alone: a press that arrived before the
	//GL context existed is still a press. The sweep delivers one that way
	//through --set, and clearing it here read as a dead control.
	modeWas      = -1;
	takeArmed    = false;
	takeExposing = false;
	lastWidth = lastHeight = 0;
	coatingKey.clear();

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Wetplate::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//The unit voting is readout's, unchanged: the ratio of the host's clock
//delta to a steady clock's names the unit outright, and nothing plausible
//sits between 1 and 1000.
double Wetplate::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
void Wetplate::resetRing()
{
	for( int i = 0; i < model::kMaxBuckets; ++i )
	{
		ring[ i ]      = Bucket();
		needClear[ i ] = true;
	}
	current = -1;
	cell    = -1;
}

bool Wetplate::rescaleBuckets( int width, int height )
{
	//Every allocation first, then the passes: allocating unbinds the active
	//texture unit, and a resample that ran between the two would read
	//nothing on the frame that mattered.
	if( !scratch.Ensure( width, height, GL_R32F, PassBuffer::Sampling::Linear ) )
		return false;

	for( int i = 0; i < bucketCount; ++i )
	{
		PassBuffer& bucket = buckets[ i ];
		if( !bucket.IsValid() || ( static_cast< int >( bucket.GetWidth() ) == width && static_cast< int >( bucket.GetHeight() ) == height ) )
			continue;
		if( ring[ i ].start < 0.0 )
			continue;//empty: a fresh allocation is the right answer, below

		{
			ScopedFBOBinding fbo( scratch.GetGLID(), ScopedFBOBinding::RB_REVERT );
			scratch.ResizeViewPort();
			ScopedShaderBinding shader( resampleShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( bucket.TextureID() );
			resampleShader.Set( "Source", 0 );
			resampleShader.Set( "HalfTexel", 0.5f / static_cast< float >( bucket.GetWidth() ), 0.5f / static_cast< float >( bucket.GetHeight() ) );
			quad.Draw();
		}
		bucket.Swap( scratch );
		//The old bucket is now `scratch`, at the old size; the next Ensure
		//reallocates it at the new size for the next bucket. That
		//reallocation clears, which is fine: it is scratch.
		if( !scratch.Ensure( width, height, GL_R32F, PassBuffer::Sampling::Linear ) )
			return false;
	}
	return true;
}

//---------------------------------------------------------------------------
FFResult Wetplate::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//The host's viewport, read before anything of ours changes it.
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// The clock. Everything that reads it -- which bucket a frame goes in,
	// what a frame is worth, whether a take is over -- is reduced in double
	// here; nothing absolute crosses into GLSL.
	//---------------------------------------------------------------------
	const double now = nowSeconds();
	double dt        = model::kNominalFrame;
	if( lastNow >= 0.0 )
		dt = std::clamp( now - lastNow, model::kMinFrameDelta, model::kMaxFrameDelta );
	lastNow = now;

	if( std::getenv( "WETPLATE_TRACE" ) )
		std::fprintf( stderr, "trace frame now=%.6f dt=%.6f cell=%lld current=%d\n", now, dt, cell, current );
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const int plate         = controls::OptionIndex( params[ PT_PLATE ], controls::kPlateCount );
	const int light         = controls::OptionIndex( params[ PT_LIGHT ], controls::kLightCount );
	const float gamma       = controls::DevelopmentGamma( params[ PT_DEVELOPMENT ] );
	const float fog         = controls::DevelopmentFog( params[ PT_DEVELOPMENT ] );
	const float speed       = std::exp2( controls::SensitivityStops( params[ PT_SENSITIVITY ] ) );
	const double exposureS  = controls::ExposureSeconds( params[ PT_EXPOSURE ] );
	const int mode          = controls::OptionIndex( params[ PT_MODE ], controls::kModeCount );
	const int wantBuckets   = std::clamp( static_cast< int >( std::lround( params[ PT_BUCKETS ] ) ), controls::kMinBuckets, controls::kMaxBuckets );
	const int pour          = controls::OptionIndex( params[ PT_POUR ], controls::kPourCount );
	const float variation   = controls::CoatingVariation( params[ PT_COATING_VARIATION ] );
	const float bareEdge    = controls::BareEdgeFraction( params[ PT_BARE_EDGE ] );
	const int defects       = controls::DefectCount( params[ PT_DEFECTS ] );
	const float tone        = controls::Tone( params[ PT_TONE ] );
	const float vignette    = controls::VignetteLoss( params[ PT_VIGNETTE ] );
	const float mixAmount   = std::clamp( params[ PT_MIX ], 0.0f, 1.0f );

	//---------------------------------------------------------------------
	// The ring's bookkeeping, all of it before any GL call, so that every
	// clear it decides on happens before anything binds a texture.
	//
	// Continuous: the window is the last Exposure seconds, held as Buckets
	// bucket sums on a grid of Exposure / Buckets seconds anchored at host
	// time 0. A bucket is in the window while it opened no earlier than
	// Exposure seconds ago, which in steady state is all of them: the
	// window then runs from the oldest bucket's start to now, between
	// Exposure - one bucket and Exposure long. That is the bound the
	// harness's --bucket check measures.
	//
	// Take: the grid is anchored at the take, frames go in until Exposure
	// seconds have passed, and then the plate is developed and held.
	//---------------------------------------------------------------------
	if( wantBuckets != bucketCount )
	{
		//A structural change: start again. Slots past the new count are
		//released below.
		resetRing();
		bucketCount = wantBuckets;
	}
	if( mode != modeWas )
	{
		//Switching to Take caps the lens; switching back opens it again.
		resetRing();
		takeArmed    = false;
		takeExposing = false;
		modeWas      = mode;
	}
	if( mode == 1 && takePending )
	{
		resetRing();
		takeStart    = now;
		takeArmed    = true;
		takeExposing = true;
	}
	takePending = false;

	const double bucketSeconds = exposureS / bucketCount;
	double origin              = 0.0;
	bool accumulate            = true;
	if( mode == 1 )
	{
		origin = takeStart;
		if( takeExposing && now - takeStart >= exposureS - kGridSlack && ( perturb & model::kPerturbTakeNeverEnds ) == 0 )
			takeExposing = false;
		accumulate = takeArmed && takeExposing;
	}

	const bool oneFrame = ( perturb & model::kPerturbOneFrame ) != 0;
	if( accumulate )
	{
		const long long wantCell = static_cast< long long >( std::floor( ( now - origin ) / bucketSeconds + kGridSlack ) );
		if( wantCell != cell || current < 0 || oneFrame )
		{
			current            = ( current + 1 ) % bucketCount;
			needClear[ current ] = true;
			ring[ current ]      = Bucket();
			ring[ current ].start = oneFrame ? now : origin + static_cast< double >( wantCell ) * bucketSeconds;
			cell                 = wantCell;
		}
		ring[ current ].seconds += dt;
	}

	//The window's weights: 1 / seconds for a bucket in it, 0 otherwise.
	double windowSeconds = 0.0;
	bool inWindow[ model::kMaxBuckets ] = {};
	for( int i = 0; i < bucketCount; ++i )
	{
		const Bucket& b = ring[ i ];
		if( b.start < 0.0 || b.seconds <= 0.0 )
			continue;
		if( oneFrame )
			inWindow[ i ] = i == current;
		else if( mode == 1 )
			inWindow[ i ] = true;
		else
			inWindow[ i ] = b.start >= now - exposureS - kGridSlack;
		if( inWindow[ i ] )
			windowSeconds += b.seconds;
	}
	float weights[ model::kMaxBuckets ] = {};
	for( int i = 0; i < bucketCount; ++i )
		weights[ i ] = inWindow[ i ] && windowSeconds > 0.0 ? static_cast< float >( 1.0 / windowSeconds ) : 0.0f;

	//---------------------------------------------------------------------
	// Buffers. Every allocation happens here, before anything binds a
	// texture: allocating leaves the active unit bound to nothing, and the
	// symptom of getting the order wrong is correct on every frame except
	// the one that allocates.
	//
	// A resize carries the buckets across, resampled, unless the negative
	// control says to let them clear: the photofinish trap, guarded by
	// --resize.
	//---------------------------------------------------------------------
	const bool resized = lastWidth != 0 && ( lastWidth != width || lastHeight != height );
	if( resized && ( perturb & model::kPerturbResizeClears ) == 0 )
	{
		if( !rescaleBuckets( width, height ) )
		{
			diag::error( "could not resample the buckets to " + std::to_string( width ) + "x" + std::to_string( height ) );
			return FF_FAIL;
		}
	}
	lastWidth  = width;
	lastHeight = height;

	for( int i = 0; i < model::kMaxBuckets; ++i )
	{
		if( i >= bucketCount )
		{
			buckets[ i ].Destroy();
			continue;
		}
		const bool was = buckets[ i ].IsValid() && static_cast< int >( buckets[ i ].GetWidth() ) == width
		                 && static_cast< int >( buckets[ i ].GetHeight() ) == height;
		if( !buckets[ i ].Ensure( width, height, GL_R32F, PassBuffer::Sampling::Linear ) )
		{
			diag::error( "could not allocate bucket " + std::to_string( i ) + " at " + std::to_string( width ) + "x" + std::to_string( height ) );
			return FF_FAIL;
		}
		if( !was )
			needClear[ i ] = false;//freshly allocated, so already cleared
	}
	if( !exposure.Ensure( width, height, GL_R32F, PassBuffer::Sampling::Nearest )
	    || !coating.Ensure( width, height, GL_R32F, PassBuffer::Sampling::Nearest ) )
	{
		diag::error( "could not allocate the pass buffers at " + std::to_string( width ) + "x" + std::to_string( height ) );
		return FF_FAIL;
	}
	if( resized )
		diag::info( "raster " + std::to_string( width ) + "x" + std::to_string( height ) + ", " + std::to_string( StateBytesForTest() / 1048576 ) + " MB of plate state" );

	for( int i = 0; i < bucketCount; ++i )
		if( needClear[ i ] )
		{
			buckets[ i ].Clear();
			needClear[ i ] = false;
		}

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );

	//---------------------------------------------------------------------
	// 1. Expose: this frame into the current bucket, added.
	//---------------------------------------------------------------------
	if( accumulate )
	{
		const spectral::Weights& w = spectral::kWeights[ light ];
		ScopedFBOBinding fbo( buckets[ current ].GetGLID(), ScopedFBOBinding::RB_REVERT );
		buckets[ current ].ResizeViewPort();
		ScopedShaderBinding shader( exposeShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );

		exposeShader.Set( "InputTexture", 0 );
		glUniform1fv( exposeShader.FindUniform( "Basis" ), 7, w.w );
		exposeShader.Set( "Dt", static_cast< float >( dt ) );
		exposeShader.Set( "Perturb", perturb );

		glEnable( GL_BLEND );
		glBlendEquation( GL_FUNC_ADD );
		glBlendFunc( GL_ONE, GL_ONE );
		quad.Draw();
		glDisable( GL_BLEND );
	}

	//---------------------------------------------------------------------
	// 2. Sum: the window, weighted, to H.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( exposure.GetGLID(), ScopedFBOBinding::RB_REVERT );
		exposure.ResizeViewPort();
		ScopedShaderBinding shader( sumShader.GetGLID() );

		//Every one of the sixteen samplers bound to a real texture: a
		//sampler left on texture 0 is incomplete, and what an incomplete
		//texture returns is the driver's business even at weight 0.
		GLint units[ model::kMaxBuckets ];
		for( int i = 0; i < model::kMaxBuckets; ++i )
		{
			glActiveTexture( GL_TEXTURE0 + i );
			glBindTexture( GL_TEXTURE_2D, buckets[ i < bucketCount ? i : 0 ].TextureID() );
			units[ i ] = i;
		}
		glActiveTexture( GL_TEXTURE0 );
		glUniform1iv( sumShader.FindUniform( "Bucket" ), model::kMaxBuckets, units );
		glUniform1fv( sumShader.FindUniform( "Weight" ), model::kMaxBuckets, weights );
		sumShader.Set( "Count", bucketCount );
		quad.Draw();
		for( int i = model::kMaxBuckets - 1; i >= 0; --i )
		{
			glActiveTexture( GL_TEXTURE0 + i );
			glBindTexture( GL_TEXTURE_2D, 0 );
		}
		glActiveTexture( GL_TEXTURE0 );
	}

	//---------------------------------------------------------------------
	// 3. The coating, only when something it depends on has moved.
	//---------------------------------------------------------------------
	{
		const std::string key = std::to_string( width ) + "x" + std::to_string( height ) + "/" + std::to_string( pour ) + "/"
		                        + std::to_string( variation ) + "/" + std::to_string( bareEdge ) + "/" + std::to_string( defects )
		                        + "/" + std::to_string( perturb );
		if( key != coatingKey )
		{
			float ox, oy, dx, dy;
			controls::PourOrigin( pour, ox, oy );
			controls::PourDirection( pour, dx, dy );

			ScopedFBOBinding fbo( coating.GetGLID(), ScopedFBOBinding::RB_REVERT );
			coating.ResizeViewPort();
			ScopedShaderBinding shader( coatingShader.GetGLID() );
			coatingShader.Set( "Origin", ox, oy );
			coatingShader.Set( "Drain", dx, dy );
			coatingShader.Set( "Variation", variation );
			coatingShader.Set( "BareEdge", bareEdge );
			coatingShader.Set( "Size", static_cast< float >( width ), static_cast< float >( height ) );
			coatingShader.Set( "DefectCount", defects );
			coatingShader.Set( "Seed", 1 );
			coatingShader.Set( "Perturb", perturb );
			quad.Draw();
			coatingKey = key;
		}
	}

	//---------------------------------------------------------------------
	// 4. The plate, straight to the host.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( plateShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding exposureTexture( exposure.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding coatingTexture( coating.TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding sourceTexture( input.Handle );

		const float* silverNeutral = plate == 0 ? model::kTintypeSilverNeutral : plate == 1 ? model::kAmbrotypeSilverNeutral : model::kNegativeLightNeutral;
		const float* silverWarm    = plate == 0 ? model::kTintypeSilverWarm : plate == 1 ? model::kAmbrotypeSilverWarm : model::kNegativeLightWarm;
		const float* plateColour   = plate == 0 ? model::kTintypePlate : plate == 1 ? model::kAmbrotypePlate : model::kNegativeLightNeutral;

		plateShader.Set( "Exposure", 0 );
		plateShader.Set( "Coating", 1 );
		plateShader.Set( "Source", 2 );
		plateShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		plateShader.Set( "Speed", speed );
		plateShader.Set( "Toe", static_cast< float >( model::kToe ) );
		plateShader.Set( "Latitude", static_cast< float >( model::kLatitude ) );
		plateShader.Set( "Knee", static_cast< float >( model::kKnee ) );
		plateShader.Set( "Fog", fog );
		plateShader.Set( "Gamma", gamma );
		plateShader.Set( "Plate", plate );
		plateShader.Set( "SilverNeutral", silverNeutral[ 0 ], silverNeutral[ 1 ], silverNeutral[ 2 ] );
		plateShader.Set( "SilverWarm", silverWarm[ 0 ], silverWarm[ 1 ], silverWarm[ 2 ] );
		plateShader.Set( "PlateColour", plateColour[ 0 ], plateColour[ 1 ], plateColour[ 2 ] );
		plateShader.Set( "Tone", tone );
		plateShader.Set( "VignetteLoss", vignette );
		plateShader.Set( "MixAmount", mixAmount );
		plateShader.Set( "Probe", probe );
		plateShader.Set( "Perturb", perturb );
		quad.Draw();
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Wetplate::DeInitGL()
{
	exposeShader.FreeGLResources();
	sumShader.FreeGLResources();
	coatingShader.FreeGLResources();
	resampleShader.FreeGLResources();
	plateShader.FreeGLResources();
	quad.Release();
	for( PassBuffer& b : buckets )
		b.Destroy();
	scratch.Destroy();
	exposure.Destroy();
	coating.Destroy();
	coatingKey.clear();
	bucketCount = 0;
	current     = -1;

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Wetplate::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_TAKE )
	{
		//An event arrives as 1.0 on press and 0.0 on release; the take is
		//the press. Edge-triggered, so a host restating 1.0 does not fire
		//it again.
		if( value >= 0.5f && takeWas < 0.5f )
			takePending = true;
		takeWas = value;
		return FF_SUCCESS;
	}

	params[ index ] = value;
	return FF_SUCCESS;
}

float Wetplate::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Wetplate::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Wetplate::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Wetplate::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Wetplate::SetPerturbForTest( int bits )
{
	perturb = bits;
}

void Wetplate::SetProbeForTest( int p )
{
	probe = p;
}

size_t Wetplate::StateBytesForTest() const
{
	size_t bytes = 0;
	auto count = [ &bytes ]( const PassBuffer& b ) {
		if( b.IsValid() )
			bytes += static_cast< size_t >( b.GetWidth() ) * b.GetHeight() * 4;
	};
	for( int i = 0; i < bucketCount; ++i )
		count( buckets[ i ] );
	count( exposure );
	count( coating );
	return bytes;
}

void Wetplate::WindowForTest( int& bucketsHeld, double& seconds ) const
{
	bucketsHeld = 0;
	seconds     = 0.0;
	for( int i = 0; i < bucketCount; ++i )
		if( ring[ i ].start >= 0.0 && ring[ i ].seconds > 0.0 )
		{
			++bucketsHeld;
			seconds += ring[ i ].seconds;
		}
}
