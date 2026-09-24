"""The demo's shaders must be the plugin's shaders, character for character.

    python3 demo/tools/check_shaders.py

Called from `tools/verify.sh`. Exit code 1 means the two copies have drifted.
Galvo's check, by way of teletext and toner, in this repo's shape.

------------------------------------------------------------------- why

`demo/plugin.js` holds seven GLSL bodies and so does `source/Shaders.cpp`.
That is two copies of the same text, and two copies drift -- quietly, because a
demo that renders a *plausible* plate looks exactly like a demo that renders
the right one. The whole claim of these pages is that they run the plugin's own
shaders rather than something reimplemented to look similar, so the claim needs
something enforcing it.

Nothing else can. `wttest` drives the real plugin class through a real FFGL
sequence and has no idea this page exists, and `tools/verify.sh`'s glslc step
runs over what `wttest --dump-shaders` writes and never looks at the JS copy.

------------------------------------------------------------------- what it does

Pulls each `R"( ... )"` body out of the C++ -- the vertex body, the `kModel`
library, and the five pass bodies -- and each matching backtick literal out of
`plugin.js`, and compares them exactly: no whitespace normalisation, no
comment stripping. The plugin assembles every shader as
`kVersion + ( withModel ? kModel : "" ) + body` (Shaders.cpp's `assemble`);
the page does the same with its `assemble`, so the bodies are what is compared
and the assembly is checked here as a literal on both sides, along with which
bodies take the model.

The sum body is checked like the others although the page never runs it: it
will not compile in GLSL ES 3.00 (a sampler array indexed by a loop variable),
so the page carries it, reports the compiler's refusal, and sums with a pass
of its own. The check keeps the carried copy honest; it says nothing about the
page's replacement, which is the page's and is stated as such.

The one transformation is a decode, not a normalisation. A backtick cannot
appear raw inside a JavaScript template literal, so `plugin.js` would have to
escape one as \\`; none of these shaders quotes one today, but a comment could
start to. This unescapes that and *rejects any other backslash on the JS side*;
there are none anywhere in the C++, so a second escape could only be somebody
hiding a difference. A `${` would be interpolated by the literal, so it is
refused on the C++ side before it can become a silent difference.

------------------------------------------------------------------- what it cannot

Nothing here checks the *ported* half. The clock, the ring's bookkeeping, the
window's weights, the take, the coating key, the resample on a resize, the
control laws and the copied spectral weights in plugin.js are a hand
translation of Wetplate.cpp, Controls.cpp, Model.h and Spectral.h, and only a
reader can tell whether they still agree. When you change one of those, change
it here too -- and remember that a wrong port shows up on the page as a picture
that is subtly wrong, which nobody will notice.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))

# JS constant, C++ file, C++ symbol. Seven bodies making six shaders: the
# model library is prepended to three of the five pass bodies.
SHADERS = [
    ("VERTEX_BODY", "source/Shaders.cpp", "kVertexBody"),
    ("MODEL", "source/Shaders.cpp", "kModel"),
    ("EXPOSE_BODY", "source/Shaders.cpp", "kExposeBody"),
    ("SUM_BODY", "source/Shaders.cpp", "kSumBody"),
    ("COATING_BODY", "source/Shaders.cpp", "kCoatingBody"),
    ("RESAMPLE_BODY", "source/Shaders.cpp", "kResampleBody"),
    ("PLATE_BODY", "source/Shaders.cpp", "kPlateBody"),
]

# How the plugin assembles a shader, and how the page must.
VERSION_CPP = 'const char* const kVersion = "#version 410 core\\n";'
ASSEMBLE_CPP = [
    "std::string s = kVersion;",
    "if( withModel )",
    "s += kModel;",
    "s += body;",
]
ASSEMBLE_JS = "const assemble = (body, withModel) => '#version 410 core\\n' + (withModel ? MODEL : '') + body;"

# Which bodies take the model, on both sides. Shaders.cpp's six accessors and
# the page's six constants.
WITH_MODEL_CPP = [
    "return assemble( kVertexBody, false );",
    "return assemble( kExposeBody, true );",
    "return assemble( kSumBody, false );",
    "return assemble( kCoatingBody, true );",
    "return assemble( kResampleBody, false );",
    "return assemble( kPlateBody, true );",
]
WITH_MODEL_JS = [
    "const VERTEX = assemble(VERTEX_BODY, false);",
    "const EXPOSE = assemble(EXPOSE_BODY, true);",
    "const SUM = assemble(SUM_BODY, false);",
    "const COATING = assemble(COATING_BODY, true);",
    "const RESAMPLE = assemble(RESAMPLE_BODY, false);",
    "const PLATE = assemble(PLATE_BODY, true);",
]


def from_cpp(path, symbol):
    with open(os.path.join(REPO, path)) as handle:
        source = handle.read()
    match = re.search(r'(?:static )?const char\* const ' + symbol + r' = R"\((.*?)\)";', source, re.S)
    if match is None:
        return None
    return match.group(1)


def from_js(source, name):
    match = re.search(r'^const ' + name + r' = `(.*?)`;$', source, re.S | re.M)
    if match is None:
        return None, None

    body = match.group(1)

    # Undo the one escape the literal needs, and refuse the rest. The C++ carries
    # no backslash at all, so a stray one here is either a typo or a difference
    # being smuggled through the decoder.
    stray = re.search(r"\\(?!`)", body)
    if stray is not None:
        upto = body[: stray.start()]
        return None, f"backslash that is not an escaped backtick, at line {upto.count(chr(10)) + 1}"

    return body.replace("\\`", "`"), None


def main():
    with open(os.path.join(REPO, "demo", "plugin.js")) as handle:
        js = handle.read()
    with open(os.path.join(REPO, "source", "Shaders.cpp")) as handle:
        cpp_all = handle.read()

    problems = 0
    cpp_stripped = "\n".join(line.strip() for line in cpp_all.splitlines())
    if VERSION_CPP not in cpp_stripped or any(line not in cpp_stripped for line in ASSEMBLE_CPP) or ASSEMBLE_JS not in js:
        print("FAIL  the way the plugin assembles its shaders is not the way the page does")
        problems += 1
    else:
        print("ok    assemble             matches Shaders.cpp's assemble")

    if any(line not in cpp_stripped for line in WITH_MODEL_CPP) or any(line not in js for line in WITH_MODEL_JS):
        print("FAIL  which bodies take the model library differs between Shaders.cpp and the page")
        problems += 1
    else:
        print("ok    model library         prepended to expose, coating and plate on both sides")

    for name, path, symbol in SHADERS:
        cpp_text = from_cpp(path, symbol)
        js_text, complaint = from_js(js, name)

        if cpp_text is None:
            print(f"FAIL  {symbol} not found in {path}")
            problems += 1
            continue
        if "${" in cpp_text:
            print(f"FAIL  {symbol} contains ${{, which a template literal would interpolate")
            problems += 1
            continue
        if complaint is not None:
            print(f"FAIL  {name} in demo/plugin.js has a {complaint}")
            problems += 1
            continue
        if js_text is None:
            print(f"FAIL  {name} not found in demo/plugin.js")
            problems += 1
            continue

        if cpp_text == js_text:
            print(f"ok    {name:<20} matches {symbol} ({len(cpp_text)} chars)")
            continue

        problems += 1
        print(f"FAIL  {name} has drifted from {symbol} in {path}")

        cpp_lines = cpp_text.splitlines()
        js_lines = js_text.splitlines()
        for i in range(max(len(cpp_lines), len(js_lines))):
            a = cpp_lines[i] if i < len(cpp_lines) else "<missing>"
            b = js_lines[i] if i < len(js_lines) else "<missing>"
            if a != b:
                print(f"        first difference at line {i + 1}")
                print(f"          C++: {a}")
                print(f"          js : {b}")
                break

    print()
    if problems:
        print(f"{problems} shader(s) differ -- copy the C++ across, do not edit plugin.js by hand")
        return 1

    print(f"all {len(SHADERS)} shader bodies (6 shaders) are identical to the plugin's")
    return 0


if __name__ == "__main__":
    sys.exit(main())
