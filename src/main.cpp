/*
 * Copyright (C) 2025 Daniel Gibson
 *
 * Released under MIT License, see Licenses.txt
 */

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/gl.h>

#include <ktx.h>

#ifdef TV_USE_NFD
#include <nfd.h>
#endif

#include <math.h>
#include <stdio.h>

#include <initializer_list>

#include "texview.h"
#include "version.h"

#include "data/texview_icon.h"
#include "data/texview_icon32.h"

static GLFWwindow* glfwWindow;

static ImVec4 clear_color(0.45f, 0.55f, 0.60f, 1.00f);

// TODO: should probably support more than one texture eventually..
static texview::Texture curTex;

static GLuint shaderProgram = 0;

static bool showImGuiDemoWindow = false;
static bool showAboutWindow = false;
static bool showGLSLeditWindow = false;

static float imGuiMenuWidth = 0.0f;
static bool imguiMenuCollapsed = false;

static double zoomLevel = 1.0;
static double transX = 10;
static double transY = 10;
static bool dragging = false;
static ImVec2 lastDragPos;

static bool linearFilter = false;
static int mipmapLevel = -1; // -1: auto, otherwise enforce that level
static int overrideSRGB = -1; // -1: auto, 0: force disable, 1: force enable
static int overrideAlpha = -1; // -1: auto, 0: force disable alpha blending, 1: force enable

static int cubeCrossVariant = 0; // 0-3
static int textureArrayIndex = 0;
static std::string texSampleAndNormalize; // used in shader and shown in GLSL (swizzle) editor
static std::string swizzle; // used in shader, modifiable by user
// something like "b1ga", transformed to swizzle with SetSwizzleFromSimple()
static char simpleSwizzle[5] = {};
static bool useSimpleSwizzle = true;


static enum ViewMode {
	SINGLE,
	MIPMAPS_COMPACT,
	MIPMAPS_ROW,
	MIPMAPS_COLUMN,
	TILED
} viewMode;
static bool viewAtSameSize = true;
static int spacingBetweenMips = 2;
static int numTiles[2] = {2, 2};

static void glfw_error_callback(int error, const char* description)
{
	errprintf("GLFW Error: %d - %s\n", error, description);
}

#ifdef __GNUC__
static void AppendFormatted(std::string& str, const char* fmt, ...)
__attribute__((format(printf, 2, 3)));
// NOTE: For some reason, MSVC only supports printf-format checking for
//       their own functions, not for user-defined ones -_-
#endif

static void AppendFormatted(std::string& str, const char* fmt, ...)
{
	va_list ap;
	char buf[2048];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	str += buf;
}

static void ZoomFitToWindow(GLFWwindow* window, float tw, float th, bool isCube)
{
	if(isCube) {
		// shown as cross lying on the side => 4 wide, 3 high
		tw *= 4.0f;
		th *= 3.0f;
	}
	int display_w, display_h;
	glfwGetFramebufferSize(window, &display_w, &display_h);
	double winW = display_w - imGuiMenuWidth;
	double zw = winW / tw;
	double zh = display_h / th;
	if(zw < zh) {
		zoomLevel = zw;
		transX = 0;
		transY = floor(0.5 * (display_h/zw - th));
	} else {
		zoomLevel = zh;
		transX = isCube ? 0.0 : floor(0.5 * (winW/zh - tw));
		transY = 0;
	}
}

static const char* vertexShaderSrc = R"(
out vec4 texCoord;
// TODO: a way to pass cube face num or array slice num? or is texCoord.z(?) stable enough?
void main()
{
	gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;
	texCoord = gl_MultiTexCoord0;
}
)";

// Note: before this something like "uniform sampler2D tex0;" is needed,
//       setting that in UpdateShaders() based on type
static const char* fragShaderStart = R"(
in vec4 texCoord;
out vec4 OutColor;
void main()
{
)";

// ... here UpdateShaders() adds a line like "	vec4 c = texture(tex0, texCoord.st);\n"
// ... at this point swizzling could happen ("	c = c.agbr;") - generate that dynamically

// Note: only indenting with single space so it looks better in the advanced swizzle editor
static const char* fragShaderEnd =  R"(
 OutColor = c;
}
)";
//"\n}\n";

static GLuint
CompileShader(GLenum shaderType, std::initializer_list<const char*> shaderSources)
{
	GLuint shader = glCreateShader(shaderType);
	glShaderSource(shader, shaderSources.size(), shaderSources.begin(), NULL);
	glCompileShader(shader);
	GLint status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if(status != GL_TRUE) {
		char buf[2048];
		char* bufPtr = buf;
		int bufLen = sizeof(buf);
		GLint infoLogLength;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &infoLogLength);
		if(infoLogLength >= bufLen) {
			bufPtr = (char*)malloc(infoLogLength+1);
			bufLen = infoLogLength+1;
			if(bufPtr == NULL) {
				bufPtr = buf;
				bufLen = sizeof(buf);
				errprintf("WARN: In CompileShader(), malloc(%d) failed!\n", infoLogLength+1);
			}
		}

		glGetShaderInfoLog(shader, bufLen, NULL, bufPtr);

		const char* shaderTypeStr = "";
		switch(shaderType)
		{
			case GL_VERTEX_SHADER:   shaderTypeStr = "Vertex"; break;
			case GL_FRAGMENT_SHADER: shaderTypeStr = "Fragment"; break;
			// I don't think I need geometry or tesselation shaders here
			// case GL_GEOMETRY_SHADER: shaderTypeStr = "Geometry"; break;
			/* not supported in OpenGL3.2 and we're unlikely to need/use them anyway
			case GL_COMPUTE_SHADER:  shaderTypeStr = "Compute"; break;
			case GL_TESS_CONTROL_SHADER:    shaderTypeStr = "TessControl"; break;
			case GL_TESS_EVALUATION_SHADER: shaderTypeStr = "TessEvaluation"; break;
			*/
		}
		errprintf("ERROR: Compiling %s Shader failed: %s\n", shaderTypeStr, bufPtr);
		errprintf("Source BEGIN\n");
		for(const char* part : shaderSources) {
			errprintf("%s", part);
		}
		errprintf("\nSource END\n");
		glDeleteShader(shader);

		if(bufPtr != buf) {
			free(bufPtr);
		}

		return 0;
	}

	return shader;
}

