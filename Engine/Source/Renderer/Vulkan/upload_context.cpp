#include "Engine/Renderer/Vulkan/upload_context.h"
#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <utility>
namespace Engine {
UploadContext* UploadContext::current_ = nullptr;
UploadContext::~UploadContext() { destroy(); }
UploadContext::Batch::Batch(UploadContext& context) : context_(&context) { context_->begin(); }
UploadContext::Batch::~Batch() { if (context_ != nullptr) context_->abort(); }
UploadContext::Batch::Batch(Batch&& other) noexcept : context_(std::exchange(other.context_, nullptr)) {}
UploadContext::Batch& UploadContext::Batch::operator=(Batch&& other) noexcept { if (this != &other) { if (context_ != nullptr) context_->abort(); context_ = std::exchange(other.context_, nullptr); } return *this; }
UploadTicket UploadContext::Batch::submit() {
    if (context_ == nullptr) return {};
    const UploadTicket ticket = context_->submit();
    context_ = nullptr;
    return ticket;
}
void UploadContext::setCurrent(UploadContext* context) noexcept { current_ = context; }
UploadContext* UploadContext::current() noexcept { return current_; }
void UploadContext::create(VkDevice device, VkQueue queue, uint32_t family, VmaAllocator allocator, VkDeviceSize bytes) {
    device_=device; queue_=queue; allocator_=allocator; capacity_=bytes;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size=bytes; info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo alloc{}; alloc.usage=VMA_MEMORY_USAGE_AUTO_PREFER_HOST; alloc.flags=VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT|VMA_ALLOCATION_CREATE_MAPPED_BIT;
    if(vmaCreateBuffer(allocator_, &info, &alloc, &staging_, &allocation_, nullptr)!=VK_SUCCESS) throw std::runtime_error("Could not create upload staging ring");
    VmaAllocationInfo details{}; vmaGetAllocationInfo(allocator_, allocation_, &details); mapped_=details.pMappedData;
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool.flags=VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; pool.queueFamilyIndex=family;
    if(vkCreateCommandPool(device_, &pool, nullptr, &pool_)!=VK_SUCCESS) throw std::runtime_error("Could not create upload command pool");
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO}; type.semaphoreType=VK_SEMAPHORE_TYPE_TIMELINE; type.initialValue=0;
    VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; semaphore.pNext=&type;
    if(vkCreateSemaphore(device_, &semaphore, nullptr, &timeline_)!=VK_SUCCESS) throw std::runtime_error("Could not create upload timeline semaphore");
}
void UploadContext::reclaim() noexcept { if(!device_) return; const auto done=completedValue(); std::erase_if(submitted_, [=](const Submitted& s){ if(s.value>done) return false; vkFreeCommandBuffers(device_,pool_,1,&s.commandBuffer); return true; }); if(submitted_.empty()) head_=0; }
void UploadContext::abort() noexcept {
    if (commandBuffer_ == VK_NULL_HANDLE) return;

    const VkCommandBuffer commandBuffer = std::exchange(commandBuffer_, VK_NULL_HANDLE);
    const bool wasRecording = std::exchange(recording_, false);
    // A failed vkEndCommandBuffer or vkQueueSubmit2 leaves this buffer in a
    // non-recording state. Do not end it a second time during RAII cleanup.
    if (wasRecording) {
        static_cast<void>(vkEndCommandBuffer(commandBuffer));
    }
    vkFreeCommandBuffers(device_, pool_, 1, &commandBuffer);
}
uint64_t UploadContext::completedValue() const noexcept { uint64_t value=0; return timeline_ && vkGetSemaphoreCounterValue(device_,timeline_,&value)==VK_SUCCESS ? value : 0; }
UploadContext::Batch UploadContext::beginBatch() { return Batch{*this}; }
void UploadContext::begin() {
    if (commandBuffer_ != VK_NULL_HANDLE) {
        throw std::logic_error("UploadContext command buffer is still active");
    }
    reclaim();
    VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = pool_;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &info, &commandBuffer_) != VK_SUCCESS) {
        throw std::runtime_error("Could not allocate upload command buffer");
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(commandBuffer_, &begin) != VK_SUCCESS) {
        vkFreeCommandBuffers(device_, pool_, 1, &commandBuffer_);
        commandBuffer_ = VK_NULL_HANDLE;
        throw std::runtime_error("Could not begin upload command buffer");
    }
    recording_ = true;
}
UploadContext::Slice UploadContext::allocate(VkDeviceSize size,VkDeviceSize alignment) { if(!recording_ || size>capacity_) throw std::runtime_error("Invalid upload-ring allocation"); auto offset=(head_+alignment-1)&~(alignment-1); if(offset+size>capacity_){ const UploadTicket ticket=submit(); VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO}; wait.semaphoreCount=1; wait.pSemaphores=&timeline_; wait.pValues=&ticket.timelineValue; vkWaitSemaphores(device_,&wait,UINT64_MAX); reclaim(); begin(); offset=0; } head_=offset+size; return {staging_,offset,static_cast<char*>(mapped_)+offset}; }
void UploadContext::copyBuffer(VkBuffer dst,const void* data,VkDeviceSize size,VkDeviceSize dstOffset) { auto slice=allocate(size); std::memcpy(slice.mapped,data,static_cast<size_t>(size)); VkBufferCopy copy{slice.offset,dstOffset,size}; vkCmdCopyBuffer(commandBuffer_,staging_,dst,1,&copy); }
UploadTicket UploadContext::pendingTicket() const noexcept { return recording_ ? UploadTicket{nextValue_} : UploadTicket{}; }
UploadTicket UploadContext::submit() {
    if (commandBuffer_ == VK_NULL_HANDLE) return {};
    if (!recording_) throw std::logic_error("Upload command buffer is not recording");

    recording_ = false;
    if (vkEndCommandBuffer(commandBuffer_) != VK_SUCCESS) {
        throw std::runtime_error("Could not end upload command buffer");
    }
    const uint64_t value = nextValue_++;
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer=commandBuffer_; VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; signal.semaphore=timeline_; signal.value=value; signal.stageMask=VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT; VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount=1; submit.pCommandBufferInfos=&command; submit.signalSemaphoreInfoCount=1; submit.pSignalSemaphoreInfos=&signal;
    if(vkQueueSubmit2(queue_,1,&submit,VK_NULL_HANDLE)!=VK_SUCCESS) throw std::runtime_error("Could not submit upload batch");
    submitted_.push_back({commandBuffer_,value}); commandBuffer_=VK_NULL_HANDLE; return {value};
}
void UploadContext::destroy() noexcept { if(!device_) return; if(commandBuffer_) { abort(); } if(timeline_ && nextValue_>1){ VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO}; const uint64_t v=nextValue_-1; wait.semaphoreCount=1; wait.pSemaphores=&timeline_; wait.pValues=&v; vkWaitSemaphores(device_,&wait,UINT64_MAX); } for(auto& s:submitted_) vkFreeCommandBuffers(device_,pool_,1,&s.commandBuffer); submitted_.clear(); if(timeline_) vkDestroySemaphore(device_,timeline_,nullptr); if(pool_) vkDestroyCommandPool(device_,pool_,nullptr); if(staging_) vmaDestroyBuffer(allocator_,staging_,allocation_); if(current_==this) current_=nullptr; device_=VK_NULL_HANDLE; queue_=VK_NULL_HANDLE; allocator_=VK_NULL_HANDLE; staging_=VK_NULL_HANDLE; allocation_=VK_NULL_HANDLE; mapped_=nullptr; capacity_=head_=0; pool_=VK_NULL_HANDLE; timeline_=VK_NULL_HANDLE; nextValue_=1; recording_=false; }
}
