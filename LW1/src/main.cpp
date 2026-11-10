#define GLFW_INCLUDE_VULKAN
#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "geometry.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

static void check(VkResult r) {
    if (r != VK_SUCCESS) throw std::runtime_error("Vulkan error: " + std::to_string(r));
}
struct Buffer { VkBuffer handle{}; VkDeviceMemory memory{}; void* mapped{}; };
struct Depth { VkImage image{}; VkDeviceMemory memory{}; VkImageView view{}; };
struct alignas(16) Uniform { glm::mat4 mvp; glm::vec4 tint; glm::vec4 options; };
struct Object {
    glm::vec3 position{0}, rotation{0}, scale{1};
    glm::vec4 tint{1};
    bool visible=true, procedural=true, animate=false;
    float radius=1.4f, height=.6f, speed=1.f, phase=0.f;
    double time=0;
    Buffer uniform;
    VkDescriptorSet descriptor{};
};
class App {
    GLFWwindow* window{};
    VkInstance instance{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    uint32_t family{};
    VkQueue queue{};
    VkDescriptorPool pool{};
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout pipelineLayout{};
    VkPipeline pipeline{};
    VkFormat depthFormat{};
    ImGui_ImplVulkanH_Window wd{};
    std::vector<Depth> depths;
    Buffer vertex, index;
    std::array<Object,3> objects;
    bool glfwReady=false, contextReady=false, glfwBackend=false, vkBackend=false;
    bool perspective=true, playing=true, rebuild=false;
    float fov=45.f, orthoSize=4.f, distance=8.f, yaw=35.f, pitch=22.f;
    int selected=0, objectCount=1;
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties p; vkGetPhysicalDeviceMemoryProperties(gpu,&p);
        for(uint32_t i=0;i<p.memoryTypeCount;++i)
            if((bits&(1u<<i)) && (p.memoryTypes[i].propertyFlags&flags)==flags) return i;
        throw std::runtime_error("Suitable GPU memory type not found");
    }
    Buffer buffer(VkDeviceSize size, VkBufferUsageFlags usage, const void* data=nullptr) {
        Buffer b;
        VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        ci.size=size; ci.usage=usage; ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device,&ci,nullptr,&b.handle));
        VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,b.handle,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize=req.size;
        ai.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        check(vkAllocateMemory(device,&ai,nullptr,&b.memory));
        check(vkBindBufferMemory(device,b.handle,b.memory,0));
        check(vkMapMemory(device,b.memory,0,size,0,&b.mapped));
        if(data) std::memcpy(b.mapped,data,static_cast<size_t>(size));
        return b;
    }
    void freeBuffer(Buffer& b) {
        if(b.mapped) vkUnmapMemory(device,b.memory);
        if(b.handle) vkDestroyBuffer(device,b.handle,nullptr);
        if(b.memory) vkFreeMemory(device,b.memory,nullptr);
        b={};
    }
    void resetObject(int i) {
        auto u=objects[i].uniform; auto d=objects[i].descriptor;
        objects[i]=Object{}; objects[i].uniform=u; objects[i].descriptor=d;
        objects[i].position={i==1 ? -2.5f : i==2 ? 2.5f : 0.f,0,0};
        objects[i].phase=i*2.0944f;
    }
    void initVulkan() {
        uint32_t n=0; const char** extensions=glfwGetRequiredInstanceExtensions(&n);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName="Lab 1 - Regular octahedron"; app.apiVersion=VK_API_VERSION_1_0;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo=&app; ci.enabledExtensionCount=n; ci.ppEnabledExtensionNames=extensions;
        check(vkCreateInstance(&ci,nullptr,&instance));
        check(glfwCreateWindowSurface(instance,window,nullptr,&wd.Surface));
        check(vkEnumeratePhysicalDevices(instance,&n,nullptr));
        std::vector<VkPhysicalDevice> devices(n); check(vkEnumeratePhysicalDevices(instance,&n,devices.data()));
        for(auto candidate:devices) {
            uint32_t count=0; vkGetPhysicalDeviceQueueFamilyProperties(candidate,&count,nullptr);
            std::vector<VkQueueFamilyProperties> props(count); vkGetPhysicalDeviceQueueFamilyProperties(candidate,&count,props.data());
            for(uint32_t i=0;i<count;++i) {
                VkBool32 present=false; check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate,i,wd.Surface,&present));
                if(present && (props[i].queueFlags&VK_QUEUE_GRAPHICS_BIT)) { gpu=candidate; family=i; break; }
            }
            if(gpu) break;
        }
        if(!gpu) throw std::runtime_error("No Vulkan GPU with graphics and presentation support");
        float priority=1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
        const char* ext=VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi; di.enabledExtensionCount=1; di.ppEnabledExtensionNames=&ext;
        check(vkCreateDevice(gpu,&di,nullptr,&device)); vkGetDeviceQueue(device,family,0,&queue);
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,32},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,16}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.flags=VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT; pi.maxSets=48; pi.poolSizeCount=2; pi.pPoolSizes=sizes;
        check(vkCreateDescriptorPool(device,&pi,nullptr,&pool));
        const VkFormat formats[]={VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R8G8B8A8_UNORM};
        wd.SurfaceFormat=ImGui_ImplVulkanH_SelectSurfaceFormat(gpu,wd.Surface,formats,2,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);
        const VkPresentModeKHR mode=VK_PRESENT_MODE_FIFO_KHR;
        wd.PresentMode=ImGui_ImplVulkanH_SelectPresentMode(gpu,wd.Surface,&mode,1);
        for(auto f:{VK_FORMAT_D32_SFLOAT,VK_FORMAT_D24_UNORM_S8_UINT,VK_FORMAT_D16_UNORM}) {
            VkFormatProperties p; vkGetPhysicalDeviceFormatProperties(gpu,f,&p);
            if(p.optimalTilingFeatures&VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {depthFormat=f; break;}
        }
        if(depthFormat==VK_FORMAT_UNDEFINED) throw std::runtime_error("No depth attachment format");
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; li.bindingCount=1; li.pBindings=&binding;
        check(vkCreateDescriptorSetLayout(device,&li,nullptr,&setLayout));
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pl.setLayoutCount=1; pl.pSetLayouts=&setLayout;
        check(vkCreatePipelineLayout(device,&pl,nullptr,&pipelineLayout));
        vertex=buffer(sizeof(vertices),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,vertices.data());
        index=buffer(sizeof(indices),VK_BUFFER_USAGE_INDEX_BUFFER_BIT,indices.data());
        for(int i=0;i<3;++i) {
            resetObject(i); auto& o=objects[i]; o.uniform=buffer(sizeof(Uniform),VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; ai.descriptorPool=pool; ai.descriptorSetCount=1; ai.pSetLayouts=&setLayout;
            check(vkAllocateDescriptorSets(device,&ai,&o.descriptor));
            VkDescriptorBufferInfo bi{o.uniform.handle,0,sizeof(Uniform)};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=o.descriptor; write.dstBinding=0;
            write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; write.pBufferInfo=&bi;
            vkUpdateDescriptorSets(device,1,&write,0,nullptr);
        }
    }
    void clearDepths() {
        for(auto& d:depths) {vkDestroyImageView(device,d.view,nullptr); vkDestroyImage(device,d.image,nullptr); vkFreeMemory(device,d.memory,nullptr);}
        depths.clear();
    }
    // Extend the ImGui sample swapchain with real depth attachments for the 3D scene.
    void createRenderTargets(int w,int h) {
        check(vkDeviceWaitIdle(device));
        if(pipeline) {vkDestroyPipeline(device,pipeline,nullptr); pipeline={};}
        // Destroy framebuffers before the depth views to which they refer.
        for(uint32_t i=0;i<wd.ImageCount;++i) {
            vkDestroyFramebuffer(device,wd.Frames[i].Framebuffer,nullptr); wd.Frames[i].Framebuffer={};
        }
        clearDepths();
        ImGui_ImplVulkanH_CreateOrResizeWindow(instance,gpu,device,&wd,family,nullptr,w,h,2);
        for(uint32_t i=0;i<wd.ImageCount;++i) vkDestroyFramebuffer(device,wd.Frames[i].Framebuffer,nullptr);
        vkDestroyRenderPass(device,wd.RenderPass,nullptr);
        VkAttachmentDescription attachments[2]{};
        attachments[0].format=wd.SurfaceFormat.format; attachments[0].samples=VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachments[0].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachments[0].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; attachments[0].finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachments[1]=attachments[0]; attachments[1].format=depthFormat;
        attachments[1].storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE; attachments[1].finalLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, depth{1,VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; sub.colorAttachmentCount=1; sub.pColorAttachments=&color; sub.pDepthStencilAttachment=&depth;
        VkSubpassDependency dep{}; dep.srcSubpass=VK_SUBPASS_EXTERNAL; dep.dstSubpass=0;
        dep.srcStageMask=dep.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT|VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO}; ri.attachmentCount=2; ri.pAttachments=attachments; ri.subpassCount=1; ri.pSubpasses=&sub; ri.dependencyCount=1; ri.pDependencies=&dep;
        check(vkCreateRenderPass(device,&ri,nullptr,&wd.RenderPass));
        depths.resize(wd.ImageCount);
        for(uint32_t i=0;i<wd.ImageCount;++i) {
            auto& d=depths[i];
            VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType=VK_IMAGE_TYPE_2D; ii.format=depthFormat;
            ii.extent={static_cast<uint32_t>(wd.Width),static_cast<uint32_t>(wd.Height),1}; ii.mipLevels=ii.arrayLayers=1;
            ii.samples=VK_SAMPLE_COUNT_1_BIT; ii.tiling=VK_IMAGE_TILING_OPTIMAL; ii.usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
            check(vkCreateImage(device,&ii,nullptr,&d.image));
            VkMemoryRequirements req; vkGetImageMemoryRequirements(device,d.image,&req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize=req.size; ai.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            check(vkAllocateMemory(device,&ai,nullptr,&d.memory)); check(vkBindImageMemory(device,d.image,d.memory,0));
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=d.image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D; vi.format=depthFormat;
            vi.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1}; check(vkCreateImageView(device,&vi,nullptr,&d.view));
            VkImageView views[]={wd.Frames[i].BackbufferView,d.view};
            VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO}; fi.renderPass=wd.RenderPass; fi.attachmentCount=2; fi.pAttachments=views;
            fi.width=wd.Width; fi.height=wd.Height; fi.layers=1; check(vkCreateFramebuffer(device,&fi,nullptr,&wd.Frames[i].Framebuffer));
        }
        createPipeline();
    }
    VkShaderModule shader(const char* name) {
        std::ifstream f(std::string(SHADER_DIR)+"/"+name,std::ios::binary|std::ios::ate);
        if(!f) throw std::runtime_error(std::string("Cannot open shader: ")+name);
        auto size=f.tellg(); if(size<=0 || size%4) throw std::runtime_error("Invalid SPIR-V file");
        std::vector<uint32_t> code(static_cast<size_t>(size)/4); f.seekg(0); f.read(reinterpret_cast<char*>(code.data()),size);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize=static_cast<size_t>(size); ci.pCode=code.data();
        VkShaderModule result; check(vkCreateShaderModule(device,&ci,nullptr,&result)); return result;
    }
    void createPipeline() {
        VkShaderModule vert=shader("octahedron.vert.spv"), frag=shader("octahedron.frag.spv");
        VkPipelineShaderStageCreateInfo stages[2]{};
        for(int i=0;i<2;++i) {stages[i].sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; stages[i].pName="main";}
        stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT; stages[0].module=vert;
        stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module=frag;
        VkVertexInputBindingDescription binding{0,sizeof(glm::vec3),VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attr{0,0,VK_FORMAT_R32G32B32_SFLOAT,0};
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount=vi.vertexAttributeDescriptionCount=1; vi.pVertexBindingDescriptions=&binding; vi.pVertexAttributeDescriptions=&attr;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount=vp.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode=VK_POLYGON_MODE_FILL; rs.cullMode=VK_CULL_MODE_BACK_BIT; rs.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable=ds.depthWriteEnable=VK_TRUE; ds.depthCompareOp=VK_COMPARE_OP_LESS;
        VkPipelineColorBlendAttachmentState ba{}; ba.colorWriteMask=0xf;
        VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; bs.attachmentCount=1; bs.pAttachments=&ba;
        VkDynamicState dynamic[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dy.dynamicStateCount=2; dy.pDynamicStates=dynamic;
        VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pi.stageCount=2; pi.pStages=stages; pi.pVertexInputState=&vi; pi.pInputAssemblyState=&ia; pi.pViewportState=&vp;
        pi.pRasterizationState=&rs; pi.pMultisampleState=&ms; pi.pDepthStencilState=&ds; pi.pColorBlendState=&bs; pi.pDynamicState=&dy;
        pi.layout=pipelineLayout; pi.renderPass=wd.RenderPass;
        auto result=vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline);
        vkDestroyShaderModule(device,vert,nullptr); vkDestroyShaderModule(device,frag,nullptr); check(result);
    }
    void initGui() {
        IMGUI_CHECKVERSION(); ImGui::CreateContext(); contextReady=true; ImGui::StyleColorsDark();
        ImGui::GetIO().IniFilename=nullptr;
        ImGui_ImplGlfw_InitForVulkan(window,true); glfwBackend=true;
        ImGui_ImplVulkan_InitInfo info{}; info.Instance=instance; info.PhysicalDevice=gpu; info.Device=device;
        info.QueueFamily=family; info.Queue=queue; info.DescriptorPool=pool; info.MinImageCount=2; info.ImageCount=wd.ImageCount;
        info.MSAASamples=VK_SAMPLE_COUNT_1_BIT; info.CheckVkResultFn=check;
        ImGui_ImplVulkan_Init(&info,wd.RenderPass); vkBackend=true;
    }
    void ui(float dt) {
        ImGui_ImplVulkan_NewFrame(); ImGui_ImplGlfw_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos({16,16},ImGuiCond_Once); ImGui::SetNextWindowSize({330,680},ImGuiCond_Once);
        ImGui::Begin("Lab 1 | Variant 2");
        ImGui::Text("Regular octahedron"); ImGui::TextDisabled("6 vertices / 12 edges / 8 faces");
        if(ImGui::CollapsingHeader("Projection and camera",ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Perspective",&perspective);
            if(perspective) ImGui::SliderFloat("FOV",&fov,20,90,"%.0f deg");
            else ImGui::SliderFloat("Ortho half-height",&orthoSize,1,10);
            ImGui::SliderFloat("Distance",&distance,3,20); ImGui::SliderFloat("Yaw",&yaw,-180,180); ImGui::SliderFloat("Pitch",&pitch,-80,80);
        }
        ImGui::SliderInt("Objects",&objectCount,1,3); selected=std::min(selected,objectCount-1);
        const char* names[]={"Octahedron 1","Octahedron 2","Octahedron 3"};
        ImGui::Combo("Selected",&selected,names,objectCount); auto& o=objects[selected];
        ImGui::Checkbox("Visible",&o.visible);
        if(ImGui::CollapsingHeader("Transform",ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::DragFloat3("Position",glm::value_ptr(o.position),.02f,-10,10);
            ImGui::SliderFloat3("Rotation",glm::value_ptr(o.rotation),-180,180,"%.0f");
            ImGui::SliderFloat3("Scale",glm::value_ptr(o.scale),.1f,3.f);
            // Clamp typed inputs as well: singular/negative scales invalidate winding.
            o.scale=glm::clamp(o.scale,glm::vec3(.1f),glm::vec3(3.f));
            ImGui::ColorEdit3("Tint",glm::value_ptr(o.tint)); ImGui::Checkbox("Procedural vertex colors",&o.procedural);
        }
        if(ImGui::CollapsingHeader("Animation",ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Checkbox("Animate selected object",&o.animate);
            if(ImGui::Button(playing?"Pause all":"Play all")) playing=!playing;
            ImGui::SameLine(); if(ImGui::Button("Restart selected")) o.time=0;
            ImGui::SliderFloat("Speed",&o.speed,.1f,3); ImGui::SliderFloat("Radius",&o.radius,0,3);
            ImGui::SliderFloat("Height",&o.height,0,2); ImGui::SliderFloat("Phase",&o.phase,0,6.283185f);
            ImGui::TextDisabled("Figure-eight path + rotation on 3 axes");
        }
        if(ImGui::Button("Reset selected object")) resetObject(selected);
        ImGui::TextDisabled("%.1f FPS | Esc: exit",ImGui::GetIO().Framerate);
        ImGui::End(); ImGui::Render();
        float a=glm::radians(yaw), b=glm::radians(pitch);
        glm::vec3 eye=distance*glm::vec3(std::cos(b)*std::sin(a),std::sin(b),std::cos(b)*std::cos(a));
        auto view=glm::lookAt(eye,glm::vec3(0),glm::vec3(0,1,0));
        // Reserve the left portion for the control panel.
        float sceneWidth=static_cast<float>(std::max(1,wd.Width-panelWidth()));
        float aspect=sceneWidth/static_cast<float>(wd.Height);
        auto projection=perspective?glm::perspective(glm::radians(fov),aspect,.1f,100.f):glm::ortho(-orthoSize*aspect,orthoSize*aspect,-orthoSize,orthoSize,.1f,100.f);
        projection[1][1]*=-1; // Vulkan framebuffer Y and GLM's zero-to-one depth.
        for(auto& obj:objects) {
            if(playing && obj.animate) obj.time+=dt*obj.speed;
            float t=static_cast<float>(obj.time)+obj.phase;
            glm::vec3 offset(0), spin(0);
            if(obj.animate) {offset={obj.radius*std::sin(t),obj.height*std::sin(3*t),obj.radius*.5f*std::sin(2*t)}; spin={t*.7f,t,t*.4f};}
            auto model=glm::translate(glm::mat4(1),obj.position+offset);
            auto angles=glm::radians(obj.rotation)+spin;
            model=glm::rotate(model,angles.z,{0,0,1}); model=glm::rotate(model,angles.y,{0,1,0}); model=glm::rotate(model,angles.x,{1,0,0});
            model=glm::scale(model,obj.scale);
            Uniform u{projection*view*model,obj.tint,{obj.procedural?1.f:0.f,0,0,0}};
            std::memcpy(obj.uniform.mapped,&u,sizeof(u));
        }
    }
    int panelWidth() const { int w,h; glfwGetWindowSize(window,&w,&h); return std::min(wd.Width/2,static_cast<int>(360.f*wd.Width/std::max(w,1))); }
    void draw() {
        // One frame in flight keeps the per-object UBOs and swapchain semaphores simple.
        auto& sem=wd.FrameSemaphores[wd.SemaphoreIndex];
        VkResult r=vkAcquireNextImageKHR(device,wd.Swapchain,UINT64_MAX,sem.ImageAcquiredSemaphore,VK_NULL_HANDLE,&wd.FrameIndex);
        if(r==VK_ERROR_OUT_OF_DATE_KHR) {rebuild=true; return;}
        if(r==VK_SUBOPTIMAL_KHR) rebuild=true; else check(r);
        auto& frame=wd.Frames[wd.FrameIndex];
        check(vkWaitForFences(device,1,&frame.Fence,VK_TRUE,UINT64_MAX)); check(vkResetFences(device,1,&frame.Fence));
        check(vkResetCommandPool(device,frame.CommandPool,0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(frame.CommandBuffer,&bi));
        VkClearValue clears[2]{}; clears[0].color={{.035f,.045f,.07f,1}}; clears[1].depthStencil={1,0};
        VkRenderPassBeginInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO}; ri.renderPass=wd.RenderPass; ri.framebuffer=frame.Framebuffer;
        ri.renderArea.extent={static_cast<uint32_t>(wd.Width),static_cast<uint32_t>(wd.Height)}; ri.clearValueCount=2; ri.pClearValues=clears;
        auto cmd=frame.CommandBuffer; vkCmdBeginRenderPass(cmd,&ri,VK_SUBPASS_CONTENTS_INLINE);
        int left=panelWidth();
        VkViewport vp{static_cast<float>(left),0,static_cast<float>(wd.Width-left),static_cast<float>(wd.Height),0,1};
        VkRect2D sc{{left,0},{static_cast<uint32_t>(wd.Width-left),static_cast<uint32_t>(wd.Height)}};
        vkCmdSetViewport(cmd,0,1,&vp); vkCmdSetScissor(cmd,0,1,&sc); vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        VkDeviceSize offset=0; vkCmdBindVertexBuffers(cmd,0,1,&vertex.handle,&offset); vkCmdBindIndexBuffer(cmd,index.handle,0,VK_INDEX_TYPE_UINT16);
        for(int i=0;i<objectCount;++i) if(objects[i].visible) {
            vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&objects[i].descriptor,0,nullptr);
            vkCmdDrawIndexed(cmd,static_cast<uint32_t>(indices.size()),1,0,0,0);
        }
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),cmd); vkCmdEndRenderPass(cmd); check(vkEndCommandBuffer(cmd));
        VkPipelineStageFlags stage=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.waitSemaphoreCount=1; si.pWaitSemaphores=&sem.ImageAcquiredSemaphore; si.pWaitDstStageMask=&stage;
        si.commandBufferCount=1; si.pCommandBuffers=&cmd; si.signalSemaphoreCount=1; si.pSignalSemaphores=&sem.RenderCompleteSemaphore;
        check(vkQueueSubmit(queue,1,&si,frame.Fence));
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; pi.waitSemaphoreCount=1; pi.pWaitSemaphores=&sem.RenderCompleteSemaphore;
        pi.swapchainCount=1; pi.pSwapchains=&wd.Swapchain; pi.pImageIndices=&wd.FrameIndex;
        r=vkQueuePresentKHR(queue,&pi); if(r==VK_ERROR_OUT_OF_DATE_KHR || r==VK_SUBOPTIMAL_KHR) rebuild=true; else check(r);
        check(vkQueueWaitIdle(queue));
        wd.SemaphoreIndex=(wd.SemaphoreIndex+1)%wd.ImageCount;
    }
public:
    void run(int frames,bool demo,bool smoke) {
        glfwSetErrorCallback([](int code,const char* message){std::cerr<<"GLFW "<<code<<": "<<message<<'\n';});
        if(!glfwInit()) throw std::runtime_error("GLFW initialization failed");
        glfwReady=true;
        if(!glfwVulkanSupported()) throw std::runtime_error("Vulkan loader/driver is unavailable");
        glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API); window=glfwCreateWindow(1280,800,"CG Lab 1 - Variant 2 - Octahedron",nullptr,nullptr);
        if(!window) throw std::runtime_error("Window creation failed");
        initVulkan(); int w,h; glfwGetFramebufferSize(window,&w,&h); createRenderTargets(w,h); initGui();
        if(demo) {objectCount=3; for(auto& o:objects) {o.animate=true; o.radius=.5f;}}
        double last=glfwGetTime(); int rendered=0;
        while(!glfwWindowShouldClose(window) && (frames<=0 || rendered<frames)) {
            if(smoke) {
                if(rendered==15) perspective=false;
                if(rendered==30) glfwSetWindowSize(window,1000,700);
                if(rendered==45) playing=false;
                if(rendered==60) { playing=true; perspective=true; glfwSetWindowSize(window,1280,800); }
            }
            glfwPollEvents(); if(glfwGetKey(window,GLFW_KEY_ESCAPE)==GLFW_PRESS) break;
            glfwGetFramebufferSize(window,&w,&h);
            if(w==0 || h==0) {glfwWaitEventsTimeout(.1); last=glfwGetTime(); continue;}
            if(rebuild || w!=wd.Width || h!=wd.Height) {
                check(vkDeviceWaitIdle(device)); ImGui_ImplVulkan_Shutdown(); vkBackend=false;
                createRenderTargets(w,h);
                VkRenderPass pass=wd.RenderPass;
                ImGui_ImplVulkan_InitInfo info{}; info.Instance=instance; info.PhysicalDevice=gpu; info.Device=device; info.QueueFamily=family; info.Queue=queue;
                info.DescriptorPool=pool; info.MinImageCount=2; info.ImageCount=wd.ImageCount; info.MSAASamples=VK_SAMPLE_COUNT_1_BIT; info.CheckVkResultFn=check;
                ImGui_ImplVulkan_Init(&info,pass); vkBackend=true; rebuild=false; wd.SemaphoreIndex=0;
            }
            double now=glfwGetTime(); ui(static_cast<float>(std::min(now-last,.1))); last=now; draw(); ++rendered;
        }
    }
    ~App() {
        if(device) vkDeviceWaitIdle(device);
        if(vkBackend) ImGui_ImplVulkan_Shutdown();
        if(glfwBackend) ImGui_ImplGlfw_Shutdown();
        if(contextReady) ImGui::DestroyContext();
        if(device) {
            if(pipeline) vkDestroyPipeline(device,pipeline,nullptr);
            if(pipelineLayout) vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
            for(auto& o:objects) freeBuffer(o.uniform);
            freeBuffer(vertex); freeBuffer(index);
            if(pool) vkDestroyDescriptorPool(device,pool,nullptr);
            if(setLayout) vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
            ImGui_ImplVulkanH_DestroyWindow(instance,device,&wd,nullptr); clearDepths();
            vkDestroyDevice(device,nullptr);
        } else if(wd.Surface) vkDestroySurfaceKHR(instance,wd.Surface,nullptr);
        if(instance) vkDestroyInstance(instance,nullptr);
        if(window) glfwDestroyWindow(window);
        if(glfwReady) glfwTerminate();
    }
};
int main(int argc,char** argv) {
    try {
        int frames=0; bool demo=false, smoke=false;
        for(int i=1;i<argc;++i) {
            std::string arg=argv[i];
            if(arg=="--frames" && i+1<argc) frames=std::stoi(argv[++i]);
            else if(arg=="--demo") demo=true;
            else if(arg=="--smoke-test") {smoke=true; demo=true; frames=120;}
            else {std::cout<<"Usage: octahedron [--frames N] [--demo] [--smoke-test]\n"; return arg=="--help"?0:1;}
        }
        App app; app.run(frames,demo,smoke); return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