static GLuint
CreateShaderProgram(const GLuint shaders[2])
{
	GLuint prog = glCreateProgram();
	if(prog == 0) {
		errprintf("ERROR: Couldn't create a new Shader Program!\n");
		return 0;
	}

	glAttachShader(prog, shaders[0]);
	glAttachShader(prog, shaders[1]);

	// TODO: glBindAttribLocation() goes here, if needed

	glLinkProgram(prog);

	GLint status;
	glGetProgramiv(prog, GL_LINK_STATUS, &status);
	if(status != GL_TRUE) {
		char buf[2048];
		char* bufPtr = buf;
		int bufLen = sizeof(buf);
		GLint infoLogLength;
		glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &infoLogLength);
		if(infoLogLength >= bufLen) {
			bufPtr = (char*)malloc(infoLogLength+1);
			bufLen = infoLogLength+1;
			if(bufPtr == NULL) {
				bufPtr = buf;
				bufLen = sizeof(buf);
				errprintf("WARN: In CreateShaderProgram(), malloc(%d) failed!\n", infoLogLength+1);
			}
		}

		glGetProgramInfoLog(prog, bufLen, NULL, bufPtr);

		errprintf("ERROR: Linking shader program failed: %s\n", bufPtr);

		glDeleteProgram(prog);

		if(bufPtr != buf) {
			free(bufPtr);
		}
		glDetachShader(prog, shaders[0]);
		glDetachShader(prog, shaders[1]);

		return 0;
	}

	return prog;
}

static void SetSwizzleFromSimple()
{
	swizzle.clear();
	const char* args[4] = { "0.0", "0.0", "0.0", "1.0" };
	for(int i=0; i<4; ++i) {
		char c = simpleSwizzle[i];
		if(c >= 'A' && c <= 'Z') {
			c += 32; // to lowercase
		}
		switch(c) {
			case '0':
				args[i] = "0.0";
				break;
			case '1':
				args[i] = "1.0";
				break;
			case 'r':
			case 'x':
				args[i] = "c.r";
				break;
			case 'g':
			case 'y':
				args[i] = "c.g";
				break;
			case 'b':
			case 'z':
				args[i] = "c.b";
				break;
			case 'a':
			case 'w':
				args[i] = "c.a";
				break;
			case '\0':
				// leave this and following at default value (0.0 or 1.0)
				// make sure the loop is terminated here, string is over
				i = 4;
				break;
			default:
				errprintf("Invalid character '%c' in swizzle!\n", simpleSwizzle[i]);
		}
	}
	AppendFormatted(swizzle, "c = vec4(%s, %s, %s, %s);\n", args[0], args[1], args[2], args[3]);
}

static bool UpdateShaders()
{
	const char* glslVersion = "#version 150 compatibility\n";

	GLuint shaders[2] = {};
	shaders[0] = CompileShader(GL_VERTEX_SHADER, { glslVersion, vertexShaderSrc });
	if(shaders[0] == 0) {
		return false;
	}

	bool isUnsigned = false;
	const char* normDiv = curTex.GetIntTexInfo(isUnsigned); // divisor to normalize integer texture
	bool isIntTexture = normDiv != nullptr;

	std::string glslAdvVersion; // if used, glslVersion will point to it

	const char* samplerBaseType = "sampler2D";
	int numTexCoords = 2; // default: Texture2D; 2 for .st, 3 for .stp, 4 for stpq (1 for .s once supporting texture1D)
	const char* typePrefix = ""; // default: standard texture (not _INTEGER)
	const char* typePostfix = ""; // default: no array texture
	if(isIntTexture) {
		typePrefix = isUnsigned ? "u" : "i";
	}
	if(curTex.IsCubemap()) {
		samplerBaseType = "samplerCube";
		numTexCoords = 3;
		if(curTex.IsArray()) {
			// for cubemap arrays, this #extension thingy must be added after the #version
			// (unless version >= 400)
			glslAdvVersion = glslVersion;
			glslAdvVersion += "#extension GL_ARB_texture_cube_map_array : enable\n";
			glslVersion = glslAdvVersion.c_str();
		}
	}
	if(curTex.IsArray()) {
		typePostfix = "Array";
		numTexCoords++;
	}

	char samplerUniform[48] = {};
	snprintf(samplerUniform, sizeof(samplerUniform), "uniform %s%s%s tex0;\n", typePrefix, samplerBaseType, typePostfix);

	texSampleAndNormalize.clear();

	if(isIntTexture) {
		AppendFormatted(texSampleAndNormalize,
		                " %svec4 v = texture( tex0, texCoord.%.*s );\n",
		                typePrefix, numTexCoords, "stpq");
		// integer textures (GL_RGB_INTEGER etc) need normalization to display something useful
		AppendFormatted(texSampleAndNormalize, " vec4 c = vec4(v) / %s;\n", normDiv);
	} else {
		// normal textures don't need normalization, so assign to vec4 c directly
		AppendFormatted(texSampleAndNormalize, " vec4 c = texture( tex0, texCoord.%.*s );\n",
		                numTexCoords, "stpq");
	}

	if(useSimpleSwizzle) {
		SetSwizzleFromSimple();
	}

	std::initializer_list<const char*> fragShaderSrc = {
		glslVersion,
		samplerUniform,
		fragShaderStart,
		texSampleAndNormalize.c_str(),
		swizzle.c_str(),
		fragShaderEnd
	};
	shaders[1] = CompileShader(GL_FRAGMENT_SHADER, fragShaderSrc );
	if(shaders[1] == 0) {
		glDeleteShader(shaders[0]);
		return false;
	}

	GLuint prog = CreateShaderProgram(shaders);

	// The shaders aren't needed anymore once they're linked into the program
	glDeleteShader(shaders[0]);
	glDeleteShader(shaders[1]);
	if(prog == 0) {
		return false;
	}

	if(shaderProgram != 0) { // if we already had one and want to replace it
		glDeleteProgram(shaderProgram);
	}

	shaderProgram = prog;

	glUseProgram(shaderProgram);
	// TODO: could set uniforms here

	return true;
}

// mipLevel -1 = auto (let GPU choose from all levels)
// otherwise use the given level (if it exists..)
static void SetMipmapLevel(texview::Texture& texture, GLint mipLevel, bool bindTexture = true)
{
	GLuint tex = texture.glTextureHandle;
	GLint numMips = texture.GetNumMips();
	if(tex == 0 || numMips == 1) {
		return;
	}
	if(bindTexture) {
		glBindTexture(texture.glTarget, tex);
	}

	mipLevel = std::min(mipLevel, numMips - 1);
	// setting both to the same level enforces using that level
	GLint baseLevel = mipLevel;
	GLint maxLevel  = mipLevel;
	if(mipLevel < 0) { // auto mode
		baseLevel = 0;
		maxLevel = numMips - 1;
	}
	glTexParameteri(texture.glTarget, GL_TEXTURE_BASE_LEVEL, baseLevel);
	glTexParameteri(texture.glTarget, GL_TEXTURE_MAX_LEVEL, maxLevel);
}

