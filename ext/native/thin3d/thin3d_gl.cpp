#include <stdio.h>
#include <vector>
#include <string>
#include <algorithm>
#include <map>
#include <cassert>

#include "base/logging.h"
#include "math/dataconv.h"
#include "math/lin/matrix4x4.h"
#include "thin3d/thin3d.h"
#include "gfx/gl_common.h"
#include "gfx/gl_debug_log.h"
#include "gfx/GLStateCache.h"
#include "gfx_es2/gpu_features.h"
#include "gfx/gl_lost_manager.h"

#include "thin3d/GLRenderManager.h"

#ifdef IOS
extern void bindDefaultFBO();
#endif

// #define DEBUG_READ_PIXELS 1

// Workaround for Retroarch. Simply declare
//   extern GLuint g_defaultFBO;
// and set is as appropriate. Can adjust the variables in ext/native/base/display.h as
// appropriate.
GLuint g_defaultFBO = 0;

namespace Draw {

static const unsigned short compToGL[] = {
	GL_NEVER,
	GL_LESS,
	GL_EQUAL,
	GL_LEQUAL,
	GL_GREATER,
	GL_NOTEQUAL,
	GL_GEQUAL,
	GL_ALWAYS
};

static const unsigned short blendEqToGL[] = {
	GL_FUNC_ADD,
	GL_FUNC_SUBTRACT,
	GL_FUNC_REVERSE_SUBTRACT,
	GL_MIN,
	GL_MAX,
};

static const unsigned short blendFactorToGL[] = {
	GL_ZERO,
	GL_ONE,
	GL_SRC_COLOR,
	GL_ONE_MINUS_SRC_COLOR,
	GL_DST_COLOR,
	GL_ONE_MINUS_DST_COLOR,
	GL_SRC_ALPHA,
	GL_ONE_MINUS_SRC_ALPHA,
	GL_DST_ALPHA,
	GL_ONE_MINUS_DST_ALPHA,
	GL_CONSTANT_COLOR,
	GL_ONE_MINUS_CONSTANT_COLOR,
	GL_CONSTANT_ALPHA,
	GL_ONE_MINUS_CONSTANT_ALPHA,
#if !defined(USING_GLES2)   // TODO: Remove when we have better headers
	GL_SRC1_COLOR,
	GL_ONE_MINUS_SRC1_COLOR,
	GL_SRC1_ALPHA,
	GL_ONE_MINUS_SRC1_ALPHA,
#elif !defined(IOS)
	GL_SRC1_COLOR_EXT,
	GL_ONE_MINUS_SRC1_COLOR_EXT,
	GL_SRC1_ALPHA_EXT,
	GL_ONE_MINUS_SRC1_ALPHA_EXT,
#else
	GL_INVALID_ENUM,
	GL_INVALID_ENUM,
	GL_INVALID_ENUM,
	GL_INVALID_ENUM,
#endif
};

static const unsigned short texWrapToGL[] = {
	GL_REPEAT,
	GL_MIRRORED_REPEAT,
	GL_CLAMP_TO_EDGE,
#if !defined(USING_GLES2)
	GL_CLAMP_TO_BORDER,
#else
	GL_CLAMP_TO_EDGE,
#endif
};

static const unsigned short texFilterToGL[] = {
	GL_NEAREST,
	GL_LINEAR,
};

static const unsigned short texMipFilterToGL[2][2] = {
	// Min nearest:
	{ GL_NEAREST_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR },
	// Min linear:
	{ GL_LINEAR_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_LINEAR },
};

#ifndef USING_GLES2
static const unsigned short logicOpToGL[] = {
	GL_CLEAR,
	GL_SET,
	GL_COPY,
	GL_COPY_INVERTED,
	GL_NOOP,
	GL_INVERT,
	GL_AND,
	GL_NAND,
	GL_OR,
	GL_NOR,
	GL_XOR,
	GL_EQUIV,
	GL_AND_REVERSE,
	GL_AND_INVERTED,
	GL_OR_REVERSE,
	GL_OR_INVERTED,
};
#endif

static const GLuint stencilOpToGL[8] = {
	GL_KEEP,
	GL_ZERO,
	GL_REPLACE,
	GL_INCR,
	GL_DECR,
	GL_INVERT,
	GL_INCR_WRAP,
	GL_DECR_WRAP,
};

static const unsigned short primToGL[] = {
	GL_POINTS,
	GL_LINES,
	GL_LINE_STRIP,
	GL_TRIANGLES,
	GL_TRIANGLE_STRIP,
	GL_TRIANGLE_FAN,
#if !defined(USING_GLES2)   // TODO: Remove when we have better headers
	GL_PATCHES,
	GL_LINES_ADJACENCY,
	GL_LINE_STRIP_ADJACENCY,
	GL_TRIANGLES_ADJACENCY,
	GL_TRIANGLE_STRIP_ADJACENCY,
#elif !defined(IOS)
	GL_POINTS,
	GL_POINTS,
	GL_POINTS,
	GL_POINTS,
	GL_POINTS,
#else
	GL_POINTS,
	GL_POINTS,
	GL_POINTS,
	GL_POINTS,
	GL_POINTS,
#endif
};

class OpenGLBuffer;

class OpenGLBlendState : public BlendState {
public:
	bool enabled;
	GLuint eqCol, eqAlpha;
	GLuint srcCol, srcAlpha, dstCol, dstAlpha;
	int colorMask;
	// uint32_t fixedColor;

	void Apply(GLRenderManager *render) {
		render->SetBlendAndMask(colorMask, enabled, srcCol, dstCol, srcAlpha, dstAlpha, eqCol, eqAlpha);
	}
};

class OpenGLSamplerState : public SamplerState {
public:
	GLint wrapU;
	GLint wrapV;
	GLint wrapW;
	GLint magFilt;
	GLint minFilt;
	GLint mipMinFilt;
};

class OpenGLDepthStencilState : public DepthStencilState {
public:
	bool depthTestEnabled;
	bool depthWriteEnabled;
	GLuint depthComp;
	// TODO: Two-sided
	GLboolean stencilEnabled;
	GLuint stencilFail;
	GLuint stencilZFail;
	GLuint stencilPass;
	GLuint stencilCompareOp;
	uint8_t stencilReference;
	uint8_t stencilCompareMask;
	uint8_t stencilWriteMask;

	void Apply(GLRenderManager *render) {
		render->SetDepth(depthTestEnabled, depthWriteEnabled, depthComp);
		render->SetStencil(stencilEnabled, stencilCompareOp, stencilFail, stencilZFail, stencilPass, stencilWriteMask, stencilCompareMask, stencilReference);
	}
};

class OpenGLRasterState : public RasterState {
public:
	void Apply(GLRenderManager *render) {
		render->SetRaster(cullEnable, frontFace, cullMode);
	}

	GLboolean cullEnable;
	GLenum cullMode;
	GLenum frontFace;
};

GLuint ShaderStageToOpenGL(ShaderStage stage) {
	switch (stage) {
	case ShaderStage::VERTEX: return GL_VERTEX_SHADER;
#ifndef USING_GLES2
	case ShaderStage::COMPUTE: return GL_COMPUTE_SHADER;
	case ShaderStage::EVALUATION: return GL_TESS_EVALUATION_SHADER;
	case ShaderStage::CONTROL: return GL_TESS_CONTROL_SHADER;
	case ShaderStage::GEOMETRY: return GL_GEOMETRY_SHADER;
#endif
	case ShaderStage::FRAGMENT:
	default:
		return GL_FRAGMENT_SHADER;
	}
}

class OpenGLShaderModule : public ShaderModule, public GfxResourceHolder {
public:
	OpenGLShaderModule(GLRenderManager *render, ShaderStage stage) : render_(render), stage_(stage), shader_(nullptr) {
		ILOG("Shader module created (%p)", this);
		register_gl_resource_holder(this, "drawcontext_shader_module", 0);
		glstage_ = ShaderStageToOpenGL(stage);
	}

	~OpenGLShaderModule() {
		ILOG("Shader module destroyed (%p)", this);
		render_->DeleteShader(shader_);
		unregister_gl_resource_holder(this);
	}

