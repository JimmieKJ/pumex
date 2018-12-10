//
// Copyright(c) 2017-2018 Pawe� Ksi�opolski ( pumexx )
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

#include <pumex/MemoryImage.h>
#include <pumex/Surface.h>
#include <pumex/Device.h>
#include <pumex/Command.h>
#include <pumex/RenderContext.h>
#include <pumex/Resource.h>
#include <pumex/utils/Buffer.h>
#include <pumex/utils/Log.h>
#include <algorithm>

using namespace pumex;

struct SetImageTraitsOperation : public MemoryImage::Operation
{
  SetImageTraitsOperation(MemoryImage* o, const ImageTraits& t, VkImageAspectFlags am, uint32_t ac)
    : MemoryImage::Operation(o, MemoryImage::Operation::SetImageTraits, ImageSubresourceRange(am, 0, t.imageSize.mipLevels, 0, t.imageSize.arrayLayers), ac), imageTraits{ t }
  {}
  bool perform(const RenderContext& renderContext, MemoryImage::MemoryImageInternal& internals, std::shared_ptr<CommandBuffer> commandBuffer) override
  {
    internals.image = nullptr; // release image before creating a new one
    internals.image = std::make_shared<Image>(renderContext.device, imageTraits, owner->getAllocator());
    owner->notifyCommandBufferSources(renderContext);
    owner->notifyImageViews(renderContext, imageRange);
    // no operations sent to command buffer
    return false;
  }
  ImageTraits imageTraits;
};

