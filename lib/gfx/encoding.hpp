#pragma once

#include "frame_packet.hpp"

namespace aurora::gfx {

struct DrawEncodeContext {
  bool hasViewport = false;
  bool hasScissor = false;
  Viewport currentViewport{};
  ClipRect currentScissor{};
};

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass);
void bind_pipeline(const wgpu::RenderPipeline& pipeline, const wgpu::RenderPassEncoder& pass);

void apply_viewport(const wgpu::RenderPassEncoder& pass, const DrawEncodeContext& ctx);
void apply_scissor(const wgpu::RenderPassEncoder& pass, const DrawEncodeContext& ctx, const wgpu::Extent3D& size);

namespace detail {
void encode_op(wgpu::CommandEncoder& encoder, FramePacket& frame, const FrameOp& op);
}

} // namespace aurora::gfx