	bool Compile(GLRenderManager *render, ShaderLanguage language, const uint8_t *data, size_t dataSize);
	GLRShader *GetShader() const {
		return shader_;
	}
	const std::string &GetSource() const { return source_; }

	ShaderLanguage GetLanguage() {
		return language_;
	}
	ShaderStage GetStage() const override {
		return stage_;
	}

	void GLLost() override {
		ILOG("Shader module lost");
		// Shader has been destroyed since the old context is gone, so let's zero it.
		shader_ = 0;
	}

	void GLRestore() override {
		ILOG("Shader module being restored");
		if (!Compile(render_, language_, (const uint8_t *)source_.data(), source_.size())) {
			ELOG("Shader restore compilation failed: %s", source_.c_str());
		}
	}

private:
	GLRenderManager *render_;
	ShaderStage stage_;
	ShaderLanguage language_;
	GLRShader *shader_;
	GLuint glstage_ = 0;
	bool ok_ = false;
	std::string source_;  // So we can recompile in case of context loss.
};

bool OpenGLShaderModule::Compile(GLRenderManager *render, ShaderLanguage language, const uint8_t *data, size_t dataSize) {
	source_ = std::string((const char *)data);
	std::string temp;
	// Add the prelude on automatically.
	if (glstage_ == GL_FRAGMENT_SHADER || glstage_ == GL_VERTEX_SHADER) {
		temp = ApplyGLSLPrelude(source_, glstage_);
		source_ = temp.c_str();
	}

	shader_ = render->CreateShader(glstage_, source_);
	return true;
}

class OpenGLInputLayout : public InputLayout {
public:
	OpenGLInputLayout(GLRenderManager *render) : render_(render) {}
	~OpenGLInputLayout();

	void Compile(const InputLayoutDesc &desc);
	bool RequiresBuffer() {
		return false;
	}

	GLRInputLayout *inputLayout_;
	int stride;
private:
	GLRenderManager *render_;
};

class OpenGLPipeline : public Pipeline, GfxResourceHolder {
public:
	OpenGLPipeline(GLRenderManager *render) : render_(render) {
		// Priority 1 so this gets restored after the shaders.
		register_gl_resource_holder(this, "drawcontext_pipeline", 1);
	}
	~OpenGLPipeline() {
		unregister_gl_resource_holder(this);
		for (auto &iter : shaders) {
			iter->Release();
		}
		render_->DeleteProgram(program_);
		if (depthStencil) depthStencil->Release();
		if (blend) blend->Release();
		if (raster) raster->Release();
		if (inputLayout) inputLayout->Release();
	}

	bool LinkShaders();

	void GLLost() override {
		render_->DeleteProgram(program_);
	}

	void GLRestore() override {
		// Shaders will have been restored before the pipeline.
		LinkShaders();
	}

	bool RequiresBuffer() override {
		return inputLayout->RequiresBuffer();
	}

	GLuint prim;
	std::vector<OpenGLShaderModule *> shaders;
	OpenGLInputLayout *inputLayout = nullptr;
	OpenGLDepthStencilState *depthStencil = nullptr;
	OpenGLBlendState *blend = nullptr;
	OpenGLRasterState *raster = nullptr;

	// TODO: Optimize by getting the locations first and putting in a custom struct
	UniformBufferDesc dynamicUniforms;

	GLRProgram *program_ = nullptr;
private:
	GLRenderManager *render_;
};

class OpenGLFramebuffer;
class OpenGLTexture;

class OpenGLContext : public DrawContext {
public:
	OpenGLContext();
	virtual ~OpenGLContext();

	const DeviceCaps &GetDeviceCaps() const override {
		return caps_;
	}
	uint32_t GetSupportedShaderLanguages() const override {
#if defined(USING_GLES2)
		return (uint32_t)ShaderLanguage::GLSL_ES_200 | (uint32_t)ShaderLanguage::GLSL_ES_300;
#else
		return (uint32_t)ShaderLanguage::GLSL_ES_200 | (uint32_t)ShaderLanguage::GLSL_410;
#endif
	}
	uint32_t GetDataFormatSupport(DataFormat fmt) const override;

	DepthStencilState *CreateDepthStencilState(const DepthStencilStateDesc &desc) override;
	BlendState *CreateBlendState(const BlendStateDesc &desc) override;
	SamplerState *CreateSamplerState(const SamplerStateDesc &desc) override;
	RasterState *CreateRasterState(const RasterStateDesc &desc) override;
	Pipeline *CreateGraphicsPipeline(const PipelineDesc &desc) override;
	InputLayout *CreateInputLayout(const InputLayoutDesc &desc) override;
	ShaderModule *CreateShaderModule(ShaderStage stage, ShaderLanguage language, const uint8_t *data, size_t dataSize) override;

	Texture *CreateTexture(const TextureDesc &desc) override;
	Buffer *CreateBuffer(size_t size, uint32_t usageFlags) override;
	Framebuffer *CreateFramebuffer(const FramebufferDesc &desc) override;

	void BeginFrame() override;
	void EndFrame() override;

	void UpdateBuffer(Buffer *buffer, const uint8_t *data, size_t offset, size_t size, UpdateBufferFlags flags) override;

	void CopyFramebufferImage(Framebuffer *src, int level, int x, int y, int z, Framebuffer *dst, int dstLevel, int dstX, int dstY, int dstZ, int width, int height, int depth, int channelBits) override;
	bool BlitFramebuffer(Framebuffer *src, int srcX1, int srcY1, int srcX2, int srcY2, Framebuffer *dst, int dstX1, int dstY1, int dstX2, int dstY2, int channelBits, FBBlitFilter filter) override;
	bool CopyFramebufferToMemorySync(Framebuffer *src, int channelBits, int x, int y, int w, int h, Draw::DataFormat format, void *pixels, int pixelStride) override;

	// These functions should be self explanatory.
	void BindFramebufferAsRenderTarget(Framebuffer *fbo, const RenderPassInfo &rp) override;
	// color must be 0, for now.
	void BindFramebufferAsTexture(Framebuffer *fbo, int binding, FBChannel channelBit, int attachment) override;

	void GetFramebufferDimensions(Framebuffer *fbo, int *w, int *h) override;

	void BindSamplerStates(int start, int count, SamplerState **states) override {
		if (boundSamplers_.size() < (size_t)(start + count)) {
			boundSamplers_.resize(start + count);
		}
		for (int i = 0; i < count; i++) {
			int index = i + start;
			boundSamplers_[index] = static_cast<OpenGLSamplerState *>(states[index]);
		}
	}

	void SetScissorRect(int left, int top, int width, int height) override {
		renderManager_.SetScissor({ left, targetHeight_ - (top + height), width, height });
	}

	void SetViewports(int count, Viewport *viewports) override {
		// Same structure, different name.
		renderManager_.SetViewport((GLRViewport &)*viewports);
	}

	void SetBlendFactor(float color[4]) override {
		renderManager_.SetBlendFactor(color);
	}

	void BindTextures(int start, int count, Texture **textures) override;
	void BindPipeline(Pipeline *pipeline) override;
	void BindVertexBuffers(int start, int count, Buffer **buffers, int *offsets) override {
		for (int i = 0; i < count; i++) {
			curVBuffers_[i + start] = (OpenGLBuffer  *)buffers[i];
			curVBufferOffsets_[i + start] = offsets ? offsets[i] : 0;
		}
	}
	void BindIndexBuffer(Buffer *indexBuffer, int offset) override {
		curIBuffer_ = (OpenGLBuffer  *)indexBuffer;
		curIBufferOffset_ = offset;
	}

	void UpdateDynamicUniformBuffer(const void *ub, size_t size) override;

	// TODO: Add more sophisticated draws.
	void Draw(int vertexCount, int offset) override;
	void DrawIndexed(int vertexCount, int offset) override;
	void DrawUP(const void *vdata, int vertexCount) override;

	void Clear(int mask, uint32_t colorval, float depthVal, int stencilVal) override;

