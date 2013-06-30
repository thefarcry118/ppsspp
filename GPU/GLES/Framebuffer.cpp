// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "gfx_es2/glsl_program.h"
#include "gfx_es2/gl_state.h"
#include "gfx_es2/fbo.h"

#include "math/lin/matrix4x4.h"

#include "Core/Host.h"
#include "Core/MemMap.h"
#include "Core/Config.h"
#include "Core/System.h"
#include "GPU/ge_constants.h"
#include "GPU/GPUState.h"

#include "GPU/GLES/Framebuffer.h"
#include "GPU/GLES/TextureCache.h"
#include "GPU/GLES/ShaderManager.h"

#if defined(USING_GLES2)
#define GL_READ_FRAMEBUFFER GL_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER GL_FRAMEBUFFER
#define GL_RGBA8 GL_RGBA
#ifndef GL_DEPTH_COMPONENT24
#define GL_DEPTH_COMPONENT24 GL_DEPTH_COMPONENT24_OES
#endif
#ifndef GL_DEPTH24_STENCIL8_OES
#define GL_DEPTH24_STENCIL8_OES 0x88F0
#endif
#endif

extern int g_iNumVideos;

static const char tex_fs[] =
	"#ifdef GL_ES\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform sampler2D sampler0;\n"
	"varying vec2 v_texcoord0;\n"
	"void main() {\n"
	"	gl_FragColor = texture2D(sampler0, v_texcoord0);\n"
	"	gl_FragColor.a = 1.0;\n"
	"}\n";

static const char basic_vs[] =
#ifndef USING_GLES2
	"#version 120\n"
#endif
	"attribute vec4 a_position;\n"
	"attribute vec2 a_texcoord0;\n"
	"uniform mat4 u_viewproj;\n"
	"varying vec4 v_color;\n"
	"varying vec2 v_texcoord0;\n"
	"void main() {\n"
	"  v_texcoord0 = a_texcoord0;\n"
	"  gl_Position = u_viewproj * a_position;\n"
	"}\n";

// Aggressively delete unused FBO:s to save gpu memory.
enum {
	FBO_OLD_AGE = 5,
};

static bool MaskedEqual(u32 addr1, u32 addr2) {
	return (addr1 & 0x3FFFFFF) == (addr2 & 0x3FFFFFF);
}

void CenterRect(float *x, float *y, float *w, float *h,
                float origW, float origH, float frameW, float frameH)
{
	if (g_Config.bStretchToDisplay)
	{
		*x = 0;
		*y = 0;
		*w = frameW;
		*h = frameH;
		return;
	}

	float origRatio = origW/origH;
	float frameRatio = frameW/frameH;

	if (origRatio > frameRatio)
	{
		// Image is wider than frame. Center vertically.
		float scale = origW / frameW;
		*x = 0.0f;
		*w = frameW;
		*h = frameW / origRatio;
#ifdef BLACKBERRY
		// Stretch a little bit
		if (g_Config.bPartialStretch)
			*h = (frameH + *h) / 2.0f; // (408 + 720) / 2 = 564
#endif
		*y = (frameH - *h) / 2.0f;
	}
	else
	{
		// Image is taller than frame. Center horizontally.
		float scale = origH / frameH;
		*y = 0.0f;
		*h = frameH;
		*w = frameH * origRatio;
		*x = (frameW - *w) / 2.0f;
	}
}

