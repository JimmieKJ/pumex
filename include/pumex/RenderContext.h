//
// Copyright(c) 2017-2018 Paweł Księżopolski ( pumexx )
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//

#pragma once
#include <memory>
#include <vulkan/vulkan.h>
#include <pumex/Export.h>

namespace pumex
{

class Surface;
class Queue;
class CommandPool;
class Device;
class DescriptorPool;
class FrameBuffer;
class RenderPass;
class RenderOperation;
class RenderGraphExecutable;
class PipelineLayout;
class AssetBuffer;

// class that is used extensively in Pumex : handles information about currently bound objects during data validation and command buffer creation
class PUMEX_EXPORT RenderContext
{
public:
  explicit RenderContext(Surface* surface, uint32_t queueNumber);

  inline void                setRenderGraphExecutable(std::shared_ptr<RenderGraphExecutable> rge);
  inline void                setFrameBuffer(std::shared_ptr<FrameBuffer> frameBuffer);
  inline void                setRenderPass(std::shared_ptr<RenderPass> renderPass);
  inline void                setSubpassIndex(uint32_t subpassIndex);
  void                       setRenderOperation(RenderOperation* ro);
  inline PipelineLayout*     setCurrentPipelineLayout(PipelineLayout* pipelineLayout);
  inline VkPipelineBindPoint setCurrentBindPoint(VkPipelineBindPoint bindPoint);
  inline AssetBuffer*        setCurrentAssetBuffer(AssetBuffer* assetBuffer);
  inline uint32_t            setCurrentRenderMask(uint32_t renderMask);

  // elements of the context that are constant through visitor work
  Surface*                               surface                = nullptr;
  VkSurfaceKHR                           vkSurface              = VK_NULL_HANDLE;
  std::shared_ptr<CommandPool>           commandPool;
  Queue*                                 queue                  = nullptr;
  VkQueue                                vkQueue                = VK_NULL_HANDLE;
  Device*                                device                 = nullptr;
  VkDevice                               vkDevice               = VK_NULL_HANDLE;
  DescriptorPool*                        descriptorPool         = nullptr;
  uint32_t                               activeIndex            = 0;
  uint32_t                               imageCount             = 1;

  // elements of the context that may change during visitor work
  std::shared_ptr<RenderGraphExecutable> renderGraphExecutable;
  std::shared_ptr<FrameBuffer>           frameBuffer;
  std::shared_ptr<RenderPass>            renderPass;
  uint32_t                               subpassIndex           = 0;
  RenderOperation*                       renderOperation        = nullptr;
  PipelineLayout*                        currentPipelineLayout  = nullptr; // pipeline layout
  AssetBuffer*                           currentAssetBuffer     = nullptr; // asset buffer
  uint32_t                               currentRenderMask      = 0;
  VkPipelineBindPoint                    currentBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
};

void             RenderContext::setRenderGraphExecutable(std::shared_ptr<RenderGraphExecutable> rg) { renderGraphExecutable = rg; }
void             RenderContext::setFrameBuffer(std::shared_ptr<FrameBuffer> fb)                     { frameBuffer = fb; }
void             RenderContext::setRenderPass(std::shared_ptr<RenderPass> rp)                       { renderPass = rp; }
void             RenderContext::setSubpassIndex(uint32_t si)                                        { subpassIndex = si; }


inline VkPipelineBindPoint RenderContext::setCurrentBindPoint(VkPipelineBindPoint bindPoint)
{
  VkPipelineBindPoint oldBindPoint = currentBindPoint;
  currentBindPoint = bindPoint;
  return oldBindPoint;
}

PipelineLayout* RenderContext::setCurrentPipelineLayout(PipelineLayout* pipelineLayout)
{
  PipelineLayout* oldPipelineLayout = currentPipelineLayout;
  currentPipelineLayout = pipelineLayout;
  return oldPipelineLayout;
}

AssetBuffer* RenderContext::setCurrentAssetBuffer(AssetBuffer* assetBuffer)
{
  AssetBuffer* oldAssetBuffer = currentAssetBuffer;
  currentAssetBuffer = assetBuffer;
  return oldAssetBuffer;
}

uint32_t RenderContext::setCurrentRenderMask(uint32_t renderMask)
{
  uint32_t oldRenderMask = currentRenderMask;
  currentRenderMask = renderMask;
  return oldRenderMask;
}

}