	std::string GetInfoString(InfoField info) const override {
		// TODO: Make these actually query the right information
		switch (info) {
			case APINAME:
				if (gl_extensions.IsGLES) {
					return "OpenGL ES";
				} else {
					return "OpenGL";
				}
			case VENDORSTRING: return (const char *)glGetString(GL_VENDOR);
			case VENDOR:
				switch (gl_extensions.gpuVendor) {
				case GPU_VENDOR_AMD: return "VENDOR_AMD";
				case GPU_VENDOR_POWERVR: return "VENDOR_POWERVR";
				case GPU_VENDOR_NVIDIA: return "VENDOR_NVIDIA";
				case GPU_VENDOR_INTEL: return "VENDOR_INTEL";
				case GPU_VENDOR_ADRENO: return "VENDOR_ADRENO";
				case GPU_VENDOR_ARM: return "VENDOR_ARM";
				case GPU_VENDOR_BROADCOM: return "VENDOR_BROADCOM";
				case GPU_VENDOR_UNKNOWN:
				default:
					return "VENDOR_UNKNOWN";
				}
				break;
			case DRIVER: return (const char *)glGetString(GL_RENDERER);
			case SHADELANGVERSION: return (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
			case APIVERSION: return (const char *)glGetString(GL_VERSION);
			default: return "?";
		}
	}

	uintptr_t GetNativeObject(NativeObject obj) override {
		switch (obj) {
		case NativeObject::RENDER_MANAGER:
			return (uintptr_t)&renderManager_;
		default:
			return 0;
		}
	}

	void HandleEvent(Event ev, int width, int height, void *param1, void *param2) override {}

private:
	OpenGLFramebuffer *fbo_ext_create(const FramebufferDesc &desc);
	void fbo_bind_fb_target(bool read, GLuint name);
	GLenum fbo_get_fb_target(bool read, GLuint **cached);
	void fbo_unbind();
	void ApplySamplers();

	GLRenderManager renderManager_;

	std::vector<OpenGLSamplerState *> boundSamplers_;
	OpenGLTexture *boundTextures_[8]{};
	int maxTextures_ = 0;
	DeviceCaps caps_{};
	
	// Bound state
	OpenGLPipeline *curPipeline_ = nullptr;
	OpenGLBuffer *curVBuffers_[4]{};
	int curVBufferOffsets_[4]{};
	OpenGLBuffer *curIBuffer_ = nullptr;
	int curIBufferOffset_ = 0;

	// Framebuffer state
	GLuint currentDrawHandle_ = 0;
	GLuint currentReadHandle_ = 0;

	// Frames in flight is not such a strict concept as with Vulkan until we start using glBufferStorage and fences.
	// But might as well have the structure ready, and can't hurt to rotate buffers.
	struct FrameData {
		GLPushBuffer *push;
	};
	FrameData frameData_[GLRenderManager::MAX_INFLIGHT_FRAMES];
};

OpenGLContext::OpenGLContext() {
	// TODO: Detect more caps
	if (gl_extensions.IsGLES) {
		if (gl_extensions.OES_packed_depth_stencil || gl_extensions.OES_depth24) {
			caps_.preferredDepthBufferFormat = DataFormat::D24_S8;
		} else {
			caps_.preferredDepthBufferFormat = DataFormat::D16;
		}
	} else {
		caps_.preferredDepthBufferFormat = DataFormat::D24_S8;
	}
	caps_.framebufferBlitSupported = gl_extensions.NV_framebuffer_blit || gl_extensions.ARB_framebuffer_object;
	caps_.framebufferDepthBlitSupported = caps_.framebufferBlitSupported;

	for (int i = 0; i < GLRenderManager::MAX_INFLIGHT_FRAMES; i++) {
		frameData_[i].push = new GLPushBuffer(&renderManager_, 64 * 1024);
	}
}

OpenGLContext::~OpenGLContext() {
	for (int i = 0; i < GLRenderManager::MAX_INFLIGHT_FRAMES; i++) {
		frameData_[i].push->Destroy();
		delete frameData_[i].push;
	}
	boundSamplers_.clear();
}

void OpenGLContext::BeginFrame() {
	renderManager_.BeginFrame();
	FrameData &frameData = frameData_[renderManager_.GetCurFrame()];
	frameData.push->Begin();
}

void OpenGLContext::EndFrame() {
	FrameData &frameData = frameData_[renderManager_.GetCurFrame()];
	frameData.push->End();  // upload the data!
	renderManager_.Finish();
}

InputLayout *OpenGLContext::CreateInputLayout(const InputLayoutDesc &desc) {
	OpenGLInputLayout *fmt = new OpenGLInputLayout(&renderManager_);
	fmt->Compile(desc);
	return fmt;
}

GLuint TypeToTarget(TextureType type) {
	switch (type) {
#ifndef USING_GLES2
	case TextureType::LINEAR1D: return GL_TEXTURE_1D;
#endif
	case TextureType::LINEAR2D: return GL_TEXTURE_2D;
	case TextureType::LINEAR3D: return GL_TEXTURE_3D;
	case TextureType::CUBE: return GL_TEXTURE_CUBE_MAP;
#ifndef USING_GLES2
	case TextureType::ARRAY1D: return GL_TEXTURE_1D_ARRAY;
#endif
	case TextureType::ARRAY2D: return GL_TEXTURE_2D_ARRAY;
	default:
		ELOG("Bad texture type %d", (int)type);
		return GL_NONE;
	}
}

inline bool isPowerOf2(int n) {
	return n == 1 || (n & (n - 1)) == 0;
}

class OpenGLTexture : public Texture {
public:
	OpenGLTexture(GLRenderManager *render, const TextureDesc &desc);
	~OpenGLTexture();

	bool HasMips() const {
		return mipLevels_ > 1 || generatedMips_;
	}
	bool CanWrap() const {
		return canWrap_;
	}
	TextureType GetType() const { return type_; }
	void Bind(int stage) {
		render_->BindTexture(stage, tex_);
	}

	void AutoGenMipmaps();

private:
	void SetImageData(int x, int y, int z, int width, int height, int depth, int level, int stride, const uint8_t *data);

	GLRenderManager *render_;
	GLRTexture *tex_;

	DataFormat format_;
	TextureType type_;
	int mipLevels_;
	bool generatedMips_;
	bool canWrap_;
};

OpenGLTexture::OpenGLTexture(GLRenderManager *render, const TextureDesc &desc) : render_(render) {
	generatedMips_ = false;
	canWrap_ = true;
	width_ = desc.width;
	height_ = desc.height;
	depth_ = desc.depth;
	format_ = desc.format;
	type_ = desc.type;
	GLenum target = TypeToTarget(desc.type);
	tex_ = render->CreateTexture(target, width_, height_);

	canWrap_ = isPowerOf2(width_) && isPowerOf2(height_);
	mipLevels_ = desc.mipLevels;
	if (!desc.initData.size())
		return;

	int level = 0;
	for (auto data : desc.initData) {
		SetImageData(0, 0, 0, width_, height_, depth_, level, 0, data);
		width_ = (width_ + 1) / 2;
		height_ = (height_ + 1) / 2;
		level++;
	}
	mipLevels_ = desc.generateMips ? desc.mipLevels : level;

	/*
#ifdef USING_GLES2
	if (gl_extensions.GLES3) {
		glTexParameteri(target_, GL_TEXTURE_MAX_LEVEL, mipLevels_ - 1);
	}
#else
	glTexParameteri(target_, GL_TEXTURE_MAX_LEVEL, mipLevels_ - 1);
#endif
*/

	if ((int)desc.initData.size() < desc.mipLevels && desc.generateMips) {
		ILOG("Generating mipmaps");
		AutoGenMipmaps();
	}

}

OpenGLTexture::~OpenGLTexture() {
	if (tex_) {
		render_->DeleteTexture(tex_);
		tex_ = 0;
		generatedMips_ = false;
	}
}

void OpenGLTexture::AutoGenMipmaps() {
	if (!generatedMips_) {
		// Assumes the texture is bound for editing
		render_->GenerateMipmap();
		generatedMips_ = true;
	}
}

class OpenGLFramebuffer : public Framebuffer, public GfxResourceHolder {
public:
	OpenGLFramebuffer() {
		register_gl_resource_holder(this, "framebuffer", 0);
	}
	~OpenGLFramebuffer();