FramebufferManager::FramebufferManager() :
	ramDisplayFramebufPtr_(0),
	displayFramebuf_(0),
	displayFramebufPtr_(0),
	prevDisplayFramebuf_(0),
	prevPrevDisplayFramebuf_(0),
	frameLastFramebufUsed(0),
	currentRenderVfb_(0),
	drawPixelsTex_(0),
	drawPixelsTexFormat_(-1),
	convBuf(0)
{
	draw2dprogram = glsl_create_source(basic_vs, tex_fs);

	glsl_bind(draw2dprogram);
	glUniform1i(draw2dprogram->sampler0, 0);
	glsl_unbind();

	// And an initial clear. We don't clear per frame as the games are supposed to handle that
	// by themselves.
	glstate.depthWrite.set(GL_TRUE);
	glstate.colorMask.set(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glClearColor(0,0,0,1);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

	useBufferedRendering_ = g_Config.bBufferedRendering;
}

FramebufferManager::~FramebufferManager() {
	if (drawPixelsTex_)
		glDeleteTextures(1, &drawPixelsTex_);
	glsl_destroy(draw2dprogram);
	delete [] convBuf;
}

void FramebufferManager::DrawPixels(const u8 *framebuf, int pixelFormat, int linesize) {
	if (drawPixelsTex_ && drawPixelsTexFormat_ != pixelFormat) {
		glDeleteTextures(1, &drawPixelsTex_);
		drawPixelsTex_ = 0;
	}

	if (!drawPixelsTex_) {
		glGenTextures(1, &drawPixelsTex_);

		// Initialize backbuffer texture for DrawPixels
		glBindTexture(GL_TEXTURE_2D, drawPixelsTex_);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

		switch (pixelFormat) {
		case PSP_DISPLAY_PIXEL_FORMAT_8888:
			break;
		}

		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 512, 272, 0, GL_RGBA, GL_UNSIGNED_BYTE, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		drawPixelsTexFormat_ = pixelFormat;
	}

	// TODO: We can just change the texture format and flip some bits around instead of this.
	if (pixelFormat != PSP_DISPLAY_PIXEL_FORMAT_8888 || linesize != 512) {
		if (!convBuf) {
			convBuf = new u8[512 * 272 * 4];
		}
		for (int y = 0; y < 272; y++) {
			switch (pixelFormat) {
			case PSP_DISPLAY_PIXEL_FORMAT_565:
				{
					const u16 *src = (const u16 *)framebuf + linesize * y;
					u8 *dst = convBuf + 4 * 512 * y;
					for (int x = 0; x < 480; x++)
					{
						u16 col = src[x];
						dst[x * 4] = ((col) & 0x1f) << 3;
						dst[x * 4 + 1] = ((col >> 5) & 0x3f) << 2;
						dst[x * 4 + 2] = ((col >> 11) & 0x1f) << 3;
						dst[x * 4 + 3] = 255;
					}
				}
				break;

			case PSP_DISPLAY_PIXEL_FORMAT_5551:
				{
					const u16 *src = (const u16 *)framebuf + linesize * y;
					u8 *dst = convBuf + 4 * 512 * y;
					for (int x = 0; x < 480; x++)
					{
						u16 col = src[x];
						dst[x * 4] = ((col) & 0x1f) << 3;
						dst[x * 4 + 1] = ((col >> 5) & 0x1f) << 3;
						dst[x * 4 + 2] = ((col >> 10) & 0x1f) << 3;
						dst[x * 4 + 3] = (col >> 15) ? 255 : 0;
					}
				}
				break;

			case PSP_DISPLAY_PIXEL_FORMAT_4444:
				{
					const u16 *src = (const u16 *)framebuf + linesize * y;
					u8 *dst = convBuf + 4 * 512 * y;
					for (int x = 0; x < 480; x++)
					{
						u16 col = src[x];
						dst[x * 4] = ((col >> 8) & 0xf) << 4;
						dst[x * 4 + 1] = ((col >> 4) & 0xf) << 4;
						dst[x * 4 + 2] = (col & 0xf) << 4;
						dst[x * 4 + 3] = (col >> 12) << 4;
					}
				}
				break;

			case PSP_DISPLAY_PIXEL_FORMAT_8888:
				{
					const u8 *src = framebuf + linesize * 4 * y;
					u8 *dst = convBuf + 4 * 512 * y;
					memcpy(dst, src, 4 * 480);
				}
				break;
			}
		}
	}

	glBindTexture(GL_TEXTURE_2D,drawPixelsTex_);
	if (g_Config.iTexFiltering == 3 || (g_Config.iTexFiltering == 4 && g_iNumVideos))
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	}
	glTexSubImage2D(GL_TEXTURE_2D,0,0,0,512,272, GL_RGBA, GL_UNSIGNED_BYTE, pixelFormat == PSP_DISPLAY_PIXEL_FORMAT_8888 ? framebuf : convBuf);

	float x, y, w, h;
	CenterRect(&x, &y, &w, &h, 480.0f, 272.0f, (float)PSP_CoreParameter().pixelWidth, (float)PSP_CoreParameter().pixelHeight);
	DrawActiveTexture(x, y, w, h, false, 480.0f / 512.0f);
}

void FramebufferManager::DrawActiveTexture(float x, float y, float w, float h, bool flip, float uscale) {
	float u2 = uscale;
	float v1 = flip ? 1.0f : 0.0f;
	float v2 = flip ? 0.0f : 1.0f;

	const float pos[12] = {x,y,0, x+w,y,0, x+w,y+h,0, x,y+h,0};
	const float texCoords[8] = {0, v1, u2, v1, u2, v2, 0, v2};

	glsl_bind(draw2dprogram);
	Matrix4x4 ortho;
	ortho.setOrtho(0, (float)PSP_CoreParameter().pixelWidth, (float)PSP_CoreParameter().pixelHeight, 0, -1, 1);
	glUniformMatrix4fv(draw2dprogram->u_viewproj, 1, GL_FALSE, ortho.getReadPtr());
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
	glEnableVertexAttribArray(draw2dprogram->a_position);
	glEnableVertexAttribArray(draw2dprogram->a_texcoord0);
	glVertexAttribPointer(draw2dprogram->a_position, 3, GL_FLOAT, GL_FALSE, 12, pos);
	glVertexAttribPointer(draw2dprogram->a_texcoord0, 2, GL_FLOAT, GL_FALSE, 8, texCoords);	
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);	// TODO: TRIANGLE_STRIP is more likely to be optimized.
	glDisableVertexAttribArray(draw2dprogram->a_position);
	glDisableVertexAttribArray(draw2dprogram->a_texcoord0);
	glsl_unbind();
}

