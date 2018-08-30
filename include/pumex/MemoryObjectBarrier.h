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
#include <memory>
#include <pumex/Export.h>
#include <pumex/MemoryObject.h>
#include <pumex/MemoryBuffer.h>
#include <pumex/MemoryImage.h>

namespace pumex
{

// MemoryObjectBarrier objects are created during workflow compilation
class PUMEX_EXPORT MemoryObjectBarrier
{
public:
  MemoryObjectBarrier();
  MemoryObjectBarrier(VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask, uint32_t srcQueueFamilyIndex, uint32_t dstQueueFamilyIndex, std::shared_ptr<MemoryObject> memoryObject, VkImageLayout oldLayout, VkImageLayout newLayout, const ImageSubresourceRange& imageRange);
  MemoryObjectBarrier(VkAccessFlags srcAccessMask, VkAccessFlags dstAccessMask, uint32_t srcQueueFamilyIndex, uint32_t dstQueueFamilyIndex, std::shared_ptr<MemoryObject> memoryObject, const BufferSubresourceRange& bufferRange);
  MemoryObjectBarrier(const MemoryObjectBarrier&) = default;
  MemoryObjectBarrier& operator=(const MemoryObjectBarrier&) = default;
  ~MemoryObjectBarrier();

  MemoryObject::Type            objectType;
  VkAccessFlags                 srcAccessMask;
  VkAccessFlags                 dstAccessMask;
  uint32_t                      srcQueueFamilyIndex;
  uint32_t                      dstQueueFamilyIndex;
  std::shared_ptr<MemoryObject> memoryObject;

  VkImageLayout                 oldLayout;   // used by images
  VkImageLayout                 newLayout;   // used by images
  ImageSubresourceRange         imageRange;  // used by images
  BufferSubresourceRange        bufferRange; // used by buffers
};

struct PUMEX_EXPORT MemoryObjectBarrierGroup
{
  MemoryObjectBarrierGroup(VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask, VkDependencyFlags dependencyFlags);

  VkPipelineStageFlags      srcStageMask;
  VkPipelineStageFlags      dstStageMask;
  VkDependencyFlags         dependencyFlags;
};

inline bool operator<(const MemoryObjectBarrierGroup& lhs, const MemoryObjectBarrierGroup& rhs)
{
  if (lhs.srcStageMask != rhs.srcStageMask)
    return lhs.srcStageMask < rhs.srcStageMask;
  if (lhs.dstStageMask != rhs.dstStageMask)
    return lhs.dstStageMask < rhs.dstStageMask;
  return lhs.dependencyFlags < rhs.dependencyFlags;
}

}