static void UpdateTextureFilter(bool bindTex = true)
{
	GLuint glTex = curTex.glTextureHandle;
	GLenum target = curTex.glTarget;
	if(glTex == 0) {
		return;
	}
	if(bindTex) {
		glBindTexture(target, glTex);
	}
	GLint filter = linearFilter ? GL_LINEAR : GL_NEAREST;
	if(curTex.GetNumMips() == 1) {
		glTexParameteri(target, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
	} else {
		GLint mipFilter = linearFilter ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
		glTexParameteri(target, GL_TEXTURE_MIN_FILTER, mipFilter);
		glTexParameteri(target, GL_TEXTURE_MAG_FILTER, filter);
	}
}

static void LoadTexture(const char* path)
{
	{
		texview::Texture newTex;
		if(!newTex.Load(path)) {
			errprintf("Couldn't load texture '%s'!\n", path);
			return;
		}

		curTex = std::move(newTex);
	}
	// set windowtitle to filename (not entire path)
	{
		const char* fileName = strrchr(path, '/');
#ifdef _WIN32
		const char* lastBS = strrchr(path, '\\');
		if( lastBS != nullptr && (fileName == nullptr || fileName < lastBS) )
			fileName = lastBS;
#endif
		if(fileName == nullptr)
			fileName = path;
		else
			++fileName; // skip (back)slash

		char winTitle[256];
		snprintf(winTitle, sizeof(winTitle), "Texture Viewer - %s", fileName);

		glfwSetWindowTitle(glfwWindow, winTitle);
	}

	curTex.CreateOpenGLtexture();
	int numMips = curTex.GetNumMips();

	UpdateTextureFilter(false);
	if(numMips > 1) {
		if(mipmapLevel != -1) {
			// if it's set to auto, keep it at auto, otherwise default to 0
			mipmapLevel = 0;
		}
		SetMipmapLevel(curTex, mipmapLevel, false);
	}

	if(curTex.IsCubemap()) {
		float w, h;
		curTex.GetSize(&w, &h);
		ZoomFitToWindow(glfwWindow, w, h, true);
		spacingBetweenMips = 0;
	} else {
		spacingBetweenMips = 2;
	}

	textureArrayIndex = 0;

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);


	if(curTex.defaultSwizzle != nullptr) {
		strncpy(simpleSwizzle, curTex.defaultSwizzle, 4);
		simpleSwizzle[4] = '\0';
	} else {
		if(curTex.textureFlags & texview::TF_HAS_ALPHA) {
			strncpy(simpleSwizzle, "rgba", 5);
		} else {
			strncpy(simpleSwizzle, "rgb1", 5);
		}
	}
	useSimpleSwizzle = true;
	swizzle.clear();

	UpdateShaders();
}

// mipLevel -1 == use configured mipmapLevel
static void DrawQuad(texview::Texture& texture, int mipLevel, int arrayIndex, ImVec2 pos, ImVec2 size, ImVec2 texCoordMax = ImVec2(1, 1))
{
	ImVec2 texCoordMin = ImVec2(0, 0);
	GLuint tex = texture.glTextureHandle;
	if(tex) {

		glBindTexture(texture.glTarget, tex);

		SetMipmapLevel(texture, (mipLevel < 0) ? mipmapLevel : mipLevel, false);

		float idx = arrayIndex;

		glBegin(GL_QUADS);
			glTexCoord3f(texCoordMin.x, texCoordMin.y, idx);
			glVertex2f(pos.x, pos.y);

			glTexCoord3f(texCoordMin.x, texCoordMax.y, idx);
			glVertex2f(pos.x, pos.y + size.y);

			glTexCoord3f(texCoordMax.x, texCoordMax.y, idx);
			glVertex2f(pos.x + size.x, pos.y + size.y);

			glTexCoord3f(texCoordMax.x, texCoordMin.y, idx);
			glVertex2f(pos.x + size.x, pos.y);
		glEnd();
	}
}

struct vec4 {
	union {
		struct { float x, y, z, w; };
		float vals[4];
	};
	vec4() = default;
	vec4(float x_, float y_, float z_ = 0.0f, float w_ = 0.0f)
	: x(x_), y(y_), z(z_), w(w_) {}
};
enum CubeFaceIndex {
	FI_XPOS = 0,
	FI_XNEG = 1,
	FI_YPOS = 2,
	FI_YNEG = 3,
	FI_ZPOS = 4,
	FI_ZNEG = 5
};

// mipLevel -1 == use configured mipmapLevel
static void DrawCubeQuad(texview::Texture& texture, int mipLevel, int faceIndex, int arrayIndex, ImVec2 pos, ImVec2 size, ImVec2 texCoordMax = ImVec2(1, 1))
{
	ImVec2 texCoordMin = ImVec2(0, 0);

	GLuint tex = texture.glTextureHandle;
	if(tex) {

		glBindTexture(texture.glTarget, tex);

		SetMipmapLevel(texture, (mipLevel < 0) ? mipmapLevel : mipLevel, false);

		// helpful: https://stackoverflow.com/questions/38543155/opengl-render-face-of-cube-map-to-a-quad

		// scale from [0, 1] to [-1, 1]
		texCoordMin.x = texCoordMin.x * 2.0f - 1.0f;
		texCoordMin.y = texCoordMin.y * 2.0f - 1.0f;
		texCoordMax.x = texCoordMax.x * 2.0f - 1.0f;
		texCoordMax.y = texCoordMax.y * 2.0f - 1.0f;

		vec4 mapCoords[4] = {
			// initialize with x, y coordinates (or s,t or whatever)
			{ texCoordMin.x, texCoordMin.y },
			{ texCoordMin.x, texCoordMax.y },
			{ texCoordMax.x, texCoordMax.y },
			{ texCoordMax.x, texCoordMin.y }
		};

		for(vec4& mc : mapCoords) {
			vec4 tmp;
			switch(faceIndex) {
				case FI_XPOS:
					tmp = vec4( 1.0f, -mc.y, -mc.x );
					break;
				case FI_XNEG:
					tmp = vec4( -1.0f, -mc.y, mc.x );
					break;
				case FI_YPOS:
					tmp = vec4( mc.x, 1.0f, mc.y );
					break;
				case FI_YNEG:
					tmp = vec4( mc.x, -1.0f, -mc.y );
					break;
				case FI_ZPOS:
					tmp = vec4( mc.x, -mc.y, 1.0f );
					break;
				case FI_ZNEG:
					tmp = vec4( -mc.x, -mc.y, -1.0f );
					break;
			}
			mc = tmp;
			mc.w = arrayIndex;
		}

		if(cubeCrossVariant > 0 && (faceIndex == FI_YPOS || faceIndex == FI_YNEG)) {
			int rotationSteps = (faceIndex == FI_YPOS) ? cubeCrossVariant : (4 - cubeCrossVariant);
			vec4 mapCoordsCopy[4];
			for(int i=0; i<4; ++i) {
				mapCoordsCopy[i] = mapCoords[ (i+rotationSteps) % 4 ];
			}
			memcpy(mapCoords, mapCoordsCopy, sizeof(mapCoords));
		}

		glBegin(GL_QUADS);
			glTexCoord4fv(mapCoords[0].vals);
			glVertex2f(pos.x, pos.y);

			glTexCoord4fv(mapCoords[1].vals);
			glVertex2f(pos.x, pos.y + size.y);

			glTexCoord4fv(mapCoords[2].vals);
			glVertex2f(pos.x + size.x, pos.y + size.y);

			glTexCoord4fv(mapCoords[3].vals);
			glVertex2f(pos.x + size.x, pos.y);
		glEnd();
	}
}

