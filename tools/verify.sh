#!/usr/bin/env bash
#
# Everything that can be checked without a host, in one go, in the order that
# fails fastest.
#
#   tools/verify.sh
#
# Each check answers a question none of the others can:
#
#   build         a FRESH universal Release build. Not the dev build: CMake
#                 latches the architecture list at the first target, so the
#                 only build worth measuring is one configured from nothing.
#   weights       the committed spectral weights recompute from the script's
#                 tables, byte for byte. The plugin compiles Spectral.h; the
#                 sources are in tools/spectral_weights.py.
#   shaders       does every shader compile, through a real GLSL compiler,
#                 before a host has to find out. The shaders are assembled at
#                 run time from one model library, so the text compiled here is
#                 what `wttest --dump-shaders` writes: the exact strings the
#                 plugin hands the driver. Then a grep for every GLSL 4.10
#                 reserved word used as an identifier, because Apple's compiler
#                 and glslc accept some (`packed`) that Mesa refuses.
#   demo          the browser demo's copy of every shader is still the
#                 plugin's, character for character (demo/tools/check_shaders.py).
#   physics       every harness check, at TWO rasters: 320x180, which is what
#                 CI renders at, and 1280x720. A check that holds at one raster
#                 was fitted to it. Each is measured out of the picture:
#                   --spectral  pure R, G, B expose in the ratio of the weights
#                   --ghost     a moving square's smear is (frames covered) / n
#                   --bucket    the bucketed window is the box mean within one
#                               bucket's edge, and exactly the stated rule
#                   --curve     a grey wedge maps through the stated curve
#                   --drainage  density along the drain axis is a + b sqrt( s )
#                   --take      the plate stops exactly Exposure after the Take
#                   --resize    a resize mid-exposure carries the exposure across
#                   --negative  every one of those FAILS on a perturbed model
#   pipe          the fleet's frame contract, plus what a closed stdout, an
#                 unknown cue, a stepped option and an event cue do.
#   sweep         does every control change the picture. A GLSL uniform whose
#                 name does not match the C++ is ignored without a word.
#   bench         the render cost and the state held, for the record.
#   registration  does the bundle contain a plugin at all -- a file-scope
#                 CFFGLPluginInfo nothing names, which a linker may drop while
#                 still producing a bundle that loads and exports plugMain.
#   lipo          is the build really universal.
#   plist         does CFBundleExecutable name the binary that is on disk.
#   codesign      the exact command the release job runs, against a copy.
#   oxbow         a real FFGL host loads the bundle and reports the name, id
#                 and type it sees -- the name field is not null-terminated
#                 and a host truncates silently past 16 characters.
#
set -uo pipefail

cd "$(dirname "$0")/.."

BUILD="${BUILD:-build-universal}"
failures=0