	void GLLost() override {
		handle = 0;
		color_texture = 0;
		z_stencil_buffer = 0;
		z_buffer = 0;
		stencil_buffer = 0;
	}

	void GLRestore() override {
		ELOG("Restoring framebuffers not yet implemented");
	}

	GLRFramebuffer *framebuffer;

	GLuint handle = 0;
	GLuint color_texture = 0;
	GLuint z_stencil_buffer = 0;  // Either this is set, or the two below.
	GLuint z_buffer = 0;
	GLuint stencil_buffer = 0;

	int width;
	int height;
	FBColorDepth colorDepth;
};


// TODO: Also output storage format (GL_RGBA8 etc) for modern GL usage.
static bool Thin3DFormatToFormatAndType(DataFormat fmt, GLuint &internalFormat, GLuint &format, GLuint &type, int &alignment) {
	alignment = 4;
	switch (fmt) {
	case DataFormat::R8G8B8A8_UNORM:
		internalFormat = GL_RGBA;
		format = GL_RGBA;
		type = GL_UNSIGNED_BYTE;
		break;

	case DataFormat::D32F:
		internalFormat = GL_DEPTH_COMPONENT;
		format = GL_DEPTH_COMPONENT;
		type = GL_FLOAT;
		break;

#ifndef USING_GLES2
	case DataFormat::S8:
		internalFormat = GL_STENCIL_INDEX;
		format = GL_STENCIL_INDEX;
		type = GL_UNSIGNED_BYTE;
		alignment = 1;
		break;
#endif

	case DataFormat::R8G8B8_UNORM:
		internalFormat = GL_RGB;
		format = GL_RGB;
		type = GL_UNSIGNED_BYTE;
		alignment = 1;
		break;

	case DataFormat::B4G4R4A4_UNORM_PACK16:
		internalFormat = GL_RGBA;
		format = GL_RGBA;
		type = GL_UNSIGNED_SHORT_4_4_4_4;
		alignment = 2;
		break;

	case DataFormat::B5G6R5_UNORM_PACK16:
		internalFormat = GL_RGB;
		format = GL_RGB;
		type = GL_UNSIGNED_SHORT_5_6_5;
		alignment = 2;
		break;

	case DataFormat::B5G5R5A1_UNORM_PACK16:
		internalFormat = GL_RGBA;
		format = GL_RGBA;
		type = GL_UNSIGNED_SHORT_5_5_5_1;
		alignment = 2;
		break;

#ifndef USING_GLES2
	case DataFormat::A4R4G4B4_UNORM_PACK16:
		internalFormat = GL_RGBA;
		format = GL_RGBA;
		type = GL_UNSIGNED_SHORT_4_4_4_4_REV;
		alignment = 2;
		break;

	case DataFormat::R5G6B5_UNORM_PACK16:
		internalFormat = GL_RGB;
		format = GL_RGB;
		type = GL_UNSIGNED_SHORT_5_6_5_REV;
		alignment = 2;
		break;

	case DataFormat::A1R5G5B5_UNORM_PACK16:
		internalFormat = GL_RGBA;
		format = GL_RGBA;
		type = GL_UNSIGNED_SHORT_1_5_5_5_REV;
		alignment = 2;
		break;
#endif

	default:
		ELOG("Thin3d GL: Unsupported texture format %d", (int)fmt);
		return false;
	}
	return true;
}

void OpenGLTexture::SetImageData(int x, int y, int z, int width, int height, int depth, int level, int stride, const uint8_t *data) {
	if (width != width_ || height != height_ || depth != depth_) {
		// When switching to texStorage we need to handle this correctly.
		width_ = width;
		height_ = height;
		depth_ = depth;
	}

	GLuint internalFormat;
	GLuint format;
	GLuint type;
	int alignment;
	if (!Thin3DFormatToFormatAndType(format_, internalFormat, format, type, alignment)) {
		return;
	}

	if (stride == 0)
		stride = width;

	// Make a copy of data with stride eliminated.
	uint8_t *texData = new uint8_t[width * height * alignment];
	for (int y = 0; y < height; y++) {
		memcpy(texData + y * width * alignment, data + y * stride * alignment, width * alignment);
	}
	render_->TextureImage(tex_, level, width, height, internalFormat, format, type, texData);
}

#ifdef DEBUG_READ_PIXELS
// TODO: Make more generic.
static void LogReadPixelsError(GLenum error) {
	switch (error) {
	case GL_NO_ERROR:
		break;
	case GL_INVALID_ENUM:
		ERROR_LOG(FRAMEBUF, "glReadPixels: GL_INVALID_ENUM");
		break;
	case GL_INVALID_VALUE:
		ERROR_LOG(FRAMEBUF, "glReadPixels: GL_INVALID_VALUE");
		break;
	case GL_INVALID_OPERATION:
		ERROR_LOG(FRAMEBUF, "glReadPixels: GL_INVALID_OPERATION");
		break;
	case GL_INVALID_FRAMEBUFFER_OPERATION:
		ERROR_LOG(FRAMEBUF, "glReadPixels: GL_INVALID_FRAMEBUFFER_OPERATION");
		break;
	case GL_OUT_OF_MEMORY:
		ERROR_LOG(FRAMEBUF, "glReadPixels: GL_OUT_OF_MEMORY");
		break;
#ifndef USING_GLES2
	case GL_STACK_UNDERFLOW:
		ERROR_LOG(FRAMEBUF, "glReadPixels: GL_STACK_UNDERFLOW");
		break;
	case GL_STACK_OVERFLOW:
		ERROR_LOG(FRAMEBUF, "glReadPixels: GL_STACK_OVERFLOW");
		break;
#endif
	default:
		ERROR_LOG(FRAMEBUF, "glReadPixels: %08x", error);
		break;
	}
}
#endif

bool OpenGLContext::CopyFramebufferToMemorySync(Framebuffer *src, int channelBits, int x, int y, int w, int h, Draw::DataFormat dataFormat, void *pixels, int pixelStride) {
	OpenGLFramebuffer *fb = (OpenGLFramebuffer *)src;
	fbo_bind_fb_target(true, fb ? fb->handle : 0);

	// Reads from the "bound for read" framebuffer.
	if (gl_extensions.GLES3 || !gl_extensions.IsGLES)
		glReadBuffer(GL_COLOR_ATTACHMENT0);

	CHECK_GL_ERROR_IF_DEBUG();

	GLuint internalFormat;
	GLuint format;
	GLuint type;
	int alignment;
	if (!Thin3DFormatToFormatAndType(dataFormat, internalFormat, format, type, alignment)) {
		assert(false);
	}
	// Apply the correct alignment.
	glPixelStorei(GL_PACK_ALIGNMENT, alignment);
	if (!gl_extensions.IsGLES || (gl_extensions.GLES3 && gl_extensions.gpuVendor != GPU_VENDOR_NVIDIA)) {
		// Some drivers seem to require we specify this.  See #8254.
		glPixelStorei(GL_PACK_ROW_LENGTH, pixelStride);
	}

	glReadPixels(x, y, w, h, format, type, pixels);
#ifdef DEBUG_READ_PIXELS
	LogReadPixelsError(glGetError());
#endif

	if (!gl_extensions.IsGLES || gl_extensions.GLES3) {
		glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	}
	CHECK_GL_ERROR_IF_DEBUG();
	return true;
}


Texture *OpenGLContext::CreateTexture(const TextureDesc &desc) {
	return new OpenGLTexture(&renderManager_, desc);
}

DepthStencilState *OpenGLContext::CreateDepthStencilState(const DepthStencilStateDesc &desc) {
	OpenGLDepthStencilState *ds = new OpenGLDepthStencilState();
	ds->depthTestEnabled = desc.depthTestEnabled;
	ds->depthWriteEnabled = desc.depthWriteEnabled;
	ds->depthComp = compToGL[(int)desc.depthCompare];
	ds->stencilEnabled = desc.stencilEnabled;
	ds->stencilCompareOp = compToGL[(int)desc.front.compareOp];
	ds->stencilPass = stencilOpToGL[(int)desc.front.passOp];
	ds->stencilFail = stencilOpToGL[(int)desc.front.failOp];
	ds->stencilZFail = stencilOpToGL[(int)desc.front.depthFailOp];
	ds->stencilWriteMask = desc.front.writeMask;
	ds->stencilReference = desc.front.reference;
	ds->stencilCompareMask = desc.front.compareMask;
	return ds;
}

BlendState *OpenGLContext::CreateBlendState(const BlendStateDesc &desc) {
	OpenGLBlendState *bs = new OpenGLBlendState();
	bs->enabled = desc.enabled;
	bs->eqCol = blendEqToGL[(int)desc.eqCol];
	bs->srcCol = blendFactorToGL[(int)desc.srcCol];
	bs->dstCol = blendFactorToGL[(int)desc.dstCol];
	bs->eqAlpha = blendEqToGL[(int)desc.eqAlpha];
	bs->srcAlpha = blendFactorToGL[(int)desc.srcAlpha];
	bs->dstAlpha = blendFactorToGL[(int)desc.dstAlpha];
	bs->colorMask = desc.colorMask;
	return bs;
}

SamplerState *OpenGLContext::CreateSamplerState(const SamplerStateDesc &desc) {
	OpenGLSamplerState *samps = new OpenGLSamplerState();
	samps->wrapU = texWrapToGL[(int)desc.wrapU];
	samps->wrapV = texWrapToGL[(int)desc.wrapV];
	samps->wrapW = texWrapToGL[(int)desc.wrapW];
	samps->magFilt = texFilterToGL[(int)desc.magFilter];
	samps->minFilt = texFilterToGL[(int)desc.minFilter];
	samps->mipMinFilt = texMipFilterToGL[(int)desc.minFilter][(int)desc.mipFilter];
	return samps;
}

RasterState *OpenGLContext::CreateRasterState(const RasterStateDesc &desc) {
	OpenGLRasterState *rs = new OpenGLRasterState();
	if (desc.cull == CullMode::NONE) {
		rs->cullEnable = GL_FALSE;
		return rs;
	}
	rs->cullEnable = GL_TRUE;
	switch (desc.frontFace) {
	case Facing::CW:
		rs->frontFace = GL_CW;
		break;
	case Facing::CCW:
		rs->frontFace = GL_CCW;
		break;
	}
	switch (desc.cull) {
	case CullMode::FRONT:
		rs->cullMode = GL_FRONT;
		break;
	case CullMode::BACK:
		rs->cullMode = GL_BACK;
		break;
	case CullMode::FRONT_AND_BACK:
		rs->cullMode = GL_FRONT_AND_BACK;
		break;
	case CullMode::NONE:
		// Unsupported
		break;
	}
	return rs;
}

class OpenGLBuffer : public Buffer, GfxResourceHolder {
public:
	OpenGLBuffer(GLRenderManager *render, size_t size, uint32_t flags) : render_(render) {
		target_ = (flags & BufferUsageFlag::INDEXDATA) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
		usage_ = 0;
		if (flags & BufferUsageFlag::DYNAMIC)
			usage_ = GL_STREAM_DRAW;
		else
			usage_ = GL_STATIC_DRAW;
		buffer_ = render->CreateBuffer(target_, size, usage_);
		totalSize_ = size;
		register_gl_resource_holder(this, "drawcontext_buffer", 0);
	}
	~OpenGLBuffer() override {
		unregister_gl_resource_holder(this);
		render_->DeleteBuffer(buffer_);
	}