VirtualFramebuffer *FramebufferManager::GetDisplayFBO() {
	VirtualFramebuffer *match = NULL;
	for (size_t i = 0; i < vfbs_.size(); ++i) {
		VirtualFramebuffer *v = vfbs_[i];
		if (MaskedEqual(v->fb_address, displayFramebufPtr_) && v->format == displayFormat_ && v->width >= 480) {
			// Could check w too but whatever
			if (match == NULL || match->last_frame_used < v->last_frame_used) {
				match = v;
			}
		}
	}
	if (match != NULL) 
		return match;	

	DEBUG_LOG(HLE, "Finding no FBO matching address %08x", displayFramebufPtr_);
#if 0  // defined(_DEBUG)
	std::string debug = "FBOs: ";
	for (size_t i = 0; i < vfbs_.size(); ++i) {
		char temp[256];
		sprintf(temp, "%08x %i %i", vfbs_[i]->fb_address, vfbs_[i]->width, vfbs_[i]->height);
		debug += std::string(temp);
	}
	ERROR_LOG(HLE, "FBOs: %s", debug.c_str());
#endif
	return 0;
}

void GetViewportDimensions(int &w, int &h) {
	float vpXa = getFloat24(gstate.viewportx1);
	float vpYa = getFloat24(gstate.viewporty1);
	w = (int)fabsf(vpXa * 2);
	h = (int)fabsf(vpYa * 2);
}

// Heuristics to figure out the size of FBO to create.
void GuessDrawingSize(int &drawing_width, int &drawing_height) {
	GetViewportDimensions(drawing_width, drawing_height);

	// HACK for first frame where some games don't init things right
	if (drawing_width <= 1 && drawing_height <= 1) {
		drawing_width = 480;
		drawing_height = 272;
	}

	// Now, cap using scissor. Hm, no, this doesn't work so well.
	/*
	if (drawing_width > gstate.getScissorX2() + 1)
		drawing_width = gstate.getScissorX2() + 1;
	if (drawing_height > gstate.getScissorY2() + 1)
		drawing_height = gstate.getScissorY2() + 1;*/
	
	// Cap at maximum texture size for now. Don't see much point in drawing bigger.
	drawing_width = std::min(drawing_width, 512);
	drawing_height = std::min(drawing_height, 512);
}

void FramebufferManager::DestroyFramebuf(VirtualFramebuffer *v) {
	textureCache_->NotifyFramebufferDestroyed(v->fb_address, v);
	if (v->fbo) {
		fbo_destroy(v->fbo);
		v->fbo = 0;
	}

	// Wipe some pointers
	if (currentRenderVfb_ == v)
		currentRenderVfb_ = 0;
	if (displayFramebuf_ == v)
		displayFramebuf_ = 0;
	if (prevDisplayFramebuf_ == v)
		prevDisplayFramebuf_ = 0;
	if (prevPrevDisplayFramebuf_ == v)
		prevPrevDisplayFramebuf_ = 0;

	delete v;
}

