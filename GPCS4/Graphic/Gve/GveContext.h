#pragma once

#include "GveCommon.h"


namespace gve
{;

class GveDevice;
class GveCommandBuffer;
class GveFrameBuffer;

// This is our render context.
// Just like GfxContext in PS4, one GveContex should be bound to one display buffer.

class GveContex : public RcObject
{
public:
	GveContex(const RcPtr<GveDevice>& device);
	~GveContex();

private:
	RcPtr<GveDevice> m_device;
	RcPtr<GveCommandBuffer> m_command;
	RcPtr<GveFrameBuffer> m_frameBuffer;
};


} // namespace gve