	void Bind(int offset) {
		Crash();
		// render_->BindBuffer(buffer_);
		// TODO: Can't support offset using ES 2.0
		// glBindBuffer(target_, buffer_);
	}

	void GLLost() override {
		buffer_ = 0;
	}

	void GLRestore() override {
		ILOG("Recreating vertex buffer after gl_restore");
		totalSize_ = 0;  // Will cause a new glBufferData call. Should genBuffers again though?
		render_->DeleteBuffer(buffer_);
		buffer_ = render_->CreateBuffer(target_, totalSize_, usage_);
	}

	GLRenderManager *render_;
	GLRBuffer *buffer_;
	GLuint target_;
	GLuint usage_;

	size_t totalSize_;
};

Buffer *OpenGLContext::CreateBuffer(size_t size, uint32_t usageFlags) {
	return new OpenGLBuffer(&renderManager_, size, usageFlags);
}

void OpenGLContext::UpdateBuffer(Buffer *buffer, const uint8_t *data, size_t offset, size_t size, UpdateBufferFlags flags) {
	OpenGLBuffer *buf = (OpenGLBuffer *)buffer;

	buf->Bind(0);
	if (size + offset > buf->totalSize_) {
		Crash();
	}

	uint8_t *dataCopy = new uint8_t[size];
	memcpy(dataCopy, data, size);
	// if (flags & UPDATE_DISCARD) we could try to orphan the buffer using glBufferData.
	renderManager_.BufferSubdata(buf->buffer_, offset, size, dataCopy);
}

Pipeline *OpenGLContext::CreateGraphicsPipeline(const PipelineDesc &desc) {
	if (!desc.shaders.size()) {
		ELOG("Pipeline requires at least one shader");
		return nullptr;
	}
	OpenGLPipeline *pipeline = new OpenGLPipeline(&renderManager_);
	for (auto iter : desc.shaders) {
		iter->AddRef();
		pipeline->shaders.push_back(static_cast<OpenGLShaderModule *>(iter));
	}
	if (pipeline->LinkShaders()) {
		// Build the rest of the virtual pipeline object.
		pipeline->prim = primToGL[(int)desc.prim];
		pipeline->depthStencil = (OpenGLDepthStencilState *)desc.depthStencil;
		pipeline->blend = (OpenGLBlendState *)desc.blend;
		pipeline->raster = (OpenGLRasterState *)desc.raster;
		pipeline->inputLayout = (OpenGLInputLayout *)desc.inputLayout;
		pipeline->depthStencil->AddRef();
		pipeline->blend->AddRef();
		pipeline->raster->AddRef();
		pipeline->inputLayout->AddRef();
		if (desc.uniformDesc)
			pipeline->dynamicUniforms = *desc.uniformDesc;
		return pipeline;
	} else {
		ELOG("Failed to create pipeline - shaders failed to link");
		delete pipeline;
		return NULL;
	}
}

void OpenGLContext::BindTextures(int start, int count, Texture **textures) {
	maxTextures_ = std::max(maxTextures_, start + count);
	for (int i = start; i < start + count; i++) {
		OpenGLTexture *glTex = static_cast<OpenGLTexture *>(textures[i]);
		if (!glTex) {
			boundTextures_[i] = 0;
			renderManager_.BindTexture(i, nullptr);
			continue;
		}
		glTex->Bind(i);
		boundTextures_[i] = glTex;
	}
}

void OpenGLContext::ApplySamplers() {
	for (int i = 0; i < maxTextures_; i++) {
		if ((int)boundSamplers_.size() > i && boundSamplers_[i]) {
			const OpenGLSamplerState *samp = boundSamplers_[i];
			const OpenGLTexture *tex = boundTextures_[i];
			if (!tex)
				continue;
			GLenum wrapS;
			GLenum wrapT;
			if (tex->CanWrap()) {
				wrapS = samp->wrapU;
				wrapT = samp->wrapV;
			} else {
				wrapS = GL_CLAMP_TO_EDGE;
				wrapT = GL_CLAMP_TO_EDGE;
			}
			GLenum magFilt = samp->magFilt;
			GLenum minFilt = tex->HasMips() ? samp->mipMinFilt : samp->minFilt;
			renderManager_.SetTextureSampler(wrapS, wrapT, magFilt, minFilt);
		}
	}
}

ShaderModule *OpenGLContext::CreateShaderModule(ShaderStage stage, ShaderLanguage language, const uint8_t *data, size_t dataSize) {
	OpenGLShaderModule *shader = new OpenGLShaderModule(&renderManager_, stage);
	if (shader->Compile(&renderManager_, language, data, dataSize)) {
		return shader;
	} else {
		shader->Release();
		return nullptr;
	}
}

bool OpenGLPipeline::LinkShaders() {
	std::vector<GLRShader *> linkShaders;
	for (auto iter : shaders) {
		linkShaders.push_back(iter->GetShader());
	}
	std::vector<GLRProgram::Semantic> semantics;
	// Bind all the common vertex data points. Mismatching ones will be ignored.
	semantics.push_back({ SEM_POSITION, "Position" });
	semantics.push_back({ SEM_COLOR0, "Color0" });
	semantics.push_back({ SEM_TEXCOORD0, "TexCoord0" });
	semantics.push_back({ SEM_NORMAL, "Normal" });
	semantics.push_back({ SEM_TANGENT, "Tangent" });
	semantics.push_back({ SEM_BINORMAL, "Binormal" });
	program_ = render_->CreateProgram(linkShaders, semantics);
	return true;
}

void OpenGLContext::BindPipeline(Pipeline *pipeline) {
	curPipeline_ = (OpenGLPipeline *)pipeline;
	curPipeline_->blend->Apply(&renderManager_);
	curPipeline_->depthStencil->Apply(&renderManager_);
	curPipeline_->raster->Apply(&renderManager_);
	renderManager_.BindProgram(curPipeline_->program_);
}

void OpenGLContext::UpdateDynamicUniformBuffer(const void *ub, size_t size) {
	if (curPipeline_->dynamicUniforms.uniformBufferSize != size) {
		Crash();
	}

	for (auto &uniform : curPipeline_->dynamicUniforms.uniforms) {
		const float *data = (const float *)((uint8_t *)ub + uniform.offset);
		switch (uniform.type) {
		case UniformType::FLOAT4:
			renderManager_.SetUniformF(uniform.name, 4, data);
			break;
		case UniformType::MATRIX4X4:
			renderManager_.SetUniformM4x4(uniform.name, data);
			break;
		}
	}
}

void OpenGLContext::Draw(int vertexCount, int offset) {
	curVBuffers_[0]->Bind(curVBufferOffsets_[0]);
	renderManager_.BindInputLayout(curPipeline_->inputLayout->inputLayout_, 0);
	ApplySamplers();

	renderManager_.Draw(curPipeline_->prim, offset, vertexCount);

	renderManager_.UnbindInputLayout(curPipeline_->inputLayout->inputLayout_);
}

void OpenGLContext::DrawIndexed(int vertexCount, int offset) {
	curVBuffers_[0]->Bind(curVBufferOffsets_[0]);
	renderManager_.BindInputLayout(curPipeline_->inputLayout->inputLayout_, 0);
	ApplySamplers();
	// Note: ibuf binding is stored in the VAO, so call this after binding the fmt.
	curIBuffer_->Bind(curIBufferOffset_);

	renderManager_.DrawIndexed(curPipeline_->prim, vertexCount, GL_UNSIGNED_INT, (void *)(intptr_t)offset);
	renderManager_.UnbindInputLayout(curPipeline_->inputLayout->inputLayout_);
}

void OpenGLContext::DrawUP(const void *vdata, int vertexCount) {
#if 1
	int stride = curPipeline_->inputLayout->stride;
	size_t dataSize = stride * vertexCount;

	FrameData &frameData = frameData_[renderManager_.GetCurFrame()];

	GLRBuffer *buf;
	size_t offset = frameData.push->Push(vdata, dataSize, &buf);

	ApplySamplers();

	renderManager_.BindVertexBuffer(buf);
	renderManager_.BindInputLayout(curPipeline_->inputLayout->inputLayout_, (void *)offset);

	renderManager_.Draw(curPipeline_->prim, 0, vertexCount);
	renderManager_.UnbindInputLayout(curPipeline_->inputLayout->inputLayout_);
	renderManager_.BindVertexBuffer(nullptr);
#else
	ApplySamplers();
	renderManager_.BindInputLayout(curPipeline_->inputLayout->inputLayout_, (void *)vdata);
	renderManager_.Draw(curPipeline_->prim, 0, vertexCount);
	renderManager_.UnbindInputLayout(curPipeline_->inputLayout->inputLayout_);

#endif
}

void OpenGLContext::Clear(int mask, uint32_t colorval, float depthVal, int stencilVal) {
	float col[4];
	Uint8x4ToFloat4(col, colorval);
	GLuint glMask = 0;
	if (mask & FBChannel::FB_COLOR_BIT) {
		glMask |= GL_COLOR_BUFFER_BIT;
	}
	if (mask & FBChannel::FB_DEPTH_BIT) {
		glMask |= GL_DEPTH_BUFFER_BIT;
	}
	if (mask & FBChannel::FB_STENCIL_BIT) {
		glMask |= GL_STENCIL_BUFFER_BIT;
	}
	renderManager_.Clear(colorval, depthVal, stencilVal, glMask);
}

DrawContext *T3DCreateGLContext() {
	return new OpenGLContext();
}

OpenGLInputLayout::~OpenGLInputLayout() {
}

void OpenGLInputLayout::Compile(const InputLayoutDesc &desc) {
	int semMask = 0;

	// This is only accurate if there's only one stream. But whatever, for now.
	stride = (GLsizei)desc.bindings[0].stride;

	std::vector<GLRInputLayout::Entry> entries;
	for (auto &attr : desc.attributes) {
		GLRInputLayout::Entry entry;
		entry.location = attr.location;
		entry.stride = (GLsizei)desc.bindings[attr.binding].stride;
		entry.offset = attr.offset;
		switch (attr.format) {
		case DataFormat::R32G32_FLOAT:
			entry.count = 2;
			entry.type = GL_FLOAT;
			entry.normalized = GL_FALSE;
			break;
		case DataFormat::R32G32B32_FLOAT:
			entry.count = 3;
			entry.type = GL_FLOAT;
			entry.normalized = GL_FALSE;
			break;
		case DataFormat::R32G32B32A32_FLOAT:
			entry.count = 4;
			entry.type = GL_FLOAT;
			entry.normalized = GL_FALSE;
			break;
		case DataFormat::R8G8B8A8_UNORM:
			entry.count = 4;
			entry.type = GL_UNSIGNED_BYTE;
			entry.normalized = GL_TRUE;
			break;
		case DataFormat::UNDEFINED:
		default:
			ELOG("Thin3DGLVertexFormat: Invalid or unknown component type applied.");
			break;
		}

		entries.push_back(entry);
	}
	inputLayout_ = render_->CreateInputLayout(entries);
}

// On PC, we always use GL_DEPTH24_STENCIL8. 
// On Android, we try to use what's available.

#ifndef USING_GLES2
OpenGLFramebuffer *OpenGLContext::fbo_ext_create(const FramebufferDesc &desc) {
	OpenGLFramebuffer *fbo = new OpenGLFramebuffer();
	fbo->width = desc.width;
	fbo->height = desc.height;
	fbo->colorDepth = desc.colorDepth;

	// Color texture is same everywhere
	glGenFramebuffersEXT(1, &fbo->handle);
	glGenTextures(1, &fbo->color_texture);

	// Create the surfaces.
	glBindTexture(GL_TEXTURE_2D, fbo->color_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	// TODO: We could opt to only create 16-bit render targets on slow devices. For later.
	switch (fbo->colorDepth) {
	case FBO_8888:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		break;
	case FBO_4444:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, NULL);
		break;
	case FBO_5551:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, NULL);
		break;
	case FBO_565:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fbo->width, fbo->height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
		break;
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	fbo->stencil_buffer = 0;
	fbo->z_buffer = 0;
	// 24-bit Z, 8-bit stencil
	glGenRenderbuffersEXT(1, &fbo->z_stencil_buffer);
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, fbo->z_stencil_buffer);
	glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH_STENCIL_EXT, fbo->width, fbo->height);
	//glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8, width, height);