void FramebufferManager::SetRenderFrameBuffer() {
	if (!gstate_c.framebufChanged && currentRenderVfb_) {
		currentRenderVfb_->last_frame_used = gpuStats.numFrames;
		return;
	}
	gstate_c.framebufChanged = false;

	// Get parameters
	u32 fb_address = (gstate.fbptr & 0xFFE000) | ((gstate.fbwidth & 0xFF0000) << 8);
	int fb_stride = gstate.fbwidth & 0x3C0;

	u32 z_address = (gstate.zbptr & 0xFFE000) | ((gstate.zbwidth & 0xFF0000) << 8);
	int z_stride = gstate.zbwidth & 0x3C0;

	// Yeah this is not completely right. but it'll do for now.
	//int drawing_width = ((gstate.region2) & 0x3FF) + 1;
	//int drawing_height = ((gstate.region2 >> 10) & 0x3FF) + 1;

	// As there are no clear "framebuffer width" and "framebuffer height" registers,
	// we need to infer the size of the current framebuffer somehow. Let's try the viewport.
	
	int fmt = gstate.framebufpixformat & 3;

	int drawing_width, drawing_height;
	GuessDrawingSize(drawing_width, drawing_height);

	int buffer_width = drawing_width;
	int buffer_height = drawing_height;

	// Find a matching framebuffer, same size or bigger
	VirtualFramebuffer *vfb = 0;
	for (size_t i = 0; i < vfbs_.size(); ++i) {
		VirtualFramebuffer *v = vfbs_[i];
		if (MaskedEqual(v->fb_address, fb_address) && v->format == fmt) {
			// Okay, let's check the sizes. If the new one is bigger than the old one, recreate.
			// If the opposite, just use it and hope that the game sets scissors accordingly.
			if (v->bufferWidth >= drawing_width && v->bufferHeight >= drawing_height) {
				// Let's not be so picky for now. Let's say this is the one.
				vfb = v;
				// Update fb stride in case it changed
				vfb->fb_stride = fb_stride;
				// Just hack the width/height and we should be fine. also hack renderwidth/renderheight?
				v->width = drawing_width;
				v->height = drawing_height;
				break;
			} else {
				INFO_LOG(HLE, "Embiggening framebuffer (%i, %i) -> (%i, %i)", (int)v->width, (int)v->height, drawing_width, drawing_height);
				// drawing_width or drawing_height is bigger. Let's recreate with the max.
				// To do this right we should copy the data over too, but meh.
				buffer_width = std::max((int)v->width, drawing_width);
				buffer_height = std::max((int)v->height, drawing_height);

				DestroyFramebuf(v);
				vfbs_.erase(vfbs_.begin() + i--);
				break;
			}
		}
	}

	float renderWidthFactor = (float)PSP_CoreParameter().renderWidth / 480.0f;
	float renderHeightFactor = (float)PSP_CoreParameter().renderHeight / 272.0f;

	// Save current render framebuffer to memory
	if(currentRenderVfb_) {
		if(g_Config.bFramebuffersToMem) {
			ReadFramebufferToMemory(currentRenderVfb_);
		}
	}

	// None found? Create one.
	if (!vfb) {
		gstate_c.textureChanged = true;
		vfb = new VirtualFramebuffer();
		vfb->fbo = 0;
		vfb->fb_address = fb_address;
		vfb->fb_stride = fb_stride;
		vfb->z_address = z_address;
		vfb->z_stride = z_stride;
		vfb->width = drawing_width;
		vfb->height = drawing_height;
		vfb->renderWidth = (u16)(drawing_width * renderWidthFactor);
		vfb->renderHeight = (u16)(drawing_height * renderHeightFactor);
		vfb->bufferWidth = buffer_width;
		vfb->bufferHeight = buffer_height;
		vfb->format = fmt;
		vfb->usageFlags = FB_USAGE_RENDERTARGET;
		vfb->dirtyAfterDisplay = true;

		if (g_Config.bTrueColor) {
			vfb->colorDepth = FBO_8888;
		} else { 
			switch (fmt) {
				case GE_FORMAT_4444: 
					vfb->colorDepth = FBO_4444; 
					break;
				case GE_FORMAT_5551: 
					vfb->colorDepth = FBO_5551; 
					break;
				case GE_FORMAT_565: 
					vfb->colorDepth = FBO_565; 
					break;
				case GE_FORMAT_8888: 
					vfb->colorDepth = FBO_8888; 
					break;
				default: 
					vfb->colorDepth = FBO_8888; 
					break;
			}
		}
			
		//#ifdef ANDROID
		//	vfb->colorDepth = FBO_8888;
		//#endif

		if (useBufferedRendering_) {
			vfb->fbo = fbo_create(vfb->renderWidth, vfb->renderHeight, 1, true, vfb->colorDepth);
			if (vfb->fbo) {
				fbo_bind_as_render_target(vfb->fbo);
			} else {
				ERROR_LOG(HLE, "Error creating FBO! %i x %i", vfb->renderWidth, vfb->renderHeight);
			}
		} else {
			fbo_unbind();
			// Let's ignore rendering to targets that have not (yet) been displayed.
			gstate_c.skipDrawReason |= SKIPDRAW_NON_DISPLAYED_FB;
		}

		textureCache_->NotifyFramebuffer(vfb->fb_address, vfb);

		vfb->last_frame_used = gpuStats.numFrames;
		frameLastFramebufUsed = gpuStats.numFrames;
		vfbs_.push_back(vfb);
		glstate.depthWrite.set(GL_TRUE);
		glstate.colorMask.set(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(0,0,0,1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		glEnable(GL_DITHER);
		currentRenderVfb_ = vfb;

		INFO_LOG(HLE, "Creating FBO for %08x : %i x %i x %i", vfb->fb_address, vfb->width, vfb->height, vfb->format);

	// We already have it!
	} else if (vfb != currentRenderVfb_) {
		// Use it as a render target.
		DEBUG_LOG(HLE, "Switching render target to FBO for %08x: %i x %i x %i ", vfb->fb_address, vfb->width, vfb->height, vfb->format);
		vfb->usageFlags |= FB_USAGE_RENDERTARGET;
		gstate_c.textureChanged = true;
		vfb->last_frame_used = gpuStats.numFrames;
		frameLastFramebufUsed = gpuStats.numFrames;
		vfb->dirtyAfterDisplay = true;

		if (useBufferedRendering_) {
			if (vfb->fbo) {
				fbo_bind_as_render_target(vfb->fbo);
			} else {
				// wtf? This should only happen very briefly when toggling bBufferedRendering
				fbo_unbind();
			}
		} else {
			if (vfb->fbo) {
				// wtf? This should only happen very briefly when toggling bBufferedRendering
				textureCache_->NotifyFramebufferDestroyed(vfb->fb_address, vfb);
				fbo_destroy(vfb->fbo);
				vfb->fbo = 0;
			}
			fbo_unbind();

			// Let's ignore rendering to targets that have not (yet) been displayed.
			if (vfb->usageFlags & FB_USAGE_DISPLAYED_FRAMEBUFFER)
				gstate_c.skipDrawReason &= ~SKIPDRAW_NON_DISPLAYED_FB;
			else
				gstate_c.skipDrawReason |= SKIPDRAW_NON_DISPLAYED_FB;

			/*
			if (drawing_width == 480 && drawing_height == 272) {
				gstate_c.skipDrawReason &= ~SKIPDRAW_SKIPNONFB;
				// OK!
			} else {
				gstate_c.skipDrawReason |= ~SKIPDRAW_SKIPNONFB;
			}*/
		}
		textureCache_->NotifyFramebuffer(vfb->fb_address, vfb);

#ifdef USING_GLES2
		// Some tiled mobile GPUs benefit IMMENSELY from clearing an FBO before rendering
		// to it. This broke stuff before, so now it only clears on the first use of an
		// FBO in a frame. This means that some games won't be able to avoid the on-some-GPUs
		// performance-crushing framebuffer reloads from RAM, but we'll have to live with that.
		if (vfb->last_frame_used != gpuStats.numFrames)	{
			glstate.depthWrite.set(GL_TRUE);
			glstate.colorMask.set(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glClearColor(0,0,0,1);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		}
#endif
		currentRenderVfb_ = vfb;
	} else {
		vfb->last_frame_used = gpuStats.numFrames;
		frameLastFramebufUsed = gpuStats.numFrames;
	}

	// ugly...
	if (gstate_c.curRTWidth != vfb->width || gstate_c.curRTHeight != vfb->height) {
		shaderManager_->DirtyUniform(DIRTY_PROJTHROUGHMATRIX);
		gstate_c.curRTWidth = vfb->width;
		gstate_c.curRTHeight = vfb->height;
	}
}

void FramebufferManager::CopyDisplayToOutput() {
	fbo_unbind();
	currentRenderVfb_ = 0;

	VirtualFramebuffer *vfb = GetDisplayFBO();
	if (!vfb) {
		if (Memory::IsValidAddress(ramDisplayFramebufPtr_)) {
			// The game is displaying something directly from RAM. In GTA, it's decoded video.
			DrawPixels(Memory::GetPointer(ramDisplayFramebufPtr_), displayFormat_, displayStride_);
		} else if (Memory::IsValidAddress(displayFramebufPtr_)) {
			// The game is displaying something directly from RAM. In GTA, it's decoded video.
			DrawPixels(Memory::GetPointer(displayFramebufPtr_), displayFormat_, displayStride_);
		} else {
			DEBUG_LOG(HLE, "Found no FBO to display! displayFBPtr = %08x", displayFramebufPtr_);
			// No framebuffer to display! Clear to black.
			glstate.depthWrite.set(GL_TRUE);
			glstate.colorMask.set(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			glClearColor(0,0,0,1);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
		}
		return;
	}

	vfb->usageFlags |= FB_USAGE_DISPLAYED_FRAMEBUFFER;
	vfb->dirtyAfterDisplay = false;

	if (prevDisplayFramebuf_ != displayFramebuf_) {
		prevPrevDisplayFramebuf_ = prevDisplayFramebuf_;
	}
	if (displayFramebuf_ != vfb) {
		prevDisplayFramebuf_ = displayFramebuf_;
	}
	displayFramebuf_ = vfb;

	if (vfb->fbo) {
		glstate.viewport.set(0, 0, PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight);
		DEBUG_LOG(HLE, "Displaying FBO %08x", vfb->fb_address);
		glstate.blend.disable();
		glstate.cullFace.disable();
		glstate.depthTest.disable();
		glstate.scissorTest.disable();
		glstate.stencilTest.disable();

		fbo_bind_color_as_texture(vfb->fbo, 0);
	
	// These are in the output display coordinates
		float x, y, w, h;
		CenterRect(&x, &y, &w, &h, 480.0f, 272.0f, (float)PSP_CoreParameter().pixelWidth, (float)PSP_CoreParameter().pixelHeight);
		DrawActiveTexture(x, y, w, h, true);
		glBindTexture(GL_TEXTURE_2D, 0);
	}

	if(g_Config.bFramebuffersToMem) {
		ReadFramebufferToMemory(vfb);
	}

	if (resized_) {
		glstate.depthWrite.set(GL_TRUE);
		glstate.colorMask.set(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(0,0,0,1);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	}
}

void FramebufferManager::ReadFramebufferToMemory(VirtualFramebuffer *vfb) {
	// This only works with buffered rendering
	if (!useBufferedRendering_) {
		return;
	}

	fbo_unbind();
	if(gl_extensions.FBO_ARB) { // TODO: fbo_unbind should use GL_FRAMEBUFFER to do this? Don't want to change native
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
	}
	if(vfb) {
		float renderWidthFactor = (float)PSP_CoreParameter().renderWidth / 480.0f;
		float renderHeightFactor = (float)PSP_CoreParameter().renderHeight / 272.0f;

		// If render resolution different we blit a new framebuffer and copy that one to memory
		// (assuming rendering a smaller resolution than the PSP isn't done on any device, but not sure on that)
		// A more accurate (and probably costly) solution would be to draw a framebuffer at native resolution 
		// parallel to the rendering one?
		if(renderWidthFactor > 1.0f || renderHeightFactor > 1.0f) {
			// For now we'll also keep these framebuffer objects on the same struct as the ones that can get displayed
			// (and blatantly copy work already done above while at it)
			VirtualFramebuffer *nvfb = 0;

			// We maintain a separate vector of framebuffer objects for blitting (guessing the point is saving on FBOs?)
			for (size_t i = 0; i < bvfbs_.size(); ++i) {
				VirtualFramebuffer *v = bvfbs_[i];
				if (MaskedEqual(v->fb_address, vfb->fb_address) && v->format == vfb->format) {
					if (v->bufferWidth == vfb->bufferWidth && v->bufferHeight == vfb->bufferHeight) {
						nvfb = v;
						v->fb_stride = vfb->fb_stride;
						v->width = vfb->width;
						v->height = vfb->height;
						break;
					}
				}
			}

			// Create a new fbo if none was found for the size
			if(!nvfb) {
				nvfb = new VirtualFramebuffer();
				nvfb->fbo = 0;
				nvfb->fb_address = vfb->fb_address;
				nvfb->fb_stride = vfb->fb_stride;
				nvfb->z_address = vfb->z_address;
				nvfb->z_stride = vfb->z_stride;
				nvfb->width = vfb->width;
				nvfb->height = vfb->height;
				nvfb->renderWidth = vfb->width;
				nvfb->renderHeight = vfb->height;
				nvfb->bufferWidth = vfb->bufferWidth;
				nvfb->bufferHeight = vfb->bufferHeight;
				nvfb->format = vfb->format;
				nvfb->usageFlags = FB_USAGE_RENDERTARGET;
				nvfb->dirtyAfterDisplay = true;

				if (g_Config.bTrueColor) {
					nvfb->colorDepth = FBO_8888;
				} else { 
					switch (vfb->format) {
						case GE_FORMAT_4444: 
							nvfb->colorDepth = FBO_4444; 
							break;
						case GE_FORMAT_5551: 
							nvfb->colorDepth = FBO_5551; 
							break;
						case GE_FORMAT_565: 
							nvfb->colorDepth = FBO_565; 
							break;
						case GE_FORMAT_8888: 
							nvfb->colorDepth = FBO_8888; 
							break;
						default: 
							nvfb->colorDepth = FBO_8888; 
							break;
					}
				}
			
				//#ifdef ANDROID
				//	nvfb->colorDepth = FBO_8888;
				//#endif

				nvfb->fbo = fbo_create(nvfb->width, nvfb->height, 1, true, nvfb->colorDepth);
				if (useBufferedRendering_) {
					if (nvfb->fbo) {
						fbo_bind_as_render_target(nvfb->fbo);
					} else {
						ERROR_LOG(HLE, "Error creating FBO! %i x %i", vfb->renderWidth, vfb->renderHeight);
					}
				}

				nvfb->last_frame_used = gpuStats.numFrames;
				bvfbs_.push_back(nvfb);
			} else {
				// We already have one, so we set it as a render target.
				//DEBUG_LOG(HLE, "Switching render target to FBO for %08x: %i x %i x %i ", nvfb->fb_address, nvfb->width, nvfb->height, nvfb->format);
				nvfb->usageFlags |= FB_USAGE_RENDERTARGET;
				nvfb->last_frame_used = gpuStats.numFrames;
				nvfb->dirtyAfterDisplay = true;

				if (useBufferedRendering_) {
					if (nvfb->fbo) {
						fbo_bind_as_render_target(nvfb->fbo);
					} else {
						fbo_unbind();
						if(gl_extensions.FBO_ARB) {
							glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
						}
					}
				}
#ifdef USING_GLES2
				// Some tiled mobile GPUs benefit IMMENSELY from clearing an FBO before rendering
				// to it. This broke stuff before, so now it only clears on the first use of an
				// FBO in a frame. This means that some games won't be able to avoid the on-some-GPUs
				// performance-crushing framebuffer reloads from RAM, but we'll have to live with that.
				if (nvfb->last_frame_used != gpuStats.numFrames)	{
					glstate.depthWrite.set(GL_TRUE);
					glstate.colorMask.set(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
					glClearColor(0,0,0,1);
					glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
				}
#endif
			}

			// We bind the resized fbo for reading
			if (useBufferedRendering_) {
				if (vfb->fbo) {
					fbo_bind_for_read(vfb->fbo);
				} else {
					fbo_unbind();
					if(gl_extensions.FBO_ARB) {
						glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
					}
				}
			}

			// And we check both framebuffers for completeness
			if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE 
				|| glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
					DEBUG_LOG(HLE, "Incomplete FBOs pre-blitting");
					fbo_unbind();
					if(gl_extensions.FBO_ARB) {
						glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
					}
					return;
			}

			// TODO: glReadBuffer and glDrawBuffer should maybe be specifically set here?

			// Then we blit the color buffer using linear filtering
#ifndef USING_GLES2
			DEBUG_LOG(HLE, "Blitting FBOs for %08x: %i x %i to %i x %i ", nvfb->fb_address, vfb->renderWidth, vfb->renderHeight, nvfb->renderWidth, nvfb->renderHeight);
			glBlitFramebuffer(0, 0, vfb->fb_stride * renderWidthFactor, vfb->renderHeight, 0, 0, nvfb->fb_stride, nvfb->height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
#else
			WARN_LOG(HLE, "Skipping FBO blit, not supported on GLES 2.0. Needs replacing with a real draw (that can also flip y)");
#endif
			fbo_unbind();
			if(gl_extensions.FBO_ARB) {
				glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			}
			vfb = nvfb;
		}

		int pixelType, pixelSize, pixelFormat, align;

		
		switch (vfb->format) {
			case GE_FORMAT_4444: // 16 bit ABGR
#ifdef USING_GLES2
				pixelType = GL_UNSIGNED_SHORT_4_4_4_4;
#else
				pixelType = GL_UNSIGNED_SHORT_4_4_4_4_REV;
#endif
				pixelFormat = GL_RGBA;
				pixelSize = 2;
				align = 8;
				break;
			case GE_FORMAT_5551: // 16 bit ABGR
#ifdef USING_GLES2
				pixelType = GL_UNSIGNED_SHORT_5_5_5_1;
#else
				pixelType = GL_UNSIGNED_SHORT_1_5_5_5_REV;
#endif
				pixelFormat = GL_RGBA;
				pixelSize = 2;
				align = 8;
				break;
			case GE_FORMAT_565: // 16 bit BGR
#ifdef USING_GLES2
				pixelType = GL_UNSIGNED_SHORT_5_6_5;
#else
				pixelType = GL_UNSIGNED_SHORT_5_6_5_REV;
#endif
				pixelFormat = GL_RGB;
				pixelSize = 2;
				align = 8;
				break;
			case GE_FORMAT_8888: // 32 bit ABGR
			default: // And same as above
#ifdef USING_GLES2
				pixelType = GL_UNSIGNED_BYTE;
#else
				pixelType = GL_UNSIGNED_INT_8_8_8_8_REV;
#endif
				pixelFormat = GL_RGBA;
				pixelSize = 4;
				align = 4;
				break;
		}

		if (useBufferedRendering_) {
			if (vfb->fbo) {
				fbo_bind_for_read(vfb->fbo);
			} else {
				fbo_unbind();
				if(gl_extensions.FBO_ARB) {
					glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
				}
				return;
			}
		}

		// Prepare buffers to read the pixel data into
		// Ideally we'd apply the image flip inplace and only need one, 
		// but right now this is simpler 
		// (maybe it's more efficient to have the GPU flip it in the framebuffer and then flip it back?)
		int bufHeight = vfb->height;
		size_t bufSize = vfb->fb_stride * bufHeight;
		GLubyte *flipBuf = (GLubyte *) malloc(bufSize * pixelSize);

		u32 fb_address = (0x04000000) | vfb->fb_address;

		DEBUG_LOG(HLE, "Reading pixels to mem, bufSize = %u, fb_address = %08x", bufSize, fb_address);
		glPixelStorei(GL_PACK_ALIGNMENT, align);
		glReadPixels(0, 0, vfb->fb_stride, vfb->height, pixelFormat, pixelType, flipBuf);
		
		// We have to flip glReadPixels data upside down
		int u8_stride = vfb->fb_stride * pixelSize;
		for (int y = 0; y < bufHeight; y++) {
			int inverted_y = bufHeight - 1 - y;
			Memory::Memcpy(fb_address + inverted_y * u8_stride, &flipBuf[u8_stride * y], u8_stride);
		}

		free(flipBuf);
		fbo_unbind();
		if(gl_extensions.FBO_ARB) {
			glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		}
	}
}

void FramebufferManager::EndFrame() {
	if (resized_) {
		DestroyAllFBOs();
		glstate.viewport.set(0, 0, PSP_CoreParameter().pixelWidth, PSP_CoreParameter().pixelHeight);
		resized_ = false;
	}
}

void FramebufferManager::DeviceLost() {
	DestroyAllFBOs();
	resized_ = false;
}

void FramebufferManager::BeginFrame() {
	DecimateFBOs();
	// NOTE - this is all wrong. At the beginning of the frame is a TERRIBLE time to draw the fb.
	if (g_Config.bDisplayFramebuffer && displayFramebufPtr_) {
		INFO_LOG(HLE, "Drawing the framebuffer (%08x)", displayFramebufPtr_);
		const u8 *pspframebuf = Memory::GetPointer((0x44000000) | (displayFramebufPtr_ & 0x1FFFFF));	// TODO - check
		glstate.cullFace.disable();
		glstate.depthTest.disable();
		glstate.blend.disable();
		glstate.scissorTest.disable();
		glstate.stencilTest.disable();
		DrawPixels(pspframebuf, displayFormat_, displayStride_);
		// TODO: restore state?
	}
	currentRenderVfb_ = 0;
	useBufferedRendering_ = g_Config.bBufferedRendering;
}

void FramebufferManager::SetDisplayFramebuffer(u32 framebuf, u32 stride, int format) {

	if ((framebuf & 0x04000000) == 0) {
		DEBUG_LOG(HLE, "Non-VRAM display framebuffer address set: %08x", framebuf);
		ramDisplayFramebufPtr_ = framebuf;
		displayStride_ = stride;
		displayFormat_ = format;
	} else {
		ramDisplayFramebufPtr_ = 0;
		displayFramebufPtr_ = framebuf;
		displayStride_ = stride;
		displayFormat_ = format;
	}
}

std::vector<FramebufferInfo> FramebufferManager::GetFramebufferList() {
	std::vector<FramebufferInfo> list;

	for (size_t i = 0; i < vfbs_.size(); ++i) {
		VirtualFramebuffer *vfb = vfbs_[i];

		FramebufferInfo info;
		info.fb_address = vfb->fb_address;
		info.z_address = vfb->z_address;
		info.format = vfb->format;
		info.width = vfb->width;
		info.height = vfb->height;
		info.fbo = vfb->fbo;
		list.push_back(info);
	}

	return list;
}

void FramebufferManager::DecimateFBOs() {
	fbo_unbind();
	currentRenderVfb_ = 0;
	for (size_t i = 0; i < vfbs_.size(); ++i) {
		VirtualFramebuffer *vfb = vfbs_[i];
		if (vfb == displayFramebuf_ || vfb == prevDisplayFramebuf_ || vfb == prevPrevDisplayFramebuf_) {
			continue;
		}
		int age = frameLastFramebufUsed - vfb->last_frame_used;
		if (age > FBO_OLD_AGE) {
			INFO_LOG(HLE, "Decimating FBO for %08x (%i x %i x %i), age %i", vfb->fb_address, vfb->width, vfb->height, vfb->format, age)
			DestroyFramebuf(vfb);
			vfbs_.erase(vfbs_.begin() + i--);
		}
	}
}

void FramebufferManager::DestroyAllFBOs() {
	fbo_unbind();
	currentRenderVfb_ = 0;
	displayFramebuf_ = 0;
	prevDisplayFramebuf_ = 0;
	prevPrevDisplayFramebuf_ = 0;

	for (size_t i = 0; i < vfbs_.size(); ++i) {
		VirtualFramebuffer *vfb = vfbs_[i];
		INFO_LOG(HLE, "Destroying FBO for %08x : %i x %i x %i", vfb->fb_address, vfb->width, vfb->height, vfb->format);
		DestroyFramebuf(vfb);
	}
	vfbs_.clear();
}

void FramebufferManager::UpdateFromMemory(u32 addr, int size) {
	addr &= ~0x40000000;

	// TODO: Could go through all FBOs, but probably not important?
	// TODO: Could also check for inner changes, but video is most important.
	if (addr == DisplayFramebufAddr() || addr == PrevDisplayFramebufAddr()) {
		// TODO: Deleting the FBO is a heavy hammer solution, so let's only do it if it'd help.
		if (!Memory::IsValidAddress(displayFramebufPtr_))
			return;

		fbo_unbind();
		currentRenderVfb_ = 0;

		bool needUnbind = false;
		for (size_t i = 0; i < vfbs_.size(); ++i) {
			VirtualFramebuffer *vfb = vfbs_[i];
			if (MaskedEqual(vfb->fb_address, addr)) {
				// TODO: This without the fbo_unbind() above would be better than destroying the FBO.
				// However, it doesn't seem to work for Star Ocean, at least
				if (g_Config.bBufferedRendering) {
					fbo_bind_as_render_target(vfb->fbo);
					needUnbind = true;
					DrawPixels(Memory::GetPointer(addr), vfb->format, vfb->fb_stride);
				} else {
					INFO_LOG(HLE, "Invalidating FBO for %08x (%i x %i x %i)", vfb->fb_address, vfb->width, vfb->height, vfb->format)
					DestroyFramebuf(vfb);
					vfbs_.erase(vfbs_.begin() + i--);
				}
			}
		}

		if (needUnbind)
			fbo_unbind();
	}
}

void FramebufferManager::Resized() {
	resized_ = true;
}
