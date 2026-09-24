#pragma once

#include "Model.h"
#include "PassBuffer.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <string>

/**
	Wetplate -- a collodion wet plate, blue-sensitive and seconds long, as an
	FFGL effect.

	**The one idea.** Two properties of the plate make the look, and both
	are in the plate rather than in a grade. It sees only ultraviolet and
	blue, so reds and skin go dark and a blue sky blows to white. And it is
	slow: it integrates light for seconds, so whatever moved is a ghost in
	proportion to how long it stayed, and whatever held still is sharp. Then
	it was hand-poured, so the coating is thicker toward the drain corner,
	bare where the pour never reached, and marked by dust the silver bath
	ran around.

	**Five passes**, in `Shaders.h`. The window is a ring of `Buckets`
	bucket sums, each the sum of whole frames' exposure times their seconds,
	so memory is B frames whatever the exposure; the window is exact to one
	bucket's width. Frame-relative time is reduced in double here; nothing
	absolute crosses into GLSL. See AGENTS.md for the traps.
*/
class Wetplate : public CFFGLPlugin
{
public:
	Wetplate();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Clock test hook: the harness DECLARES its unit rather than leaving the
	/// voting to infer one.
	void SetClockScaleForTest( double scale );

	/// Negative-control hooks, a bitmask of `model::Perturb`. Always 0 in
	/// the plugin; each bit perturbs the model so a check can be shown to fail.
	void SetPerturbForTest( int bits );

	/// Probe hook: `model::kProbeRaw` makes the plate pass write raw floats.
	void SetProbeForTest( int probe );

	/// Bytes of GPU state held across frames right now: every bucket in
	/// use, the exposure sum and the coating, colour texture only. The SDK's
	/// FBO also attaches a depth renderbuffer to each, which this does not
	/// count and the plugin never uses.
	size_t StateBytesForTest() const;

	/// The window as the plugin has it: how many buckets hold seconds, and
	/// the seconds in the window. For `--bucket`.
	void WindowForTest( int& bucketsHeld, double& seconds ) const;

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Plate
		PT_PLATE,
		PT_LIGHT,
		PT_DEVELOPMENT,
		PT_SENSITIVITY,

		//Exposure
		PT_EXPOSURE,
		PT_MODE,
		PT_TAKE,
		PT_BUCKETS,

		//Coating
		PT_POUR,
		PT_COATING_VARIATION,
		PT_BARE_EDGE,
		PT_DEFECTS,

		//Output
		PT_TONE,
		PT_VIGNETTE,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// The host's clock in seconds, whatever unit it arrived in.
	double nowSeconds();

	/// Empty the ring: every bucket in use is cleared on the GPU and forgets
	/// its seconds. Called before anything binds a texture.
	void resetRing();

	/// Carry every bucket across to a new raster, resampled, instead of
	/// letting a reallocation clear it. The photofinish trap.
	bool rescaleBuckets( int width, int height );

	ffglex::FFGLShader exposeShader;
	ffglex::FFGLShader sumShader;
	ffglex::FFGLShader coatingShader;
	ffglex::FFGLShader resampleShader;
	ffglex::FFGLShader plateShader;
	ffglex::FFGLScreenQuad quad;

	/// The ring. A fixed array and not a vector: FFGLFBO's implicit copy
	/// would duplicate GL ids without the objects behind them.
	wetplate::PassBuffer buckets[ wetplate::model::kMaxBuckets ];
	wetplate::PassBuffer scratch;  ///< the other side of a resize
	wetplate::PassBuffer exposure; ///< H
	wetplate::PassBuffer coating;  ///< h times the masks

	struct Bucket
	{
		double start   = -1.0;///< host seconds when it opened; < 0 for empty
		double seconds = 0.0; ///< the seconds it holds
	};
	Bucket ring[ wetplate::model::kMaxBuckets ];
	int bucketCount = 0;   ///< buckets in use
	int current     = -1;  ///< the slot taking frames
	long long cell  = -1;  ///< the grid cell `current` covers
	bool needClear[ wetplate::model::kMaxBuckets ] = {};

	int modeWas       = -1;
	bool takePending  = false;
	bool takeArmed    = false;///< a take is exposing or held
	bool takeExposing = false;
	double takeStart  = 0.0;
	float takeWas     = 0.0f;

	int lastWidth  = 0;
	int lastHeight = 0;
	std::string coatingKey;

	//--- the clock (readout's unit voting) -----------------------------------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastNow      = -1.0;
	int clockFrames     = 0;

	int perturb = 0;
	int probe   = 0;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