	// Bind it all together
	glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo->handle);
	glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, fbo->color_texture, 0);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, fbo->z_stencil_buffer);
	glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, fbo->z_stencil_buffer);

	GLenum status = glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
	switch (status) {
	case GL_FRAMEBUFFER_COMPLETE_EXT:
		// ILOG("Framebuffer verified complete.");
		break;
	case GL_FRAMEBUFFER_UNSUPPORTED_EXT:
		ELOG("GL_FRAMEBUFFER_UNSUPPORTED");
		break;
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_EXT:
		ELOG("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT ");
		break;
	default:
		FLOG("Other framebuffer error: %i", status);
		break;
	}
	// Unbind state we don't need
	glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	currentDrawHandle_ = fbo->handle;
	currentReadHandle_ = fbo->handle;
	return fbo;
}
#endif

Framebuffer *OpenGLContext::CreateFramebuffer(const FramebufferDesc &desc) {
	CheckGLExtensions();

#ifndef USING_GLES2
	if (!gl_extensions.ARB_framebuffer_object && gl_extensions.EXT_framebuffer_object) {
		return fbo_ext_create(desc);
	} else if (!gl_extensions.ARB_framebuffer_object) {
		return nullptr;
	}
	// If GLES2, we have basic FBO support and can just proceed.
#endif
	CHECK_GL_ERROR_IF_DEBUG();

	OpenGLFramebuffer *fbo = new OpenGLFramebuffer();
	fbo->width = desc.width;
	fbo->height = desc.height;
	fbo->colorDepth = desc.colorDepth;

	// Color texture is same everywhere
	glGenFramebuffers(1, &fbo->handle);
	glGenTextures(1, &fbo->color_texture);

	// Create the surfaces.
	glBindTexture(GL_TEXTURE_2D, fbo->color_texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	// TODO: We could opt to only create 16-bit render targets on slow devices. For later.
	switch (fbo->colorDepth) {
	case FBO_8888:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		break;
	case FBO_4444:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4, NULL);
		break;
	case FBO_5551:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fbo->width, fbo->height, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, NULL);
		break;
	case FBO_565:
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fbo->width, fbo->height, 0, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, NULL);
		break;
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	if (gl_extensions.IsGLES) {
		if (gl_extensions.OES_packed_depth_stencil) {
			ILOG("Creating %i x %i FBO using DEPTH24_STENCIL8", fbo->width, fbo->height);
			// Standard method
			fbo->stencil_buffer = 0;
			fbo->z_buffer = 0;
			// 24-bit Z, 8-bit stencil combined
			glGenRenderbuffers(1, &fbo->z_stencil_buffer);
			glBindRenderbuffer(GL_RENDERBUFFER, fbo->z_stencil_buffer);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8_OES, fbo->width, fbo->height);

			// Bind it all together
			glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo->color_texture, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo->z_stencil_buffer);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo->z_stencil_buffer);
		} else {
			ILOG("Creating %i x %i FBO using separate stencil", fbo->width, fbo->height);
			// TEGRA
			fbo->z_stencil_buffer = 0;
			// 16/24-bit Z, separate 8-bit stencil
			glGenRenderbuffers(1, &fbo->z_buffer);
			glBindRenderbuffer(GL_RENDERBUFFER, fbo->z_buffer);
			// Don't forget to make sure fbo_standard_z_depth() matches.
			glRenderbufferStorage(GL_RENDERBUFFER, gl_extensions.OES_depth24 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT16, fbo->width, fbo->height);

			// 8-bit stencil buffer
			glGenRenderbuffers(1, &fbo->stencil_buffer);
			glBindRenderbuffer(GL_RENDERBUFFER, fbo->stencil_buffer);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, fbo->width, fbo->height);

			// Bind it all together
			glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo->color_texture, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo->z_buffer);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo->stencil_buffer);
		}
	} else {
		fbo->stencil_buffer = 0;
		fbo->z_buffer = 0;
		// 24-bit Z, 8-bit stencil
		glGenRenderbuffers(1, &fbo->z_stencil_buffer);
		glBindRenderbuffer(GL_RENDERBUFFER, fbo->z_stencil_buffer);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, fbo->width, fbo->height);

		// Bind it all together
		glBindFramebuffer(GL_FRAMEBUFFER, fbo->handle);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, fbo->color_texture, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, fbo->z_stencil_buffer);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, fbo->z_stencil_buffer);
	}

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	switch (status) {
	case GL_FRAMEBUFFER_COMPLETE:
		// ILOG("Framebuffer verified complete.");
		break;
	case GL_FRAMEBUFFER_UNSUPPORTED:
		ELOG("GL_FRAMEBUFFER_UNSUPPORTED");
		break;
	case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
		ELOG("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT ");
		break;
	default:
		FLOG("Other framebuffer error: %i", status);
		break;
	}

	// Unbind state we don't need
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);
	CHECK_GL_ERROR_IF_DEBUG();

	currentDrawHandle_ = fbo->handle;
	currentReadHandle_ = fbo->handle;
	return fbo;
}

