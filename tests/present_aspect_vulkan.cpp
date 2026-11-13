// Synthetic transfer/readback fixture. test_present_aspect_vulkan.py inserts the
// production helpers/present block at the markers below; no game content is used.
#include <vulkan/vulkan.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#include "vk_present_rect.h"
using namespace mkwvk;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); std::exit(1); } } while(0)
#define OK(x) CHECK((x)==VK_SUCCESS)
VkDevice device;
VkPhysicalDevice physical;
VkPhysicalDeviceMemoryProperties memory_props;
struct { VkExtent2D swap_extent; bool hdr = false; } dev;
std::atomic<bool> present_native_pq{false};
float display_aspect;
float get_aspect() { return display_aspect; }
struct Host { float (*aspect_ratio)(); } host_object{get_aspect};
Host* host = &host_object;
VkImage framegen_scale(VkCommandBuffer,VkImageView,VkImage,uint32_t,uint32_t,const int*,uint32_t,uint32_t,bool) {
    return VK_NULL_HANDLE; // This GPU fixture exercises direct blits; filtered routing is covered by L1.
}
// (SDR only here: the HDR pass and Native's routing are covered by test_present_aspect.py)
VkImage framegen_hdr(VkCommandBuffer,VkImageView,uint32_t,uint32_t,const int*,uint32_t,uint32_t,bool) { return VK_NULL_HANDLE; }
VkImageView framegen_scaled_view() { return VK_NULL_HANDLE; }
void full_barrier(VkCommandBuffer) {}
void logf(const char*,...) {}
// @PRODUCTION_HELPERS@
void record_actual(VkCommandBuffer command,VkImage src,uint32_t sw,uint32_t sh,const int* crop,VkImage dst) {
    struct { VkCommandBuffer cmd; } f{command};
    VkImageView view=VK_NULL_HANDLE;
// @PRODUCTION_PRESENT@
}
uint32_t memory_type(uint32_t bits,VkMemoryPropertyFlags flags) {
    for(uint32_t i=0;i<memory_props.memoryTypeCount;i++)
        if((bits&(1u<<i))&&(memory_props.memoryTypes[i].propertyFlags&flags)==flags) return i;
    CHECK(false); return 0;
}
struct Buffer { VkBuffer object; VkDeviceMemory memory; };
Buffer buffer(VkDeviceSize size,VkBufferUsageFlags usage) {
    Buffer b{}; VkBufferCreateInfo ci{}; ci.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ci.size=size;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    OK(vkCreateBuffer(device,&ci,nullptr,&b.object));
    VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device,b.object,&req);
    VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize=req.size;
    allocation.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    OK(vkAllocateMemory(device,&allocation,nullptr,&b.memory));
    OK(vkBindBufferMemory(device,b.object,b.memory,0));return b;
}
struct Image { VkImage object;VkDeviceMemory memory; };
Image image(uint32_t width,uint32_t height) {
    Image result{};VkImageCreateInfo ci{};ci.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;ci.extent={width,height,1};
    ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
    ci.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
    OK(vkCreateImage(device,&ci,nullptr,&result.object));
    VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,result.object,&req);
    VkMemoryAllocateInfo allocation{};allocation.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize=req.size;allocation.memoryTypeIndex=memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    OK(vkAllocateMemory(device,&allocation,nullptr,&result.memory));
    OK(vkBindImageMemory(device,result.object,result.memory,0));return result;
}
void release(Buffer b){vkDestroyBuffer(device,b.object,nullptr);vkFreeMemory(device,b.memory,nullptr);}
void release(Image i){vkDestroyImage(device,i.object,nullptr);vkFreeMemory(device,i.memory,nullptr);}
struct Case { const char* name;uint32_t width,height;float aspect;PresentRect expected;bool padded; };
std::string lower(std::string s) {
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return (char)std::tolower(c);});
    return s;
}
VkPhysicalDevice pick_device(const std::vector<VkPhysicalDevice>& cards,const std::string& requested) {
    VkPhysicalDevice selected=VK_NULL_HANDLE;
    for(auto candidate:cards) {
        VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(candidate,&props);
        if(!requested.empty() && lower(props.deviceName).find(requested)==std::string::npos) continue;
        if(!selected) selected=candidate;
        if(props.deviceType==VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) return candidate;
    }
    if(!selected) {
        std::fprintf(stderr,"No Vulkan device matched '%s'; devices seen:\n",requested.c_str());
        for(auto candidate:cards) {
            VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(candidate,&props);
            std::fprintf(stderr,"  %s\n",props.deviceName);
        }
    }
    return selected;
}
int main(int argc,char** argv) {
    std::string requested;
    if(argc==3 && std::strcmp(argv[1],"--device")==0 && argv[2][0]) requested=lower(argv[2]);
    else if(argc!=1) { std::fprintf(stderr,"usage: %s [--device NAME_SUBSTRING]\n",argv[0]); return 2; }
    VkApplicationInfo app{};app.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO;app.pApplicationName="Hookshot F01 synthetic presentation";
    app.apiVersion=VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{};ici.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;ici.pApplicationInfo=&app;
    VkInstance instance{};OK(vkCreateInstance(&ici,nullptr,&instance));
    uint32_t count=0;OK(vkEnumeratePhysicalDevices(instance,&count,nullptr));CHECK(count);
    std::vector<VkPhysicalDevice> cards(count);OK(vkEnumeratePhysicalDevices(instance,&count,cards.data()));
    physical=pick_device(cards,requested);
    if(!physical) { vkDestroyInstance(instance,nullptr); return 1; }
    VkPhysicalDeviceProperties props{};vkGetPhysicalDeviceProperties(physical,&props);
    std::printf("DEVICE: %s, api %u.%u, driver %u\n",props.deviceName,VK_VERSION_MAJOR(props.apiVersion),VK_VERSION_MINOR(props.apiVersion),props.driverVersion);
    CHECK(props.apiVersion>=VK_API_VERSION_1_3);
    vkGetPhysicalDeviceMemoryProperties(physical,&memory_props);
    VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,&format);
    const VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    CHECK((format.optimalTilingFeatures&needed)==needed);
    uint32_t families=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,nullptr);
    std::vector<VkQueueFamilyProperties> queues(families);vkGetPhysicalDeviceQueueFamilyProperties(physical,&families,queues.data());
    uint32_t family=0;while(family<families&&!(queues[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;CHECK(family<families);
    float priority=1;VkDeviceQueueCreateInfo qci{};qci.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
    VkPhysicalDeviceVulkan13Features features{};features.sType=VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    features.synchronization2=VK_TRUE;
    VkDeviceCreateInfo dci{};dci.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;dci.pNext=&features;dci.queueCreateInfoCount=1;dci.pQueueCreateInfos=&qci;
    OK(vkCreateDevice(physical,&dci,nullptr,&device));VkQueue queue{};vkGetDeviceQueue(device,family,0,&queue);
    VkCommandPoolCreateInfo pci{};pci.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;pci.queueFamilyIndex=family;
    VkCommandPool pool{};OK(vkCreateCommandPool(device,&pci,nullptr,&pool));
    VkCommandBufferAllocateInfo cai{};cai.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;cai.commandPool=pool;
    cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;VkCommandBuffer command{};OK(vkAllocateCommandBuffers(device,&cai,&command));
    const float wide=43.0f/18.0f;
    const Case cases[]={
        {"physical16:9",1600,1000,0,{0,50,1600,900},false},
        {"wide matching",3440,1440,wide,{0,0,3440,1440},false},
        {"maximize geometry",1920,1009,wide,{0,102,1920,804},false},
        {"wide into16:9",1280,720,wide,{0,92,1280,536},false},
        {"restore geometry",1720,720,wide,{0,0,1720,720},false},
        {"padded active crop",1280,720,wide,{0,92,1280,536},true},
        {"invalid override",1600,1000,std::numeric_limits<float>::infinity(),{0,50,1600,900},false}
    };
    for(const auto& test:cases){
        const uint32_t sw=test.padded?80:64,sh=test.padded?50:36;
        const int crop[4]={8,7,64,36};
        Buffer upload=buffer(sw*sh*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        Buffer download=buffer(VkDeviceSize(test.width)*test.height*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        Image src=image(sw,sh),dst=image(test.width,test.height);
        void* mapped=nullptr;OK(vkMapMemory(device,upload.memory,0,VK_WHOLE_SIZE,0,&mapped));
        auto* pixels=static_cast<uint8_t*>(mapped);
        for(uint32_t y=0;y<sh;y++)for(uint32_t x=0;x<sw;x++){
            bool white=!test.padded||(x>=8&&x<72&&y>=7&&y<43);
            const size_t at=(size_t(y)*sw+x)*4;
            pixels[at]=255;pixels[at+1]=pixels[at+2]=white?255:0;pixels[at+3]=255;
        }
        vkUnmapMemory(device,upload.memory);
        OK(vkResetCommandPool(device,pool,0));VkCommandBufferBeginInfo begin{};begin.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        OK(vkBeginCommandBuffer(command,&begin));
        image_barrier(command,src.object,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={sw,sh,1};
        vkCmdCopyBufferToImage(command,upload.object,src.object,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        image_barrier(command,src.object,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
        image_barrier(command,dst.object,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkClearColorValue magenta{{1,0,1,1}};const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        vkCmdClearColorImage(command,dst.object,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&magenta,1,&range);
        image_barrier(command,dst.object,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        dev.swap_extent={test.width,test.height};display_aspect=test.aspect;
        record_actual(command,src.object,sw,sh,test.padded?crop:nullptr,dst.object);
        image_barrier(command,dst.object,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        copy.imageExtent={test.width,test.height,1};
        vkCmdCopyImageToBuffer(command,dst.object,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,download.object,1,&copy);
        OK(vkEndCommandBuffer(command));VkSubmitInfo submit{};submit.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO;submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        OK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE));OK(vkQueueWaitIdle(queue));
        OK(vkMapMemory(device,download.memory,0,VK_WHOLE_SIZE,0,&mapped));pixels=static_cast<uint8_t*>(mapped);
        uint64_t checked=0;
        for(uint32_t y=0;y<test.height;y++)for(uint32_t x=0;x<test.width;x++){
            const auto r=test.expected;const bool inside=x>=r.x&&x<r.x+r.w&&y>=r.y&&y<r.y+r.h;
            // Vulkan linear blits sample a half-source-texel footprint beyond a
            // crop boundary. Exclude that scaled footprint only for the padded
            // fixture; still check every bar pixel and all other content pixels.
            const uint32_t margin_x=(r.w+127)/128+1,margin_y=(r.h+71)/72+1;
            if(test.padded&&inside&&(x-r.x<margin_x||r.x+r.w-1-x<margin_x||
                                     y-r.y<margin_y||r.y+r.h-1-y<margin_y))continue;
            const size_t at=(size_t(y)*test.width+x)*4;const uint8_t want=inside?255:0;
            if(pixels[at]!=want||pixels[at+1]!=want||pixels[at+2]!=want||pixels[at+3]!=255){
                std::fprintf(stderr,"FAIL %s pixel %u,%u: %u,%u,%u,%u\n",test.name,x,y,pixels[at],pixels[at+1],pixels[at+2],pixels[at+3]);return 1;
            }
            ++checked;
        }
        vkUnmapMemory(device,download.memory);release(src);release(dst);release(upload);release(download);
        std::printf("PASS %s: %llu GPU readback pixels\n",test.name,static_cast<unsigned long long>(checked));
    }
    vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
    std::puts("PASS: seven offscreen GPU cases; no swapchain/window, no game assets");return 0;
}