struct SetImageOperation : public MemoryImage::Operation
{
  SetImageOperation(MemoryImage* o, const ImageSubresourceRange& r, const ImageSubresourceRange& sr, std::shared_ptr<gli::texture> tex, uint32_t ac)
    : MemoryImage::Operation(o, MemoryImage::Operation::SetImage, r, ac), sourceRange{ sr }, texture{ tex }
  {}
  bool perform(const RenderContext& renderContext, MemoryImage::MemoryImageInternal& internals, std::shared_ptr<CommandBuffer> commandBuffer) override
  {
    CHECK_LOG_THROW(internals.image == nullptr, "Image was not created before call to setImage operation, which should not happen because this call is made automatically during setImage() setup...");
    gli::texture::extent_type extent = texture->extent();
    const ImageTraits& imageTraits   = internals.image->getImageTraits();
    VkExtent3D currExtent            = makeVkExtent3D(imageTraits.imageSize);
    CHECK_LOG_THROW((extent.x != currExtent.width) || (extent.y != currExtent.height) || (extent.z != currExtent.depth), "MemoryImage has wrong size : ( " << extent.x << " x " << extent.y << " x " << extent.z << " ) should be ( " << currExtent.width << " x " << currExtent.height << " x " << currExtent.depth << " )");

    auto ownerAllocator = owner->getAllocator();
    bool memoryIsLocal  = ((ownerAllocator->getMemoryPropertyFlags() & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    auto aspectMask     = imageRange.aspectMask;

    if (memoryIsLocal)
    {
      // copy texture data to staging buffer manually
      auto stagingBuffer = renderContext.device->acquireStagingBuffer(nullptr, texture->size());
      unsigned char* mapAddress = (unsigned char*)stagingBuffer->mapMemory(texture->size());
      size_t offset = 0;
      for (uint32_t layer = sourceRange.baseArrayLayer; layer < sourceRange.baseArrayLayer + sourceRange.layerCount; ++layer)
      {
        for (uint32_t level = sourceRange.baseMipLevel; level < sourceRange.baseMipLevel + sourceRange.levelCount; ++level)
        {
          std::memcpy(mapAddress + offset, texture->data(layer, 0, level), texture->size(level));
          offset += texture->size(level);
        }
      }
      stagingBuffer->unmapMemory();

      // we have to copy a texture to local device memory using staging buffers
      std::vector<VkBufferImageCopy> bufferCopyRegions;
      offset = 0;
      for (uint32_t layer = imageRange.baseArrayLayer ; layer < imageRange.baseArrayLayer + imageRange.layerCount; ++layer)
      {
        for (uint32_t level = imageRange.baseMipLevel; level < imageRange.baseMipLevel + imageRange.levelCount; ++level)
        {
          auto mipMapExtents = texture->extent(level);
          VkBufferImageCopy bufferCopyRegion{};
            bufferCopyRegion.imageSubresource.aspectMask     = aspectMask;
            bufferCopyRegion.imageSubresource.mipLevel       = level;
            bufferCopyRegion.imageSubresource.baseArrayLayer = layer;
            bufferCopyRegion.imageSubresource.layerCount     = 1;
            bufferCopyRegion.imageExtent.width               = static_cast<uint32_t>(mipMapExtents.x);
            bufferCopyRegion.imageExtent.height              = static_cast<uint32_t>(mipMapExtents.y);
            bufferCopyRegion.imageExtent.depth               = static_cast<uint32_t>(mipMapExtents.z);
            bufferCopyRegion.bufferOffset                    = offset;
          bufferCopyRegions.push_back(bufferCopyRegion);

          // Increase offset into staging buffer for next level / face
          offset += texture->size(level);
        }
      }
      // Image barrier for optimal image (target)
      // Optimal image will be used as destination for the copy
      commandBuffer->setImageLayout( *(internals.image), aspectMask, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

      // Copy mip levels from staging buffer
      commandBuffer->cmdCopyBufferToImage(stagingBuffer->buffer, *(internals.image), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, bufferCopyRegions);

      // Change texture image layout to shader read after all mip levels have been copied
      commandBuffer->setImageLayout( *(internals.image), aspectMask, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

      stagingBuffers.push_back(stagingBuffer);
    }
    else
    {
      // BTW : this only works for images created with linear tiling
      // we have to copy image to host visible memory - no staging buffers, no commands for commandBuffer
      unsigned char* data = (unsigned char*)internals.image->mapMemory(0, internals.image->getMemorySize(), 0);
      for (uint32_t layer = sourceRange.baseArrayLayer, targetLayer = imageRange.baseArrayLayer; layer < sourceRange.baseArrayLayer + sourceRange.layerCount; ++layer, ++targetLayer)
      {
        for (uint32_t level = sourceRange.baseMipLevel, targetLevel = imageRange.baseMipLevel; level < sourceRange.baseMipLevel + sourceRange.levelCount; ++level, ++targetLevel)
        {
          VkImageSubresource subRes{};
          subRes.aspectMask = imageRange.aspectMask;
          subRes.arrayLayer = targetLayer;
          subRes.mipLevel   = targetLevel;

          VkSubresourceLayout subResLayout;
          internals.image->getImageSubresourceLayout(subRes, subResLayout);
          std::memcpy(data + subResLayout.offset, texture->data(layer, 0, level), texture->size(level));
        }
      }
      internals.image->unmapMemory();

      // Setup image memory barrier
      commandBuffer->setImageLayout(*(internals.image), aspectMask, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    }

    // if memory is accessible from host ( is not local ) - we generated no commands to command buffer
    return memoryIsLocal;
  }
  void releaseResources(const RenderContext& renderContext) override
  {
    for (auto& s : stagingBuffers)
      renderContext.device->releaseStagingBuffer(s);
    stagingBuffers.clear();
  }

  std::shared_ptr<gli::texture>               texture;
  ImageSubresourceRange                       sourceRange;
  std::vector<std::shared_ptr<StagingBuffer>> stagingBuffers;
};

struct NotifyImageViewsOperation : public MemoryImage::Operation
{
  NotifyImageViewsOperation(MemoryImage* o, const ImageSubresourceRange& r, uint32_t ac)
    : MemoryImage::Operation(o, MemoryImage::Operation::NotifyImageViews, r, ac)
  {}
  bool perform(const RenderContext& renderContext, MemoryImage::MemoryImageInternal& internals, std::shared_ptr<CommandBuffer> commandBuffer) override
  {
    owner->notifyCommandBufferSources(renderContext);
    owner->notifyImageViews(renderContext, imageRange);
    // no operations sent to command buffer
    return false;
  }
};

struct ClearImageOperation : public MemoryImage::Operation
{
  ClearImageOperation(MemoryImage* o, const ImageSubresourceRange& r, VkClearValue cv, uint32_t ac)
    : MemoryImage::Operation(o, MemoryImage::Operation::ClearImage, r, ac), clearValue{ cv }
  {}
  bool perform(const RenderContext& renderContext, MemoryImage::MemoryImageInternal& internals, std::shared_ptr<CommandBuffer> commandBuffer) override
  {
    std::vector<VkImageSubresourceRange> subResources;
    subResources.push_back(imageRange.getSubresource());

    commandBuffer->setImageLayout(*internals.image, imageRange.aspectMask, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, subResources[0]);
    if (imageRange.aspectMask | VK_IMAGE_ASPECT_COLOR_BIT)
      commandBuffer->cmdClearColorImage(*internals.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, clearValue, subResources);
    else
      commandBuffer->cmdClearDepthStencilImage(*internals.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, clearValue, subResources);
    commandBuffer->setImageLayout(*(internals.image), imageRange.aspectMask, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    return true;
  }
  VkClearValue clearValue;
};

MemoryImage::MemoryImage(const ImageTraits& it, std::shared_ptr<DeviceMemoryAllocator> a, VkImageAspectFlags am, PerObjectBehaviour pob, SwapChainImageBehaviour scib, bool stpo, bool useSetImageMethods)
  : MemoryObject(MemoryObject::moImage), perObjectBehaviour{ pob }, swapChainImageBehaviour{ scib }, sameTraitsPerObject{ stpo }, imageTraits{ it }, allocator { a }, aspectMask{ am }, activeCount{ 1 }
{
  if(useSetImageMethods)
    imageTraits.usage = imageTraits.usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}

MemoryImage::MemoryImage(std::shared_ptr<gli::texture> tex, std::shared_ptr<DeviceMemoryAllocator> a, VkImageAspectFlags am, VkImageUsageFlags iu, PerObjectBehaviour pob)
  : MemoryObject(MemoryObject::moImage), perObjectBehaviour{ pob }, swapChainImageBehaviour{ swOnce }, sameTraitsPerObject{ true }, allocator{ a }, aspectMask{ am }, activeCount{ 1 }
{
  // for now we will only use textures that have base_level==0 and base_layer==0
  CHECK_LOG_THROW(tex == nullptr, "Cannot create MemoryImage object without data");
  CHECK_LOG_THROW(tex->base_level() != 0, "Cannot create MemoryImage object when base_level != 0");
  CHECK_LOG_THROW(tex->base_layer() != 0, "Cannot create MemoryImage object when base_layer != 0");

  texture     = tex;
  imageTraits = getImageTraitsFromTexture(*texture, iu);
  // flag VK_IMAGE_USAGE_TRANSFER_DST_BIT because user wants to send gli::texture to GPU memory
  imageTraits.usage = imageTraits.usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
}

MemoryImage::~MemoryImage()
{
  std::lock_guard<std::mutex> lock(mutex);
  perObjectData.clear();
}

MemoryImage* MemoryImage::asMemoryImage()
{
  return this;
}

void MemoryImage::setImageTraits(const ImageTraits& traits)
{
  CHECK_LOG_THROW(!sameTraitsPerObject, "Cannot set image traits for all objects - MemoryImage uses different traits per each surface");
  CHECK_LOG_THROW(texture != nullptr, "Cannot set image traits - there's a gli::texture that prevents it");

  std::lock_guard<std::mutex> lock(mutex);
  imageTraits = traits;
  for (auto& pdd : perObjectData)
  {
    // remove all previous calls to setImageTraits
    pdd.second.commonData.imageOperations.remove_if([](std::shared_ptr<Operation> texop) { return texop->type == MemoryImage::Operation::SetImageTraits; });
    // add setImageTraits operation
    pdd.second.commonData.imageOperations.push_back(std::make_shared<SetImageTraitsOperation>(this, traits, aspectMask, activeCount));
    pdd.second.invalidate();
  }
  invalidateImageViews();
}

void MemoryImage::setImageTraits(Surface* surface, const ImageTraits& traits)
{
  CHECK_LOG_THROW(perObjectBehaviour != pbPerSurface, "Cannot set image traits per surface for this texture");
  CHECK_LOG_THROW(sameTraitsPerObject, "Cannot set traits per surface - Texture uses the same traits per each surface");
  std::lock_guard<std::mutex> lock(mutex);
  internalSetImageTraits(surface->getID(), surface->device.lock()->device, surface->surface, traits, aspectMask);
}

void MemoryImage::setImageTraits(Device* device, const ImageTraits& traits)
{
  CHECK_LOG_THROW(perObjectBehaviour != pbPerDevice, "Cannot set image traits per device for this texture");
  CHECK_LOG_THROW(sameTraitsPerObject, "Cannot set traits per device - texture uses the same traits per each device");
  std::lock_guard<std::mutex> lock(mutex);
  internalSetImageTraits(device->getID(), device->device, VK_NULL_HANDLE, traits, aspectMask);
}

void MemoryImage::invalidateImage()
{
  CHECK_LOG_THROW(texture == nullptr, "Cannot invalidate texture - wrong constructor used to create an object");
  std::lock_guard<std::mutex> lock(mutex);
  ImageSubresourceRange range(aspectMask, texture->base_level(), texture->levels(), texture->base_layer(), texture->layers());
  for (auto& pdd : perObjectData)
  {
    // remove all previous calls to setImage
    pdd.second.commonData.imageOperations.remove_if([](std::shared_ptr<Operation> texop) { return texop->type == MemoryImage::Operation::SetImage; });
    // add setImage operation with full texture size
    pdd.second.commonData.imageOperations.push_back(std::make_shared<SetImageOperation>(this, range, range, texture, activeCount));
    pdd.second.invalidate();
  }
  invalidateImageViews();
}

void MemoryImage::setImage(Surface* surface, std::shared_ptr<gli::texture> tex)
{
  CHECK_LOG_THROW(perObjectBehaviour != pbPerSurface, "Cannot set image per surface for this texture");
  CHECK_LOG_THROW((imageTraits.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0, "Cannot set image for this texture - user declared it as not writeable");
  std::lock_guard<std::mutex> lock(mutex);
  internalSetImage(surface->getID(), surface->device.lock()->device, surface->surface, tex);
}

void MemoryImage::setImage(Device* device, std::shared_ptr<gli::texture> tex)
{
  CHECK_LOG_THROW(perObjectBehaviour != pbPerDevice, "Cannot set image per device for this texture");
  CHECK_LOG_THROW((imageTraits.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0, "Cannot set image for this texture - user declared it as not writeable");
  std::lock_guard<std::mutex> lock(mutex);
  internalSetImage(device->getID(), device->device, VK_NULL_HANDLE, tex);
}

void MemoryImage::setImageLayer(uint32_t layer, std::shared_ptr<gli::texture> tex)
{
  CHECK_LOG_THROW(texture == nullptr, "Cannot set texture layer - wrong constructor used to create an object");
  CHECK_LOG_THROW(!sameTraitsPerObject, "Cannot set texture layer when each device/surface may use different traits");
  CHECK_LOG_THROW((layer >= texture->layers()), "Layer out of bounds : " << layer << " should be between 0 and " << texture->layers() - 1);
  CHECK_LOG_THROW(tex->format() != texture->format(), "Input texture has wrong format : " << tex->format() << " should be " << texture->format());
  CHECK_LOG_THROW(tex->layers() > 1, "Cannot call setTextureLayer() with texture that has more than one layer");
  CHECK_LOG_THROW(tex->base_level() != texture->base_level(), "Cannot set image layer when there are different base mip levels");
  CHECK_LOG_THROW(tex->levels() != texture->levels(), "Cannot set image layer when there is different count of mip levels");
  gli::texture::extent_type extent = tex->extent();
  gli::texture::extent_type myExtent = texture->extent();
  CHECK_LOG_THROW((extent.x != myExtent.x) || (extent.y != myExtent.y) , "Texture has wrong size : ( " << extent.x << " x " << extent.y << " ) should be ( " << myExtent.x << " x " << myExtent.y << " )");

  // place the data in a texture, so that texture on CPU side is in sync with texture on GPU side
  std::lock_guard<std::mutex> lock(mutex);
  if (texture != nullptr)
  {
    for (uint32_t level = texture->base_level(); level < texture->levels(); ++level)
      std::memcpy(texture->data(layer, 0, level), tex->data(0, 0, level), tex->size(level));
  }

  ImageSubresourceRange targetRange(aspectMask, texture->base_level(), texture->levels(), layer, 1);
  ImageSubresourceRange sourceRange(aspectMask, tex->base_level(), tex->levels(), 0, 1);

  for (auto& pdd : perObjectData)
  {
    // remove all previous calls for setting the image
    pdd.second.commonData.imageOperations.remove_if([&targetRange](std::shared_ptr<Operation> texop) { return texop->type == MemoryImage::Operation::SetImage && targetRange.contains(texop->imageRange); });
    // add setImage operation
    pdd.second.commonData.imageOperations.push_back(std::make_shared<SetImageOperation>(this, targetRange, sourceRange, tex, activeCount));
    pdd.second.invalidate();
  }
  invalidateImageViews();
}

void MemoryImage::setImages(Surface* surface, std::vector<std::shared_ptr<Image>>& images)
{
  CHECK_LOG_THROW(perObjectBehaviour != pbPerSurface, "Cannot set foreign images per surface for this texture");
  CHECK_LOG_THROW(texture != nullptr, "Cannot set foreign images - wrong constructor used to create an object");
  CHECK_LOG_THROW(sameTraitsPerObject, "Cannot set foreign images when each device/surface must use the same traits");
  std::lock_guard<std::mutex> lock(mutex);
  internalSetImages(surface->getID(), surface->device.lock()->device, surface->surface, images);
}

void MemoryImage::setImages(Device* device, std::vector<std::shared_ptr<Image>>& images)
{
  CHECK_LOG_THROW(perObjectBehaviour != pbPerDevice, "Cannot set foreign images per device for this texture");
  CHECK_LOG_THROW(texture != nullptr, "Cannot set foreign images - wrong constructor used to create an object");
  CHECK_LOG_THROW(sameTraitsPerObject, "Cannot set foreign images when each device/surface must use the same traits");
  std::lock_guard<std::mutex> lock(mutex);
  internalSetImages(device->getID(), device->device, VK_NULL_HANDLE, images);
}

void MemoryImage::clearImages(const glm::vec4& clearValue, const ImageSubresourceRange& range)
{
  // build clear value depending on MemoryImage aspectMask
  VkClearValue cv = makeClearValue(clearValue, aspectMask);
  // override aspectMask delivered by user with aspectMask defined in MemoryImage object
  ImageSubresourceRange realRange(aspectMask, range.baseMipLevel, range.levelCount, range.baseArrayLayer, range.layerCount);

  std::lock_guard<std::mutex> lock(mutex);
  for (auto& pdd : perObjectData)
  {
    // remove all previous calls for image clearing that are contained by this call
    pdd.second.commonData.imageOperations.remove_if ([&realRange](std::shared_ptr<Operation> texop) { return texop->type == MemoryImage::Operation::ClearImage && realRange.contains(texop->imageRange); });
    // add clear operation
    pdd.second.commonData.imageOperations.push_back( std::make_shared<ClearImageOperation>( this, realRange, cv, activeCount ) );
    pdd.second.invalidate();
  }
}

void MemoryImage::clearImage(Surface* surface, const glm::vec4& clearValue, const ImageSubresourceRange& range)
{
  CHECK_LOG_THROW(perObjectBehaviour != pbPerSurface, "Cannot clear image per surface for this texture");
  std::lock_guard<std::mutex> lock(mutex);
  internalClearImage(surface->getID(), surface->device.lock()->device, surface->surface, clearValue, range);
}

void MemoryImage::clearImage(Device* device, const glm::vec4& clearValue, const ImageSubresourceRange& range)
{
  CHECK_LOG_THROW(perObjectBehaviour != pbPerDevice, "Cannot clear image per device for this texture");
  std::lock_guard<std::mutex> lock(mutex);
  internalClearImage(device->getID(), device->device, VK_NULL_HANDLE, clearValue, range);
}

Image* MemoryImage::getImage(const RenderContext& renderContext) const
{
  std::lock_guard<std::mutex> lock(mutex);
  auto pddit = perObjectData.find(getKeyID(renderContext, perObjectBehaviour));
  if (pddit == end(perObjectData))
    return nullptr;
  return pddit->second.data[renderContext.activeIndex % activeCount].image.get();
}

void MemoryImage::validate(const RenderContext& renderContext)
{
  std::lock_guard<std::mutex> lock(mutex);
  if (swapChainImageBehaviour == swForEachImage && renderContext.imageCount > activeCount)
  {
    activeCount = renderContext.imageCount;
    for (auto& pdd : perObjectData)
    {
      pdd.second.resize(activeCount);
      for (auto& op : pdd.second.commonData.imageOperations)
        op->resize(activeCount);
    }
  }
  auto keyValue = getKeyID(renderContext, perObjectBehaviour);
  auto pddit = perObjectData.find(keyValue);
  if (pddit == end(perObjectData))
    pddit = perObjectData.insert({ keyValue, MemoryImageData(renderContext, swapChainImageBehaviour) }).first;
  uint32_t activeIndex = renderContext.activeIndex % activeCount;
  if (pddit->second.valid[activeIndex])
    return;

  // methods working per device may add PerObjectData without defining surface handle - we have to fill that gap
  if (pddit->second.surface == VK_NULL_HANDLE)
    pddit->second.surface = renderContext.vkSurface;
  // also methods working per surface may add PerObjectData before device realization ( so the device == VK_NULL_HANDLE )
  if (pddit->second.device == VK_NULL_HANDLE)
    pddit->second.device = renderContext.vkDevice;

  // images are created here, when MemoryImage uses sameTraitsPerObject - otherwise it's a reponsibility of the user to create them through setImageTraits() call
  if (pddit->second.data[activeIndex].image == nullptr && sameTraitsPerObject)
  {
    pddit->second.data[activeIndex].image = std::make_shared<Image>(renderContext.device, imageTraits, allocator);
    notifyCommandBufferSources(renderContext);
    notifyImageViews(renderContext, ImageSubresourceRange(aspectMask, 0, imageTraits.imageSize.mipLevels, 0, imageTraits.imageSize.arrayLayers));
    // if there's a texture - it must be sent now
    if (texture != nullptr)
      internalSetImage(keyValue, renderContext.vkDevice, renderContext.vkSurface, texture);
  }
  // if there are some pending texture operations
  if (!pddit->second.commonData.imageOperations.empty())
  {
    // perform all operations in a single command buffer
    auto cmdBuffer = renderContext.device->beginSingleTimeCommands(renderContext.commandPool);
    bool submit = false;
    for (auto& texop : pddit->second.commonData.imageOperations)
    {
      if (!texop->updated[activeIndex])
      {
        submit |= texop->perform(renderContext, pddit->second.data[activeIndex], cmdBuffer);
        // mark operation as done for this activeIndex
        texop->updated[activeIndex] = true;
      }
    }
    renderContext.device->endSingleTimeCommands(cmdBuffer, renderContext.vkQueue, submit);
    for (auto& texop : pddit->second.commonData.imageOperations)
      texop->releaseResources(renderContext);
    // if all operations are done for each index - remove them from list
    pddit->second.commonData.imageOperations.remove_if(([](std::shared_ptr<Operation> texop) { return texop->allUpdated(); }));
  }
  pddit->second.valid[activeIndex] = true;
}

ImageSubresourceRange MemoryImage::getFullImageRange()
{
  return ImageSubresourceRange(aspectMask, 0, imageTraits.imageSize.mipLevels, 0, imageTraits.imageSize.arrayLayers);
}

void MemoryImage::addCommandBufferSource(std::shared_ptr<CommandBufferSource> cbSource)
{
  if (std::find_if(begin(commandBufferSources), end(commandBufferSources), [&cbSource](std::weak_ptr<CommandBufferSource> cbs) { return !cbs.expired() && cbs.lock().get() == cbSource.get(); }) == end(commandBufferSources))
    commandBufferSources.push_back(cbSource);
}

void MemoryImage::notifyCommandBufferSources(const RenderContext& renderContext)
{
  auto eit = std::remove_if(begin(commandBufferSources), end(commandBufferSources), [](std::weak_ptr<CommandBufferSource> r) { return r.expired();  });
  for (auto it = begin(commandBufferSources); it != eit; ++it)
    it->lock()->notifyCommandBuffers(renderContext.activeIndex);
  commandBufferSources.erase(eit, end(commandBufferSources));
}

void MemoryImage::addImageView(std::shared_ptr<ImageView> imageView)
{
  if (std::find_if(begin(imageViews), end(imageViews), [&imageView](std::weak_ptr<ImageView> iv) { return !iv.expired() && iv.lock().get() == imageView.get(); }) == end(imageViews))
    imageViews.push_back(imageView);
}

void MemoryImage::notifyImageViews(const RenderContext& renderContext, const ImageSubresourceRange& range)
{
  auto eit = std::remove_if(begin(imageViews), end(imageViews), [](std::weak_ptr<ImageView> iv) { return iv.expired();  });
  for (auto it = begin(imageViews); it != eit; ++it)
    if( range.contains(it->lock()->subresourceRange) )
      it->lock()->notify(renderContext);
  imageViews.erase(eit, end(imageViews));
}

void MemoryImage::invalidateImageViews()
{
  auto eit = std::remove_if(begin(imageViews), end(imageViews), [](std::weak_ptr<ImageView> iv) { return iv.expired();  });
  for (auto it = begin(imageViews); it != eit; ++it)
    it->lock()->invalidateResources();
  imageViews.erase(eit, end(imageViews));
}

// caution : mutex lock must be called prior to this method
void MemoryImage::internalSetImageTraits(uint32_t key, VkDevice device, VkSurfaceKHR surface, const ImageTraits& traits, VkImageAspectFlags aMask)
{
  auto pddit = perObjectData.find(key);
  if (pddit == end(perObjectData))
    pddit = perObjectData.insert({ key, MemoryImageData(device, surface, activeCount, swapChainImageBehaviour) }).first;

  // remove all previous calls to setImageTraits
  pddit->second.commonData.imageOperations.remove_if([](std::shared_ptr<Operation> texop) { return texop->type == MemoryImage::Operation::SetImageTraits; });
  // add setImageTraits operation
  pddit->second.commonData.imageOperations.push_back(std::make_shared<SetImageTraitsOperation>(this, traits, aMask, activeCount));
  pddit->second.invalidate();
  invalidateImageViews();
}

// caution : mutex lock must be called prior to this method
void MemoryImage::internalSetImage(uint32_t key, VkDevice device, VkSurfaceKHR surface, std::shared_ptr<gli::texture> tex)
{
  auto pddit = perObjectData.find(key);
  if (pddit == end(perObjectData))
  {
    pddit = perObjectData.insert({ key, MemoryImageData(device, surface, activeCount, swapChainImageBehaviour) }).first;
    // image does not exist at that moment - we should add imageTraits
    // image usage is always taken from main imageTraits
    auto traits = getImageTraitsFromTexture(*tex, imageTraits.usage);
    internalSetImageTraits(key, device, surface, traits, aspectMask);
  }

  ImageSubresourceRange range(aspectMask, tex->base_level(), tex->levels(), tex->base_layer(), tex->layers());
  // remove all previous calls to setImage, but only when these calls are a subset of current call
  pddit->second.commonData.imageOperations.remove_if([&range](std::shared_ptr<Operation> texop) { return texop->type == MemoryImage::Operation::SetImage && range.contains(texop->imageRange); });
  // add setImage operation
  pddit->second.commonData.imageOperations.push_back(std::make_shared<SetImageOperation>(this, range, range, tex, activeCount));
  pddit->second.invalidate();
  invalidateImageViews();
}

// set foreign images as images used by texture
// caution : mutex lock must be called prior to this method
void MemoryImage::internalSetImages(uint32_t key, VkDevice device, VkSurfaceKHR surface, std::vector<std::shared_ptr<Image>>& images)
{
  for (uint32_t i = 0; i < images.size(); i++)
  {
    CHECK_LOG_THROW(images[i]->getDevice() != device, "Cannot set foreign images for this texture - mismatched devices");
  }

  if (swapChainImageBehaviour == swForEachImage && images.size() > activeCount)
  {
    activeCount = images.size();
    for (auto& pdd : perObjectData)
    {
      pdd.second.resize(activeCount);
      for (auto& op : pdd.second.commonData.imageOperations)
        op->resize(activeCount);
    }
  }
  auto pddit = perObjectData.find(key);
  if (pddit == end(perObjectData))
    pddit = perObjectData.insert({ key, MemoryImageData(device, surface, activeCount, swapChainImageBehaviour) }).first;
  for (uint32_t i = 0; i < images.size(); i++)
  {
    pddit->second.data[i].image = nullptr;
    pddit->second.data[i].image = images[i];
  }
  pddit->second.commonData.imageOperations.clear();
  ImageSubresourceRange range(aspectMask, 0, images[0]->getImageTraits().imageSize.mipLevels, 0, images[0]->getImageTraits().imageSize.arrayLayers);
  pddit->second.commonData.imageOperations.push_back(std::make_shared<NotifyImageViewsOperation>(this, range, activeCount));
  pddit->second.invalidate();
  invalidateImageViews();
}

// build clear value depending on Texture aspectMask
// caution : mutex lock must be called prior to this method
void MemoryImage::internalClearImage(uint32_t key, VkDevice device, VkSurfaceKHR surface, const glm::vec4& clearValue, const ImageSubresourceRange& range)
{
  VkClearValue cv = makeClearValue(clearValue, aspectMask);
  // override aspectMask delivered by user with aspectMask defined in Texture object
  ImageSubresourceRange realRange(aspectMask, range.baseMipLevel, range.levelCount, range.baseArrayLayer, range.layerCount);

  auto pddit = perObjectData.find(key);
  if (pddit == end(perObjectData))
    pddit = perObjectData.insert({ key, MemoryImageData(device, surface, activeCount, swapChainImageBehaviour) }).first;

  // remove all previous calls for image clearing
  pddit->second.commonData.imageOperations.remove_if([](std::shared_ptr<Operation> texop) { return texop->type == MemoryImage::Operation::ClearImage; });
  // add clear operation
  pddit->second.commonData.imageOperations.push_back(std::make_shared<ClearImageOperation>(this, realRange, cv, activeCount));
  pddit->second.invalidate();
}

ImageView::ImageView(std::shared_ptr<MemoryImage> mi, const ImageSubresourceRange& sr, VkImageViewType vt, VkFormat f, const gli::swizzles& sw)
  : std::enable_shared_from_this<ImageView>(), memoryImage{ mi }, subresourceRange{ sr }, viewType{ vt }, swizzles{ sw }, activeCount{ 1 }
{
  format = (f == VK_FORMAT_UNDEFINED) ? memoryImage->getImageTraits().format : f;
}

ImageView::~ImageView()
{
  std::lock_guard<std::mutex> lock(mutex);
  for( auto& pdd : perObjectData )
    for( uint32_t i=0; i<pdd.second.data.size(); ++i)
      vkDestroyImageView(pdd.second.device, pdd.second.data[i].imageView, nullptr);
}

VkImage ImageView::getHandleImage(const RenderContext& renderContext) const
{
  return memoryImage->getImage(renderContext)->getHandleImage();
}

VkImageView ImageView::getImageView(const RenderContext& renderContext) const
{
  std::lock_guard<std::mutex> lock(mutex);
  auto keyValue = getKeyID(renderContext, memoryImage->getPerObjectBehaviour());
  auto pddit = perObjectData.find(keyValue);
  if (pddit == end(perObjectData))
    return VK_NULL_HANDLE;
  uint32_t activeIndex = renderContext.activeIndex % activeCount;
  return pddit->second.data[activeIndex].imageView;
}

void ImageView::validate(const RenderContext& renderContext)
{
  if (!registered)
  {
    memoryImage->addImageView(shared_from_this());
    registered = true;
  }
  memoryImage->validate(renderContext);
  std::lock_guard<std::mutex> lock(mutex);
  if (memoryImage->getSwapChainImageBehaviour() == swForEachImage && renderContext.imageCount > activeCount)
  {
    activeCount = renderContext.imageCount;
    for (auto& pdd : perObjectData)
      pdd.second.resize(activeCount);
  }
  auto keyValue = getKeyID(renderContext, memoryImage->getPerObjectBehaviour());
  auto pddit = perObjectData.find(keyValue);
  if (pddit == end(perObjectData))
    pddit = perObjectData.insert({ keyValue, ImageViewData(renderContext, memoryImage->getSwapChainImageBehaviour()) }).first;
  uint32_t activeIndex = renderContext.activeIndex % activeCount;
  if (pddit->second.valid[activeIndex])
    return;

  if (pddit->second.data[activeIndex].imageView != VK_NULL_HANDLE)
  {
    vkDestroyImageView(pddit->second.device, pddit->second.data[activeIndex].imageView, nullptr);
    pddit->second.data[activeIndex].imageView = VK_NULL_HANDLE;
  }

  VkImageViewCreateInfo imageViewCI{};
    imageViewCI.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewCI.flags            = 0;
    imageViewCI.image            = getHandleImage(renderContext);
    imageViewCI.viewType         = viewType;
    imageViewCI.format           = format;
    imageViewCI.components       = vulkanComponentMappingFromGliComponentMapping(swizzles);
    imageViewCI.subresourceRange = subresourceRange.getSubresource();
  VK_CHECK_LOG_THROW(vkCreateImageView(pddit->second.device, &imageViewCI, nullptr, &pddit->second.data[activeIndex].imageView), "failed vkCreateImageView");

  notifyResources(renderContext);
  pddit->second.valid[activeIndex] = true;
}

void ImageView::notify(const RenderContext& renderContext)
{
  std::lock_guard<std::mutex> lock(mutex);
  auto keyValue = getKeyID(renderContext, memoryImage->getPerObjectBehaviour());
  auto pddit = perObjectData.find(keyValue);
  if (pddit == end(perObjectData))
    pddit = perObjectData.insert({ keyValue, ImageViewData(renderContext, memoryImage->getSwapChainImageBehaviour()) }).first;
  pddit->second.invalidate();
}

void ImageView::addResource(std::shared_ptr<Resource> resource)
{
  if (std::find_if(begin(resources), end(resources), [&resource](std::weak_ptr<Resource> r) { return !r.expired() && r.lock().get() == resource.get(); }) == end(resources))
    resources.push_back(resource);
}

void ImageView::notifyResources(const RenderContext& renderContext)
{
  auto eit = std::remove_if(begin(resources), end(resources), [](std::weak_ptr<Resource> r) { return r.expired();  });
  for (auto it = begin(resources); it != eit; ++it)
    it->lock()->notifyDescriptors(renderContext);
  resources.erase(eit, end(resources));
}

void ImageView::invalidateResources()
{
  auto eit = std::remove_if(begin(resources), end(resources), [](std::weak_ptr<Resource> r) { return r.expired();  });
  for (auto it = begin(resources); it != eit; ++it)
    it->lock()->invalidateDescriptors();
  resources.erase(eit, end(resources));
}