GLenum OpenGLContext::fbo_get_fb_target(bool read, GLuint **cached) {
	bool supportsBlit = gl_extensions.ARB_framebuffer_object;
	if (gl_extensions.IsGLES) {
		supportsBlit = (gl_extensions.GLES3 || gl_extensions.NV_framebuffer_blit);
	}

	// Note: GL_FRAMEBUFFER_EXT and GL_FRAMEBUFFER have the same value, same with _NV.
	if (supportsBlit) {
		if (read) {
			*cached = &currentReadHandle_;
			return GL_READ_FRAMEBUFFER;
		} else {
			*cached = &currentDrawHandle_;
			return GL_DRAW_FRAMEBUFFER;
		}
	} else {
		*cached = &currentDrawHandle_;
		return GL_FRAMEBUFFER;
	}
}

void OpenGLContext::fbo_bind_fb_target(bool read, GLuint name) {
	GLuint *cached;
	GLenum target = fbo_get_fb_target(read, &cached);
	if (*cached != name) {
		if (gl_extensions.ARB_framebuffer_object || gl_extensions.IsGLES) {
			glBindFramebuffer(target, name);
		} else {
#ifndef USING_GLES2
			glBindFramebufferEXT(target, name);
#endif
		}
		*cached = name;
	}
}

void OpenGLContext::fbo_unbind() {
#ifndef USING_GLES2
	if (gl_extensions.ARB_framebuffer_object || gl_extensions.IsGLES) {
		glBindFramebuffer(GL_FRAMEBUFFER, g_defaultFBO);
	} else if (gl_extensions.EXT_framebuffer_object) {
		glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, g_defaultFBO);
	}
#else
	glBindFramebuffer(GL_FRAMEBUFFER, g_defaultFBO);
#endif

#ifdef IOS
	bindDefaultFBO();
#endif

	currentDrawHandle_ = 0;
	currentReadHandle_ = 0;
}