step() { printf '\n\033[1m== %s\033[0m\n' "$1"; }
pass() { printf '   \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '   \033[31mFAIL\033[0m %s\n' "$1"; failures=$(( failures + 1 )); }

step "build (fresh universal Release, $BUILD)"
rm -rf "$BUILD"
if cmake -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$BUILD" --parallel >/dev/null 2>&1; then
	pass "builds"
else
	fail "build failed -- run: cmake -B $BUILD -DCMAKE_BUILD_TYPE=Release && cmake --build $BUILD"
	exit 1
fi

WTTEST="$BUILD/wttest"

step "weights"
if out=$(python3 tools/spectral_weights.py --check 2>&1); then
	pass "$out"
else
	fail "$out"
fi

#---------------------------------------------------------------------------
# Every shader, through a real GLSL compiler.
#
# --target-env=opengl4.5 with -fauto-map-locations: glslc targets SPIR-V, which
# demands an explicit layout( location ) on every uniform and varying. Those are
# Vulkan rules and not GLSL ones, and without the flag every shader "fails" for
# reasons that have nothing to do with the code.
#
# glslc is optional -- `brew install shaderc` -- so a machine without it skips
# rather than fails. No shaders at all is a FAILURE: it means the dump broke.
#---------------------------------------------------------------------------
step "shaders"
dir="$( mktemp -d )"
"$WTTEST" --dump-shaders "$dir" >/dev/null
if ! command -v glslc >/dev/null 2>&1; then
	printf '   skipped: glslc not installed (brew install shaderc)\n'
else
	n=0; bad=0
	for shader in "$dir"/*.vert "$dir"/*.frag; do
		[ -e "$shader" ] || continue
		n=$(( n + 1 ))
		if ! glslc --target-env=opengl4.5 -fauto-map-locations "$shader" -o /dev/null 2>"$dir/err"; then
			printf '   %s does not compile\n' "$( basename "$shader" )"
			sed "s|$dir/||; s|^|      |" "$dir/err"
			bad=$(( bad + 1 ))
		fi
	done
	if [ "$n" -eq 0 ]; then
		fail "no shaders were dumped"
	elif [ "$bad" -eq 0 ]; then
		pass "all $n shaders compile"
	else
		fail "$bad of $n shaders do not compile"
	fi
fi
# The GLSL 4.10 reserved words (s3.6 of the spec: keywords reserved for future
# use, plus the ones the fleet has been bitten by) as identifiers. Apple's
# compiler and glslc accept `packed`; Mesa's llvmpipe, which the Arena gate
# runs on, does not. Matched as whole words followed by something an
# identifier is followed by, on lines that are not comments.
reserved='common|partition|active|asm|class|union|enum|typedef|template|this|packed|resource|goto|inline|noinline|public|static|extern|external|interface|long|short|half|fixed|unsigned|superp|input|output|hvec2|hvec3|hvec4|fvec2|fvec3|fvec4|sampler3DRect|filter|sizeof|cast|namespace|using|row_major|patch|sample|subroutine'
hits=$( cat "$dir"/*.vert "$dir"/*.frag | sed 's|//.*||' | grep -nwE "($reserved)" | grep -vE '^\s*$' || true )
if [ -z "$hits" ]; then
	pass "no GLSL 4.10 reserved word used as an identifier"
else
	fail "a GLSL 4.10 reserved word appears in a shader:"
	printf '%s\n' "$hits" | sed 's/^/      /'
fi
rm -rf "$dir"

#---------------------------------------------------------------------------
# The browser demo's copy of every shader is the plugin's, character for
# character. A drifted comment counts. It says nothing about the page's PORT
# of the CPU half (the clock, the ring, the window, the take, the laws) or the
# page's own sum pass; only a reader checks those.
#---------------------------------------------------------------------------
step "demo shaders"
if [ -f demo/tools/check_shaders.py ]; then
	if out=$(python3 demo/tools/check_shaders.py 2>&1); then
		pass "$( printf '%s\n' "$out" | tail -1 )"
	else
		fail "the demo's shaders have drifted from source/Shaders.cpp"
		printf '%s\n' "$out" | tail -12
	fi
else
	printf '   skipped: no demo/\n'
fi

for size in 320x180 1280x720; do
	step "physics at $size"
	for check in spectral ghost bucket curve drainage take resize negative; do
		if out=$("$WTTEST" --$check --size $size 2>&1); then
			pass "wttest --$check: $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
		else
			fail "wttest --$check at $size"
			printf '%s\n' "$out" | sed 's/^/      /'
		fi
	done
done

# The whole physics list again on Apple's SOFTWARE renderer, which is what
# GitHub's macOS runners have. It is not repeatable at the last bit (repousse's
# resize check failed CI by one ulp), so a check that asserts exactness on this
# Mac's GPU is found here before CI finds it.
step "physics at 320x180 on the software renderer (CI's)"
for check in spectral ghost bucket curve drainage take resize negative; do
	if out=$(WTTEST_RENDERER=software "$WTTEST" --$check --size 320x180 2>&1); then
		pass "wttest --$check (software): $( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
	else
		fail "wttest --$check at 320x180 on the software renderer -- run: WTTEST_RENDERER=software $WTTEST --$check --size 320x180"
		printf '%s\n' "$out" | sed 's/^/      /'
	fi
done

step "names"
if out=$("$WTTEST" --names 2>&1); then
	pass "$( printf '%s\n' "$out" | grep -v '^$' | tail -1 )"
else
	fail "wttest --names"
	printf '%s\n' "$out" | sed 's/^/      /'
fi

#---------------------------------------------------------------------------
# --pipe, in the fleet's frame format. Two and a half frames in must be exactly
# two frames out and a clean exit -- a partial frame is the end of the stream,
# never a frame -- and a cue naming no parameter must be refused rather than
# silently doing nothing to a take.
#---------------------------------------------------------------------------
step "pipe"
W=64; H=36
frame=$(( W * H * 4 ))
raw=$( mktemp ); cues=$( mktemp ); out=$( mktemp )
head -c $(( frame * 5 / 2 )) /dev/zero > "$raw"
got=$( "$WTTEST" --pipe --size ${W}x${H} < "$raw" 2>/dev/null | wc -c | tr -d ' ' )
status=${PIPESTATUS[0]}
if [ "$status" -eq 0 ] && [ "$got" = "$(( frame * 2 ))" ]; then
	pass "2.5 frames in, exactly 2 frames out, clean exit"
else
	fail "2.5 frames in gave $got bytes out (want $(( frame * 2 ))), exit $status"
fi
# Read from a file, not a pipe: a writer killed by SIGPIPE would fail the
# pipeline whatever wttest did, and the refusal would pass for the wrong reason.
printf '0 No Such Control 0.5\n' > "$cues"
"$WTTEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 2 ]; then
	pass "a cue naming no parameter is refused (exit 2)"
else
	fail "a cue naming no parameter gave exit $status, not 2"
fi
# A reader that hangs up early (`| head -c 1`, ffmpeg dying) must end the run
# with exit 1 and a message, not SIGPIPE's silent 141.
head -c $(( frame * 20 )) /dev/zero > "$raw"
"$WTTEST" --pipe --size ${W}x${H} < "$raw" 2>/dev/null | head -c 1 >/dev/null
status=${PIPESTATUS[0]}
if [ "$status" -eq 1 ]; then
	pass "a closed stdout ends the run with exit 1, not SIGPIPE"
else
	fail "a closed stdout gave exit $status, not 1"
fi
# A failed render ends the run with exit 1 too.
"$WTTEST" --pipe --size ${W}x${H} --fail-render-at 3 < "$raw" >/dev/null 2>&1
status=$?
if [ "$status" -eq 1 ]; then
	pass "a failed render ends the run with exit 1"
else
	fail "a failed render gave exit $status, not 1"
fi
# An option STEPS between cues: Plate 0 at frame 0 and 2 at frame 4 must give
# frames 0..3 the same plate (a still white input exposes the same picture
# every frame) and frame 4 a different one. A ramp would put Ambrotype on
# frame 2.
python3 -c "import sys; sys.stdout.buffer.write(bytes([255,255,255,255]) * ($W * $H * 6))" > "$raw"
printf '0 Plate 0\n4 Plate 2\n' > "$cues"
"$WTTEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" > "$out" 2>/dev/null
if python3 - "$out" $frame <<'PY'
import sys
data = open(sys.argv[1], 'rb').read(); n = int(sys.argv[2])
f = [data[i * n:(i + 1) * n] for i in range(6)]
ok = len(data) == 6 * n and f[1] == f[0] and f[2] == f[0] and f[3] == f[0] and f[4] != f[0] and f[5] == f[4]
sys.exit(0 if ok else 1)
PY
then
	pass "an option cue steps (frames 0-3 one plate, 4-5 the next), no ramp between"
else
	fail "an option cue did not step -- see the loadScript/valueAt kinds in tools/wttest/main.cpp"
fi
# An event cue fires on its frame only: in Take mode with the Take at frame 2,
# frames 0 and 1 are the unexposed plate and frames 2 on are exposed.
printf '0 Mode 1\n2 Take 1\n' > "$cues"
"$WTTEST" --pipe --size ${W}x${H} --script "$cues" < "$raw" > "$out" 2>/dev/null
if python3 - "$out" $frame <<'PY'
import sys
data = open(sys.argv[1], 'rb').read(); n = int(sys.argv[2])
f = [data[i * n:(i + 1) * n] for i in range(6)]
ok = len(data) == 6 * n and f[1] == f[0] and f[2] != f[0] and f[3] != f[0]
sys.exit(0 if ok else 1)
PY
then
	pass "an event cue fires on its frame: Take at 2 leaves 0-1 unexposed and exposes 2 on"
else
	fail "an event cue did not fire where it should"
fi
rm -f "$raw" "$cues" "$out"

step "sweep"
if out=$(python3 tools/sweep.py --binary "$WTTEST" 2>/dev/null); then
	pass "$( printf '%s\n' "$out" | tail -1 )"
else
	fail "tools/sweep.py reports a dead control"
	printf '%s\n' "$out" | grep -E '^DEAD|DEAD CONTROLS' | sed 's/^/      /'
fi

step "bench (for the record)"
"$WTTEST" --bench --frames 60 2>&1 | sed -n '3,6p' | sed 's/^/   /'

BUNDLE="$BUILD/Wetplate.bundle"
BIN="$BUNDLE/Contents/MacOS/Wetplate"

if [ "$(uname)" = "Darwin" ] && [ -d "$BUNDLE" ]; then
	step "registration"
	# `nm ... | grep -q X` FAILS when grep FINDS its match under `set -o pipefail`:
	# grep exits at once, nm takes SIGPIPE, and the pipeline reports failure.
	# Capture and match instead of piping.
	syms=$(nm -gU "$BIN" 2>/dev/null)
	case "$syms" in
		*_plugMain*) pass "exports plugMain" ;;
		*) fail "no plugMain -- the bundle contains no plugin" ;;
	esac

	step "lipo"
	archs=$(lipo -archs "$BIN" 2>/dev/null)
	case "$archs" in *arm64*) pass "arm64 present" ;; *) fail "no arm64 (got: $archs)" ;; esac
	case "$archs" in *x86_64*) pass "x86_64 present" ;; *) fail "no x86_64 (got: $archs) -- a universal build was asked for" ;; esac

	step "plist"
	exe=$(/usr/libexec/PlistBuddy -c "Print :CFBundleExecutable" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	ident=$(/usr/libexec/PlistBuddy -c "Print :CFBundleIdentifier" "$BUNDLE/Contents/Info.plist" 2>/dev/null)
	if [ -n "$exe" ] && [ -f "$BUNDLE/Contents/MacOS/$exe" ]; then
		pass "CFBundleExecutable ($exe) is on disk"
	else
		fail "CFBundleExecutable is '$exe' but no such binary exists -- codesign will fail after the tag"
	fi
	if [ "$ident" = "com.stoatworks.ffgl.wetplate" ]; then
		pass "CFBundleIdentifier is $ident"
	else
		fail "CFBundleIdentifier is '$ident'"
	fi

	step "codesign"
	tmp=$(mktemp -d)
	cp -R "$BUNDLE" "$tmp/" 2>/dev/null
	if codesign --force --sign - --timestamp=none "$tmp/Wetplate.bundle" >/dev/null 2>&1; then
		pass "ad-hoc signs (the command the release job runs)"
	else
		fail "ad-hoc signing failed"
	fi
	rm -rf "$tmp"

	step "oxbow"
	OXBOW="${OXBOW:-../oxbow/build/oxbow}"
	[ -x "$OXBOW" ] || OXBOW="$HOME/Projects/resolume/oxbow/build/oxbow"
	if [ -x "$OXBOW" ]; then
		probe=$("$OXBOW" probe "$BUNDLE" 2>&1)
		for want in "name:        SW Wetplate" "id:          WT01" "type:        effect"; do
			case "$probe" in
				*"$want"*) pass "host sees '$want'" ;;
				*) fail "host does not see '$want' -- see: $OXBOW probe $BUNDLE" ;;
			esac
		done
		self=$("$OXBOW" selftest "$BUNDLE" 2>&1)
		case "$self" in
			*"selftest:    PASS"*) pass "instantiates through plugMain and renders 120 frames" ;;
			*) fail "oxbow selftest did not pass -- see: $OXBOW selftest $BUNDLE" ;;
		esac
	else
		printf '   skipped: oxbow not built at %s\n' "$OXBOW"
	fi
fi

printf '\n'
if [ "$failures" -eq 0 ]; then
	printf '\033[32mall checks passed\033[0m\n'
else
	printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ? 1 : 0 ))