static void DrawTexture()
{
	texview::Texture& tex = curTex;

	bool enableAlphaBlend = (tex.textureFlags & texview::TF_HAS_ALPHA) != 0;
	if(overrideAlpha != -1)
		enableAlphaBlend = overrideAlpha;
	if(enableAlphaBlend)
		glEnable(GL_BLEND);
	else
		glDisable(GL_BLEND);

	int arrayIndex = textureArrayIndex;

	// this whole SRGB thing confuses me.. if the gl texture has an SRGB format
	// (like GL_SRGB_ALPHA), it must have GL_FRAMEBUFFER_SRGB enabled for drawing.
	// if it has a non-SRGB format (even if using the exact same pixeldata
	// e.g. from stb_image!) it must have GL_FRAMEBUFFER_SRGB disabled.
	// no idea what sense that's supposed to make (if all the information is in
	// the texture, why is there no magic to always make it look correct?),
	// but maybe it makes a difference when writing shaders?
	bool enableSRGB = (tex.textureFlags & texview::TF_SRGB) != 0;
	if(overrideSRGB != -1)
		enableSRGB = overrideSRGB;
	if(enableSRGB)
		glEnable( GL_FRAMEBUFFER_SRGB );
	else
		glDisable( GL_FRAMEBUFFER_SRGB );

	glUseProgram(shaderProgram);

	float texW, texH;
	tex.GetSize(&texW, &texH);

	if(tex.IsCubemap()) {
		// render it as a scandinavian-flag style cross (those "Mittelchristen"
		//  can't decide between cross and inverted cross)
		// Y+ is always the upper square, Y- the lower square
		// between them are the remaining ones, by default X-, Z+, X+, Z-
		// extra feature of this texture viewer: cycle the middle ones (e.g. Z+, X+, Z+, X-)
		// and rotate the upper/lower ones accordingly

		const float offset = texW + spacingBetweenMips; // texW = texH
		float posX = offset;
		float posY = 0.0f;
		const ImVec2 size(texW, texH);
		DrawCubeQuad(tex, -1, FI_YPOS, arrayIndex, ImVec2(posX, posY), size);

		posX = 0.0f;
		posY += offset;
		const int middleIndices[4] = { FI_XNEG, FI_ZPOS, FI_XPOS, FI_ZNEG };
		for(int i=cubeCrossVariant, n=cubeCrossVariant+4; i < n; ++i) {
			int faceIndex = middleIndices[i % 4];
			DrawCubeQuad(tex, -1, faceIndex, arrayIndex, ImVec2(posX, posY), size);
			posX += offset;
		}
		posX = offset;
		posY += offset;

		DrawCubeQuad(tex, -1, FI_YNEG, arrayIndex, ImVec2(posX, posY), size);

		glDisable( GL_FRAMEBUFFER_SRGB ); // make sure it's disabled or ImGui will look wrong
		return;
	}

	if(viewMode == SINGLE) {
		DrawQuad(tex, -1, arrayIndex, ImVec2(0, 0), ImVec2(texW, texH));
	} else if(viewMode == TILED) {
		float tilesX = numTiles[0];
		float tilesY = numTiles[1];
		ImVec2 size(texW*tilesX, texH*tilesY);
		DrawQuad(tex, -1, arrayIndex, ImVec2(0, 0), size, ImVec2(tilesX, tilesY));
	} else if(viewAtSameSize) {
		int numMips = tex.GetNumMips();
		if(viewMode == MIPMAPS_COMPACT) {
			// try to have about the same with and height
			// (but round up because more horizontally is preferable due to displays being wide)
			int numHor = ceil(sqrtf(numMips * texH / texW));
			float posX = 0.0f;
			float posY = 0.0f;
			float hOffset = texW + spacingBetweenMips;
			float vOffset = texH + spacingBetweenMips;
			int rowNum = 0;
			for(int i=0; i < numMips; ++i) {
				DrawQuad(tex, i, arrayIndex, ImVec2(posX, posY), ImVec2(texW, texH));
				if(((i+1) % numHor) == 0) {
					posY += vOffset;
					// change horizontal direction every line
					// so the next level of the last mip of one line
					// is right below it instead of the start of the next line
					hOffset = -hOffset;
					++rowNum;
				} else {
					posX += hOffset;
				}
			}
		} else if(viewMode == MIPMAPS_ROW || viewMode == MIPMAPS_COLUMN) {
			float hOffset = (viewMode == MIPMAPS_ROW) ? texW + spacingBetweenMips : 0.0f;
			float vOffset = (viewMode == MIPMAPS_ROW) ? 0.0f : texH + spacingBetweenMips;
			float posX = 0.0f;
			float posY = 0.0f;
			for(int i=0; i < numMips; ++i) {
				DrawQuad(tex, i, arrayIndex, ImVec2(posX, posY), ImVec2(texW, texH));
				posX += hOffset;
				posY += vOffset;
			}
		} else {
			assert(0 && "unknown viewmode?!");
		}

	} else { // don't view at same size
		int numMips = tex.GetNumMips();
		if(viewMode == MIPMAPS_COMPACT) {

			bool toRight = (texW/texH <= 1.2f); // otherwise down

			// below I adjust the spacing between mipmaps so it's not absurdly big
			// for the smallest mips, by limiting it to half the current mipmap width or height
			// but I also want to make sure that it's at least 2 pixels
			// UNLESS spacingBetweenMips is smaller than that.
			// using minSpace instead of 2 helps with that.
			float minSpace = std::min(2, spacingBetweenMips);

			float posX = 0.0f;
			float posY = 0.0f;
			for(int i=0; i < numMips; ++i) {
				float w, h;
				tex.GetMipSize(i, &w, &h);
				DrawQuad(tex, i, arrayIndex, ImVec2(posX, posY), ImVec2(w, h));

				if( (toRight && (i & 1) == 0)
				   || (!toRight && (i & 1) == 1) ) {
					float space = std::max(minSpace, std::min(float(spacingBetweenMips), w * 0.5f));
					posX += space + w;
				} else {
					float space = std::max(minSpace, std::min(float(spacingBetweenMips), h * 0.5f));
					posY += space + h;
				}
			}

		} else if(viewMode == MIPMAPS_ROW || viewMode == MIPMAPS_COLUMN) {
			bool inRow = (viewMode == MIPMAPS_ROW);
			float posX = 0.0f;
			float posY = 0.0f;
			for(int i=0; i < numMips; ++i) {
				float w, h;
				tex.GetMipSize(i, &w, &h);
				DrawQuad(tex, i, arrayIndex, ImVec2(posX, posY), ImVec2(w, h));
				if(inRow) {
					posX += spacingBetweenMips + w;
				} else {
					posY += spacingBetweenMips + h;
				}
			}
		} else {
			assert(0 && "unknown viewmode?!");
		}
	}

	glDisable( GL_FRAMEBUFFER_SRGB ); // make sure it's disabled or ImGui will look wrong
}