void OpenGLContext::BindFramebufferAsRenderTarget(Framebuffer *fbo, const RenderPassInfo &rp) {
	OpenGLFramebuffer *fb = (OpenGLFramebuffer *)fbo;
	GLRRenderPassAction color = (GLRRenderPassAction)rp.color;
	GLRRenderPassAction depth = (GLRRenderPassAction)rp.depth;

	renderManager_.BindFramebufferAsRenderTarget(fb ? fb->framebuffer : nullptr, color, depth, rp.clearColor, rp.clearDepth, rp.clearStencil);

#if 0

	CHECK_GL_ERROR_IF_DEBUG();
	if (fbo) {
		OpenGLFramebuffer *fb = (OpenGLFramebuffer *)fbo;
		// Without FBO_ARB / GLES3, this will collide with bind_for_read, but there's nothing
		// in ES 2.0 that actually separate them anyway of course, so doesn't matter.
		fbo_bind_fb_target(false, fb->handle);
		// Always restore viewport after render target binding. Works around driver bugs.
		glstate.viewport.restore();
	} else {
		fbo_unbind();
	}
	int clearFlags = 0;
	if (rp.color == RPAction::CLEAR) {
		float fc[4]{};
		if (rp.clearColor) {
			Uint8x4ToFloat4(fc, rp.clearColor);
		}
		glClearColor(fc[0], fc[1], fc[2], fc[3]);
		clearFlags |= GL_COLOR_BUFFER_BIT;
		glstate.colorMask.force(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	}
	if (rp.depth == RPAction::CLEAR) {
#ifdef USING_GLES2
		glClearDepthf(rp.clearDepth);
#else
		glClearDepth(rp.clearDepth);
#endif
		glClearStencil(rp.clearStencil);
		clearFlags |= GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT;
		glstate.depthWrite.force(GL_TRUE);
		glstate.stencilFunc.force(GL_ALWAYS, 0, 0);
		glstate.stencilMask.force(0xFF);
	}
	if (clearFlags) {
		glstate.scissorTest.force(false);
		glClear(clearFlags);
		glstate.scissorTest.restore();
	}
	if (rp.color == RPAction::CLEAR) {
		glstate.colorMask.restore();
	}
	if (rp.depth == RPAction::CLEAR) {
		glstate.depthWrite.restore();
		glstate.stencilFunc.restore();
		glstate.stencilMask.restore();
	}
	CHECK_GL_ERROR_IF_DEBUG();
#endif
}

void OpenGLContext::CopyFramebufferImage(Framebuffer *fbsrc, int srcLevel, int srcX, int srcY, int srcZ, Framebuffer *fbdst, int dstLevel, int dstX, int dstY, int dstZ, int width, int height, int depth, int channelBits) {
	OpenGLFramebuffer *src = (OpenGLFramebuffer *)fbsrc;
	OpenGLFramebuffer *dst = (OpenGLFramebuffer *)fbdst;

	int aspect = 0;
	if (channelBits & FB_COLOR_BIT) {
		aspect |= GL_COLOR_BUFFER_BIT;
	} else if (channelBits & (FB_STENCIL_BIT | FB_DEPTH_BIT)) {
		if (channelBits & FB_STENCIL_BIT)
			aspect |= GL_STENCIL_BUFFER_BIT;
		if (channelBits & FB_DEPTH_BIT)
			aspect |= GL_DEPTH_BUFFER_BIT;
	}
	renderManager_.CopyFramebuffer(src->framebuffer, GLRect2D{ srcX, srcY, width, height }, dst->framebuffer, GLOffset2D{ dstX, dstY }, aspect);
}

bool OpenGLContext::BlitFramebuffer(Framebuffer *fbsrc, int srcX1, int srcY1, int srcX2, int srcY2, Framebuffer *fbdst, int dstX1, int dstY1, int dstX2, int dstY2, int channels, FBBlitFilter linearFilter) {
	OpenGLFramebuffer *src = (OpenGLFramebuffer *)fbsrc;
	OpenGLFramebuffer *dst = (OpenGLFramebuffer *)fbdst;
	GLuint bits = 0;
	if (channels & FB_COLOR_BIT)
		bits |= GL_COLOR_BUFFER_BIT;
	if (channels & FB_DEPTH_BIT)
		bits |= GL_DEPTH_BUFFER_BIT;
	if (channels & FB_STENCIL_BIT)
		bits |= GL_STENCIL_BUFFER_BIT;
	// Without FBO_ARB / GLES3, this will collide with bind_for_read, but there's nothing
	// in ES 2.0 that actually separate them anyway of course, so doesn't matter.
	fbo_bind_fb_target(false, dst->handle);
	fbo_bind_fb_target(true, src->handle);
	if (gl_extensions.GLES3 || gl_extensions.ARB_framebuffer_object) {
		glBlitFramebuffer(srcX1, srcY1, srcX2, srcY2, dstX1, dstY1, dstX2, dstY2, bits, linearFilter == FB_BLIT_LINEAR ? GL_LINEAR : GL_NEAREST);
		CHECK_GL_ERROR_IF_DEBUG();
#if defined(USING_GLES2) && defined(__ANDROID__)  // We only support this extension on Android, it's not even available on PC.
		return true;
	} else if (gl_extensions.NV_framebuffer_blit) {
		glBlitFramebufferNV(srcX1, srcY1, srcX2, srcY2, dstX1, dstY1, dstX2, dstY2, bits, linearFilter == FB_BLIT_LINEAR ? GL_LINEAR : GL_NEAREST);
		CHECK_GL_ERROR_IF_DEBUG();
#endif // defined(USING_GLES2) && defined(__ANDROID__)
		return true;
	} else {
		return false;
	}
}

void OpenGLContext::BindFramebufferAsTexture(Framebuffer *fbo, int binding, FBChannel channelBit, int color) {
	OpenGLFramebuffer *fb = (OpenGLFramebuffer *)fbo;

	GLuint aspect = 0;
	renderManager_.BindFramebufferAsTexture(fb->framebuffer, binding, (int)channelBit, color);
}

OpenGLFramebuffer::~OpenGLFramebuffer() {
	unregister_gl_resource_holder(this);
	CHECK_GL_ERROR_IF_DEBUG();
	if (gl_extensions.ARB_framebuffer_object || gl_extensions.IsGLES) {
		if (handle) {
			glBindFramebuffer(GL_FRAMEBUFFER, handle);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
			glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
			glBindFramebuffer(GL_FRAMEBUFFER, g_defaultFBO);
			glDeleteFramebuffers(1, &handle);
		}
		if (z_stencil_buffer)
			glDeleteRenderbuffers(1, &z_stencil_buffer);
		if (z_buffer)
			glDeleteRenderbuffers(1, &z_buffer);
		if (stencil_buffer)
			glDeleteRenderbuffers(1, &stencil_buffer);
	} else if (gl_extensions.EXT_framebuffer_object) {
#ifndef USING_GLES2
		if (handle) {
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, handle);
			glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
			glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER_EXT, 0);
			glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, g_defaultFBO);
			glDeleteFramebuffersEXT(1, &handle);
		}
		if (z_stencil_buffer)
			glDeleteRenderbuffers(1, &z_stencil_buffer);
		if (z_buffer)
			glDeleteRenderbuffers(1, &z_buffer);
		if (stencil_buffer)
			glDeleteRenderbuffers(1, &stencil_buffer);
#endif
	}

	glDeleteTextures(1, &color_texture);
}

void OpenGLContext::GetFramebufferDimensions(Framebuffer *fbo, int *w, int *h) {
	OpenGLFramebuffer *fb = (OpenGLFramebuffer *)fbo;
	if (fb) {
		*w = fb->width;
		*h = fb->height;
	} else {
		*w = targetWidth_;
		*h = targetHeight_;
	}
}

uint32_t OpenGLContext::GetDataFormatSupport(DataFormat fmt) const {
	switch (fmt) {
	case DataFormat::B8G8R8A8_UNORM:
		return FMT_RENDERTARGET | FMT_TEXTURE | FMT_AUTOGEN_MIPS;
	case DataFormat::B4G4R4A4_UNORM_PACK16:
		return FMT_RENDERTARGET | FMT_TEXTURE | FMT_AUTOGEN_MIPS;  // native support
	case DataFormat::A4R4G4B4_UNORM_PACK16:
#ifndef USING_GLES2
		// Can support this if _REV formats are supported.
		return FMT_TEXTURE;
#endif
		return 0;

	case DataFormat::R8G8B8A8_UNORM:
		return FMT_RENDERTARGET | FMT_TEXTURE | FMT_INPUTLAYOUT | FMT_AUTOGEN_MIPS;

	case DataFormat::R32_FLOAT:
	case DataFormat::R32G32_FLOAT:
	case DataFormat::R32G32B32_FLOAT:
	case DataFormat::R32G32B32A32_FLOAT:
		return FMT_INPUTLAYOUT;

	case DataFormat::R8_UNORM:
		return 0;
	case DataFormat::BC1_RGBA_UNORM_BLOCK:
	case DataFormat::BC2_UNORM_BLOCK:
	case DataFormat::BC3_UNORM_BLOCK:
		return FMT_TEXTURE;
	default:
		return 0;
	}
}

}  // namespace Draw