static void GenericFrame(GLFWwindow* window)
{
	int display_w, display_h;
	glfwGetFramebufferSize(window, &display_w, &display_h);
	glViewport(0, 0, display_w, display_h);
	glClearColor(clear_color.x * clear_color.w, clear_color.y * clear_color.w,
	             clear_color.z * clear_color.w, clear_color.w);
	glClear(GL_COLOR_BUFFER_BIT);

	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

	float sx, sy;
	glfwGetWindowContentScale(window, &sx, &sy);

	float xOffs = imguiMenuCollapsed ? 0.0f : imGuiMenuWidth * sx;
	float winW = display_w - xOffs;

	// good thing we're using a compat profile :-p
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glViewport(xOffs, 0, winW, display_h);
	glOrtho(0, winW, display_h, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	glScaled(zoomLevel, zoomLevel, 1);
	glTranslated((transX * sx) / zoomLevel, (transY * sy) / zoomLevel, 0.0);

	DrawTexture();
}


static void OpenFilePicker() {
#ifdef TV_USE_NFD
		nfdopendialogu8args_t args = {0};
		//args.filterList = filters;
		//args.filterCount = 2;
		std::string dp;
		if(!curTex.name.empty()) {
			dp = curTex.name;
			size_t lastSlash = dp.find_last_of('/');
	#ifdef _WIN32
			size_t lastBS = dp.find_last_of('\\');
			if( (lastBS != std::string::npos && lastBS > lastSlash)
			   || lastSlash == std::string::npos )
			{
				lastSlash = lastBS;
			}
	#endif
			if(lastSlash != std::string::npos) {
				dp.resize(lastSlash);
				args.defaultPath = dp.c_str();
			}
		}
		nfdu8char_t* outPath = nullptr;
		nfdresult_t result = NFD_OpenDialogU8_With(&outPath, &args);
		if(result == NFD_OKAY) {
			LoadTexture(outPath);
		}
		if(outPath != nullptr) {
			NFD_FreePathU8(outPath);
		}
#else
		// TODO: imgui-only alternative, maybe https://github.com/aiekick/ImGuiFileDialog
		errprintf("Built without NativeFileDialog support, have no alternative (yet)!\n");
#endif
}

static void DrawAboutWindow(GLFWwindow* window)
{
	ImGuiIO& io = ImGui::GetIO();

	ImGui::SetNextWindowPos( ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
	                         ImGuiCond_Appearing, ImVec2(0.5f, 0.5f) );
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize
	                        | ImGuiWindowFlags_NoCollapse;
	if(ImGui::Begin("About", &showAboutWindow, flags)) {
		ImGui::TextDisabled("A texture viewer.");
		ImGui::TextDisabled("              v"  texview_version);

		ImGui::Spacing();
		ImGui::Text("Zoom with the mouse wheel,\nmove texture by dragging mouse.");
		ImGui::Text("Press R to reset view.");
		ImGui::Text("You can Ctrl-Click into sliders and\n"
		            "similar to enter the value as text.");
		ImGui::Spacing();

		ImGui::BeginDisabled();
		ImGui::Text("(C) 2025 Daniel Gibson");
		ImGui::Spacing();
		ImGui::Text("Released under MIT license.");
		ImGui::Text("Uses several libraries including GLFW,\n"
		            "Dear ImGui, Native File Dialog Extended,\nstb_image.h and libktx.");
		ImGui::Text("See Licenses.txt for details.");
		ImGui::EndDisabled();

		ImGui::Spacing();
		ImGui::TextLinkOpenURL("https://github.com/DanielGibson/texview");
		ImGui::TextLinkOpenURL("https://blog.gibson.sh");
		ImGui::Spacing();
		ImGui::Spacing();

		float dialogButtonWidth = ImGui::CalcTextSize( "Ok or Cancel ???" ).x; // this width looks ok
		float buttonOffset = (ImGui::GetWindowWidth() - dialogButtonWidth) * 0.5f;
		ImGui::SetCursorPosX( buttonOffset );
		if( ImGui::Button("Close", ImVec2(dialogButtonWidth, 0))
		   || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ) {
			showAboutWindow = false;
		}
	}
	ImGui::End();
}

static void DrawGLSLeditWindow(GLFWwindow* window)
{
	ImGuiIO& io = ImGui::GetIO();

	ImGui::SetNextWindowPos( ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
	                         ImGuiCond_Once, ImVec2(0.5f, 0.5f) );

	ImGuiWindowFlags flags = 0;
	if(ImGui::Begin("Advanced Swizzling", &showGLSLeditWindow, flags)) {
		ImGui::TextDisabled("%s", texSampleAndNormalize.c_str());

		static char buf[4096] = {};
		if(ImGui::IsWindowAppearing()) {
			size_t len = std::min(swizzle.size(), sizeof(buf)-1);
			memcpy(buf, swizzle.c_str(), len);
			buf[len] = '\0';
		}

		ImGuiInputTextFlags flags = ImGuiInputTextFlags_AllowTabInput;
		ImGui::SetNextItemWidth(-8.0f);
		if(ImGui::InputTextMultiline("##glslcode", buf, sizeof(buf), ImVec2(0, 0), flags)) {
			swizzle = buf;
		}

		ImGui::TextDisabled(" OutColor = c;");
		ImGui::Spacing();

		float buttonWidth = ImGui::CalcTextSize("Close or what").x;
		if(ImGui::Button("Apply", ImVec2(buttonWidth, 0.0f))
		   || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Enter)) {
			UpdateShaders();
		}
		ImGui::SetItemTooltip("Alternatively you can press Ctrl+Enter to apply");

		ImGui::SameLine();
		float buttonOffset = (ImGui::GetWindowWidth() - buttonWidth - 8.0f - ImGui::GetStyle().WindowPadding.x);
		ImGui::SetCursorPosX(buttonOffset);
		if(ImGui::Button("Close", ImVec2(buttonWidth, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			showGLSLeditWindow = false;
		}
	}
	ImGui::End();
}

static void DrawSidebar(GLFWwindow* window)
{
	ImGuiIO& io = ImGui::GetIO();

	ImGui::SetNextWindowPos( ImVec2(0, 0), ImGuiCond_Appearing );
	if(!imguiMenuCollapsed) {
		ImGui::SetNextWindowSize(ImVec2(0, io.DisplaySize.y), ImGuiCond_Always);
	}

	ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
	if(ImGui::Begin("##options", NULL, flags)) {
		if(ImGui::Button("Open File")) {
			OpenFilePicker();
		}
		float fontWrapWidth = ImGui::CalcTextSize("0123456789abcdef0123456789ABCDEF").x;
		ImGui::PushTextWrapPos(fontWrapWidth);
		//ImGui::TextWrapped("File: %s", curTex.name.c_str());
		ImGui::Text("File: ");
		ImGui::BeginDisabled(true);
		ImGui::TextWrapped("%s", curTex.name.c_str());
		ImGui::EndDisabled();
		ImGui::Text("Format: %s", curTex.formatName.c_str());
		float tw, th;
		curTex.GetSize(&tw, &th);
		ImGui::Text("Texture Size: %d x %d", (int)tw, (int)th);
		ImGui::Text("MipMap Levels: %d", curTex.GetNumMips());
		bool isCubemap = curTex.IsCubemap();
		int numCubeFaces = curTex.GetNumCubemapFaces();
		if(curTex.IsArray()) {
			ImGui::Text("%sArray Elements: %d", isCubemap ? "Cubemap " : "", curTex.GetNumElements());
		} else if(isCubemap) {
			if(numCubeFaces == 6) {
				ImGui::Text("Cubemap Texture");
			} else {
				ImGui::Text("Cubemap Texture with %d faces", curTex.GetNumCubemapFaces());
			}
		}
		const char* alphaStr = "no";
		bool texHasAlpha = (curTex.textureFlags & texview::TF_HAS_ALPHA) != 0;
		if(texHasAlpha) {
			alphaStr = (curTex.textureFlags & texview::TF_PREMUL_ALPHA) ? "Premultiplied" : "Straight";
		}
		bool texIsSRGB = (curTex.textureFlags & texview::TF_SRGB) != 0;
		ImGui::Text("Alpha: %s - sRGB: %s", alphaStr, texIsSRGB ? "yes" : "no");

		ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
		ImGui::PushItemWidth(fontWrapWidth - ImGui::CalcTextSize("View Mode  ").x);
		float zl = zoomLevel;
		if(ImGui::SliderFloat("Zoom", &zl, 0.0125, 50.0f, "%.3f", ImGuiSliderFlags_Logarithmic)) {
			zoomLevel = zl;
		}
		if(ImGui::Button("Fit to Window")) {
			ZoomFitToWindow(window, tw, th, isCubemap);
		}
		ImGui::SameLine();
		if(ImGui::Button("Reset Zoom")) {
			zoomLevel = 1.0;
		}
		if(ImGui::Button("Reset Position")) {
			transX = transY = 10.0;
		}

		ImGui::Spacing();

		int vMode = viewMode;
		if(curTex.IsCubemap()) {
			ImGui::SliderInt("View Mode##cube", &cubeCrossVariant, 0, 3, "%d", ImGuiSliderFlags_AlwaysClamp);

			ImGui::SliderInt("Spacing", &spacingBetweenMips, 0, 32, "%d pix");
		} else { // not cubemap
			if(ImGui::Combo("View Mode", &vMode, "Single\0MipMaps Compact\0MipMaps in Row\0MipMaps in Column\0Tiled\0")) {
				// zoom out when not single, so everything (or at least more) is on the screen
				// TODO: do some calculation for good amount of zooming out here?
				if(viewMode == SINGLE && vMode != SINGLE) {
					zoomLevel *= 0.5;
				}
				viewMode = (ViewMode)vMode;
			}
			if(vMode != SINGLE && vMode != TILED) {
				ImGui::Checkbox("Show MipMaps at same size", &viewAtSameSize);
				ImGui::SliderInt("Spacing", &spacingBetweenMips, 0, 32, "%d pix");
				ImGui::SetItemTooltip("Spacing between mips");
			} else if(vMode == TILED) {
				ImGui::InputInt2("Tiles", numTiles);
			}
		}
		if(isCubemap || vMode == SINGLE || vMode == TILED) {
			int mipLevel = mipmapLevel;
			int maxLevel = std::max(0, curTex.GetNumMips() - 1);
			if(maxLevel == 0) {
				ImGui::BeginDisabled(true);
				ImGui::SliderInt("LOD", &mipLevel, 0, 1, "0 (No Mip Maps)");
				ImGui::EndDisabled();
			} else {
				const char* miplevelString = "Auto"; // (normal mip mapping)";
				char miplevelStrBuf[64] = {};
				if(mipLevel >= 0) {
					mipLevel = std::min(mipLevel, maxLevel);
					miplevelString = miplevelStrBuf;
					float w, h;
					curTex.GetMipSize(mipLevel, &w, &h);
					snprintf(miplevelStrBuf, sizeof(miplevelStrBuf), "%d (%dx%d)",
					         mipLevel, (int)w, (int)h);
				}
				if(ImGui::SliderInt("Mip Level", &mipLevel, -1, maxLevel,
				                    miplevelString, ImGuiSliderFlags_AlwaysClamp)) {
					mipmapLevel = mipLevel;
					SetMipmapLevel(curTex, mipLevel);
				}
			}
		}
		if(curTex.IsArray()) {
			int numElems = curTex.GetNumElements();
			ImGui::SliderInt("Array Index", &textureArrayIndex, 0, numElems-1,
			                 "%d", ImGuiSliderFlags_AlwaysClamp);
		}

		ImGui::Spacing();
		int texFilter = linearFilter;
		if(ImGui::Combo("Filter", &texFilter, "Nearest\0Linear\0")) {
			if(texFilter != (int)linearFilter) {
				linearFilter = texFilter != 0;
				UpdateTextureFilter();
			}
		}

		int srgb = overrideSRGB + 1 ; // -1 => 0 etc
		const char* srgbStr = texIsSRGB ? "Tex Default (sRGB)\0Force Linear\0Force sRGB\0"
		                                : "Tex Default (Linear)\0Force Linear\0Force sRGB\0";
		if(ImGui::Combo("sRGB", &srgb, srgbStr)) {
			overrideSRGB = srgb - 1;
		}
		ImGui::SetItemTooltip("Override if texture is assumed to have sRGB or Linear data");

		int alpha = overrideAlpha + 1; // -1 => 0 etc
		const char* alphaSelStr = texHasAlpha ? "Tex Default (on)\0Force Disable\0Force Enable\0"
		                                      : "Tex Default (off)\0Force Disable\0Force Enable\0";
		if(ImGui::Combo("Alpha", &alpha, alphaSelStr)) {
			overrideAlpha = alpha - 1;
		}
		ImGui::SetItemTooltip("Enable/Disable Alpha Blending");

		if(useSimpleSwizzle) {
			ImGuiInputTextFlags swizzleInputFlags = ImGuiInputTextFlags_CallbackCharFilter;
			ImGuiInputTextCallback swizzleInputCB = [](ImGuiInputTextCallbackData* data) -> int {
				// according to the documentation, returning 1 here skips the char
				// probably returning 0 means "use it, it's valid"
				const char* validChars = "rgbaRGBAxyzwXYZW01";
				int c = data->EventChar;
				if(c < '0' || c > 'z') {
					return 1; // definitely invalid
				}
				return strchr(validChars, c) == nullptr;
			};
			if( ImGui::InputText("Swizzle", simpleSwizzle, sizeof(simpleSwizzle), swizzleInputFlags, swizzleInputCB) ) {
				UpdateShaders();
			}
			ImGui::SetItemTooltip("Swizzles the color channels. Four characters,\n"
			                      "for the Red, Green, Blue and Alpha channels.\n"
			                      "Valid characters: r, g, b, a, x, y, z, w, 0, 1\n"
			                      "0 and 1 set the color channel to that value,\n"
			                      "the others set the color channel to the value of the given channel.\n"
			                      "Default: \"rgba\" if texture has alpha channel, else \"rgb1\"\n");
		} else {
			ImGui::Text("Using advanced Swizzling:");
			ImGui::BeginDisabled();
			ImGui::Text("%.*s ...", 24, swizzle.c_str());
			ImGui::EndDisabled();
			if(ImGui::Button("Edit advanced Swizzling")) {
				showGLSLeditWindow = true;
			}
		}
		bool useAdvancedSwizzle = !useSimpleSwizzle;
		if(ImGui::Checkbox("Use advanced Swizzling", &useAdvancedSwizzle)) {
			useSimpleSwizzle = !useAdvancedSwizzle;
			if(useAdvancedSwizzle && simpleSwizzle[0] == '\0') {
				// in case no simple swizzle was set, set the default one now
				// so the advanced swizzle text isn't empty
				memcpy(simpleSwizzle, "rgba", 5);
				SetSwizzleFromSimple();
			}
		}

		ImGui::Spacing(); ImGui::Spacing();

		ImGui::ColorEdit3("BG Color", &clear_color.x);
		ImGui::Spacing(); ImGui::Spacing();
		ImGui::Separator();
		ImGui::Spacing(); ImGui::Spacing();
		float aboutButtonWidth = ImGui::CalcTextSize( "About blah" ).x; // this width looks ok
		ImGui::SetCursorPosX( (ImGui::GetWindowWidth() - aboutButtonWidth) * 0.5f );
		if(ImGui::Button("About")) {
			showAboutWindow = true;
		}
		ImGui::Dummy(ImVec2(8, 32));
		ImGui::Checkbox("Show ImGui Demo Window", &showImGuiDemoWindow);
		imGuiMenuWidth = ImGui::GetWindowWidth();
	}
	imguiMenuCollapsed = ImGui::IsWindowCollapsed();
	ImGui::End();
}

static void ImGuiFrame(GLFWwindow* window)
{
	// Start the Dear ImGui frame
	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	if(showImGuiDemoWindow)
		ImGui::ShowDemoWindow(&showImGuiDemoWindow);

	if(showAboutWindow)
		DrawAboutWindow(window);

	if(showGLSLeditWindow)
		DrawGLSLeditWindow(window);

	DrawSidebar(window);

	// NOTE: ImGui::GetMouseDragDelta() is not very useful here, because
	//       I only want drags that start outside of ImGui windows
	bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
	if( dragging || (mouseDown && !ImGui::GetIO().WantCaptureMouse) ) {
		ImVec2 mousePos = ImGui::GetMousePos();
		if(mouseDown) {
			if(dragging) {
				float dx = mousePos.x - lastDragPos.x;
				float dy = mousePos.y - lastDragPos.y;
				transX += dx;
				transY += dy;
				lastDragPos = mousePos;
			} else {
				lastDragPos = mousePos;
				dragging = true;
			}
		} else { // left mousebutton not down (anymore) => stop dragging
			dragging = false;
		}
	}

	ImGui::Render();
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static double CalcZoomLevel(double zl, bool increase)
{
	if(increase) {
		if(zl >= 2.0)
			zl += 0.5;
		else if(zl >= 1.0)
			zl += 0.25;
		else if (zl >= 0.125)
			zl += 0.125;
		else
			zl *= sqrt(2.0);
	} else {
		if(zl <= 0.125)
			zl *= 1.0/sqrt(2.0);
		else if(zl <= 1.0)
			zl -= 0.125;
		else if(zl <= 2.0)
			zl -= 0.25;
		else
			zl -= 0.5;
	}

	if(zl >= 1.0) {
		double nearestHalf = round(zl*2.0)*0.5;
		if(fabs(nearestHalf - zl) <= std::min(0.25, 0.1 * zl)) {
			return nearestHalf;
		}
	} else if(zl > 0.25) {
		double nearestEighth = round(zl*8.0)*0.125;
		if(fabs(nearestEighth - zl) <= 0.05) {
			return nearestEighth;
		}
	}
	return zl;
}

static void myGLFWscrollfun(GLFWwindow* window, double xoffset, double yoffset)
{
	// ImGui_ImplSDL2_ProcessEvent() doc says:
	//   You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
	//   - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
	//   - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
	//   Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
	if(yoffset == 0 || ImGui::GetIO().WantCaptureMouse) {
		return;
	}

	zoomLevel = CalcZoomLevel(zoomLevel, yoffset > 0.0);
}

static void myGLFWkeyfun(GLFWwindow* window, int key, int scancode, int action, int mods)
{
	// while io.WantCaptureKeyboard doesn't work well (it returns true if an
	// ImGui window has focus, even if no text input is active), this seems to
	// do exactly what I want (i.e. let me ignore keys only if one is currently
	// typing text into some ImGui widget)
	if(ImGui::GetIO().WantTextInput) {
		return;
	}

	if(key == GLFW_KEY_R) {
		zoomLevel = 1.0;
		transX = 10.0;
		transY = 10.0;
	}
}

void myGLFWwindowcontentscalefun(GLFWwindow* window, float xscale, float yscale)
{
	ImGui::GetIO().FontGlobalScale = std::max(xscale, yscale);
}

/*
 * Callback function for debug output.
 */
static void APIENTRY
GLDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length,
                const GLchar *message, const void *userParam)
{
	const char* sourceStr = "Source: Unknown";
	const char* typeStr = "Type: Unknown";
	const char* severityStr = "Severity: Unknown";

	switch (severity)
	{
#define SVRCASE(X, STR)  case GL_DEBUG_SEVERITY_ ## X ## _ARB : severityStr = STR; break;
		// GL_DEBUG_SEVERITY_NOTIFICATION_ARB is not in the glad header
		// (not specified in GL_ARB_debug_output I think?) but drivers send such
		// messages anyway. I don't want them so just return when getting that value
#if 01 // allow to quickly enable notification messages as well
		case 0x826B:  return;
#else
		case 0x826B: severityStr = "Severity: Notification"; break;
#endif
		SVRCASE(HIGH, "Severity: High")
		SVRCASE(MEDIUM, "Severity: Medium")
		SVRCASE(LOW, "Severity: Low")
#undef SVRCASE
	}

	switch (source)
	{
#define SRCCASE(X)  case GL_DEBUG_SOURCE_ ## X ## _ARB: sourceStr = "Source: " #X; break;
		SRCCASE(API);
		SRCCASE(WINDOW_SYSTEM);
		SRCCASE(SHADER_COMPILER);
		SRCCASE(THIRD_PARTY);
		SRCCASE(APPLICATION);
		SRCCASE(OTHER);
#undef SRCCASE
	}

	switch(type)
	{
#define TYPECASE(X)  case GL_DEBUG_TYPE_ ## X ## _ARB: typeStr = "Type: " #X; break;
		TYPECASE(ERROR);
		TYPECASE(DEPRECATED_BEHAVIOR);
		TYPECASE(UNDEFINED_BEHAVIOR);
		TYPECASE(PORTABILITY);
		TYPECASE(PERFORMANCE);
		TYPECASE(OTHER);
#undef TYPECASE
	}

	errprintf("GLDBG %s %s %s: %s\n", sourceStr, typeStr, severityStr, message);
}

#ifdef _WIN32
int my_main(int argc, char** argv) // called from WinMain() in sys_win.cpp
#else
int main(int argc, char** argv)
#endif
{
	int ret = 0;
	glfwSetErrorCallback(glfw_error_callback);
	if (!glfwInit()) {
		errprintf("glfwInit() failed! Exiting..\n");
		return 1;
	}

#ifdef TV_USE_NFD
	if (NFD_Init() != NFD_OKAY) {
		errprintf("Couldn't initialize Native File Dialog library!\n");
		glfwTerminate();
		return 1; // TODO: instead start and just don't provide a file picker?
	}
#endif

	const char* glDebugEnv = getenv("TEXVIEW_GLDEBUG");
	bool wantDebugContext = (glDebugEnv != nullptr && atoi(glDebugEnv) != 0);

	// Create window with graphics context
	const char* glsl_version = "#version 330"; // for ImGui
	glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
	glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
	if(wantDebugContext) {
		glfwWindowHint(GLFW_CONTEXT_DEBUG, GLFW_TRUE);
	}
#if __APPLE__
	glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

	glfwWindowHint(GLFW_SRGB_CAPABLE, 1); // FIXME: this doesn't seem to make a difference visually or in behavior?!
	//glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
	glfwWindow = glfwCreateWindow(1280, 720, "Texture Viewer", nullptr, nullptr);
	if (glfwWindow == nullptr) {
		errprintf("Couldn't create glfw glfwWindow! Exiting..\n");
		glfwTerminate();
		return 1;
	}

	GLFWimage icons[2] = {
		{ texview_icon32.width, texview_icon32.height, (unsigned char*)texview_icon32.pixel_data },
		{ texview_icon.width, texview_icon.height, (unsigned char*)texview_icon.pixel_data }
	};
	glfwSetWindowIcon(glfwWindow, 2, icons);

	glfwMakeContextCurrent(glfwWindow);
	gladLoadGL(glfwGetProcAddress);

	if(wantDebugContext) {
		int haveDebugContext = glfwGetWindowAttrib(glfwWindow, GLFW_CONTEXT_DEBUG);
		if(!GLAD_GL_ARB_debug_output) {
			errprintf( "You set the TEXVIEW_GLDEBUG environment variable, but GL_ARB_debug_output is not available!\n" );
		} else if(!haveDebugContext) {
			errprintf( "You set the TEXVIEW_GLDEBUG environment variable, but GLFW didn't give us a debug context (for whatever reason)!\n" );
		} else {
			errprintf( "You set the TEXVIEW_GLDEBUG environment variable, enabling OpenGL debug logging\n" );
			glDebugMessageCallbackARB(GLDebugCallback, NULL);
			glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS_ARB);
		}
	}

	glfwSwapInterval(1); // Enable vsync
	ktxLoadOpenGL(glfwGetProcAddress);

	glfwSetScrollCallback(glfwWindow, myGLFWscrollfun);
	glfwSetKeyCallback(glfwWindow, myGLFWkeyfun);

	if(argc > 1) {
		LoadTexture(argv[1]);
	}

	// Setup Dear ImGui context
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();
	// make it look a bit nicer with rounded edges
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 2.0f;
	style.FrameRounding = 3.0f;
	style.FramePadding = ImVec2( 6.0f, 3.0f );
	//style.ChildRounding = 6.0f;
	style.ScrollbarRounding = 8.0f;
	style.GrabRounding = 3.0f;
	style.PopupRounding = 2.0f;

	// Setup Platform/Renderer backends
	ImGui_ImplGlfw_InitForOpenGL(glfwWindow, true);
	ImGui_ImplOpenGL3_Init(glsl_version);

	{
		float xscale = 1.0f;
		float yscale = 1.0f;
		glfwGetWindowContentScale(glfwWindow, &xscale, &yscale);
		myGLFWwindowcontentscalefun(glfwWindow, xscale, yscale);
		glfwSetWindowContentScaleCallback(glfwWindow, myGLFWwindowcontentscalefun);
	}

	while (!glfwWindowShouldClose(glfwWindow)) {
		// Poll and handle events (inputs, window resize, etc.)
		// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
		// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
		// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
		// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
		glfwPollEvents();
		if (glfwGetWindowAttrib(glfwWindow, GLFW_ICONIFIED) != 0)
		{
			ImGui_ImplGlfw_Sleep(32);
			continue;
		}

		GenericFrame(glfwWindow);

		ImGuiFrame(glfwWindow);

		glfwSwapBuffers(glfwWindow);
	}

	if(shaderProgram != 0) { // if we already had one and want to replace it
		glDeleteProgram(shaderProgram);
	}

	curTex.Clear(); // also frees opengl texture which must happen before shutdown

	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();

	glfwDestroyWindow(glfwWindow);
#ifdef TV_USE_NFD
	NFD_Quit();
#endif
	glfwTerminate();

	return ret;
}
