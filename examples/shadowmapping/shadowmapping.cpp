/*
 * Vulkan Example - Shadow mapping for directional light sources
 *
 * Copyright (C) 2016-2021 by Sascha Willems - www.saschawillems.de
 *
 * This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
 */

/*
 * This sample implements a basic projected shadow mapping method using multiple render passes
 * The shadow map is generated in a first offscreen pass by rendering the scene from the light's point of view to a framebuffer
 * This framebuffer is used as a shadow map and projected onto the actual scene in the second render pass
 */

#include "vulkanexamplebase.h"
#include "VulkanglTFModel.h"

#define ENABLE_VALIDATION false


class VulkanExample : public VulkanExampleBase
{
public:
	// Use a smaller size for the shadow map on mobile devices
#if defined(__ANDROID__)
	VkExtent2D shadowMapExtent = { 1024, 1024 };
#else
	VkExtent2D shadowMapExtent = { 2048, 2048 };
#endif
	// Use 16 bits of depth precision for the shadow map, which is sufficient for this sample
	VkFormat shadowMapFormat = VK_FORMAT_D16_UNORM;

	bool displayShadowMap = false;
	bool filterPCF = true;

	// We keep depth range as small as possible for better shadow map precision
	float zNear = 1.0f;
	float zFar = 96.0f;

	// Depth bias (and slope) are used to avoid shadowing artifacts
	// The constant depth bias factor is always applied
	float depthBiasConstant = 1.25f;
	// The slope depth bias factor is applied depending on polygon's slope
	float depthBiasSlope = 1.75f;

	vkglTF::Model scene;

	struct UniformData {
		glm::mat4 projection;
		glm::mat4 view;
		glm::mat4 model;
		glm::mat4 depthMVP;
		glm::vec4 lightPos;
	} uniformData;
	
	// Holds the Vulkan objects to store an offscreen framebuffer for rendering the depth map to
	// This will then be sampled as the shadow map during scene rendering
	struct Framebuffer {
		VkDeviceMemory memory;
		VkImage image;
		VkImageView view;
		VkFramebuffer handle;
	};

	struct FrameObjects : public VulkanFrameObjects {
		vks::Buffer uniformBuffer;
		VkDescriptorSet descriptorSet;
	};
	std::vector<FrameObjects> frameObjects;
	// The descriptors for shadow map are static, and not required to be per-frame
	VkDescriptorSet shadowDescriptorSet;

	struct Pipelines {
		VkPipeline offscreen;
		VkPipeline sceneShadow;
		VkPipeline sceneShadowPCF;
		VkPipeline debug;
	} pipelines;
	VkPipelineLayout pipelineLayout;

	struct DescriptorSetLayouts {
		VkDescriptorSetLayout uniformbuffers;
		VkDescriptorSetLayout shadowmap;
	} descriptorSetLayouts;

	// @todo: struct

	VkRenderPass shadowMapRenderPass;
	VkSampler shadowMapSampler;
	VkPipelineLayout shadowMapPipelineLayout;
	Framebuffer shadowMapFramebuffer;

	VulkanExample() : VulkanExampleBase(ENABLE_VALIDATION)
	{
		title = "Projected shadow mapping";
		camera.setType(Camera::CameraType::lookat);
		camera.setPosition(glm::vec3(0.0f, 2.0f, -12.5f));
		camera.setRotation(glm::vec3(-15.0f, -390.0f, 0.0f));
		camera.setPerspective(60.0f, (float)width / (float)height, 1.0f, 256.0f);
		timerSpeed *= 0.5f;
		settings.overlay = true;
	}

	~VulkanExample()
	{
		if (device) {
			vkDestroySampler(device, shadowMapSampler, nullptr);
			vkDestroyRenderPass(device, shadowMapRenderPass, nullptr);
			vkDestroyPipeline(device, pipelines.debug, nullptr);
			vkDestroyPipeline(device, pipelines.offscreen, nullptr);
			vkDestroyPipeline(device, pipelines.sceneShadow, nullptr);
			vkDestroyPipeline(device, pipelines.sceneShadowPCF, nullptr);
			vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
			vkDestroyPipelineLayout(device, shadowMapPipelineLayout, nullptr);
			vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.uniformbuffers, nullptr);
			vkDestroyDescriptorSetLayout(device, descriptorSetLayouts.shadowmap, nullptr);
			for (FrameObjects& frame : frameObjects) {
				frame.uniformBuffer.destroy();
				destroyBaseFrameObjects(frame);
			}
			vkDestroyImageView(device, shadowMapFramebuffer.view, nullptr);
			vkDestroyImage(device, shadowMapFramebuffer.image, nullptr);
			vkFreeMemory(device, shadowMapFramebuffer.memory, nullptr);
			vkDestroyFramebuffer(device, shadowMapFramebuffer.handle, nullptr);
		}
	}

	// Create all Vulkan objects for the shadow map generation pass
	// This includes the depth image (that's also sampled in the scene rendering pass) and a separate render pass used for rendering to the shadow map
	void createShadowMapObjects()
	{
		// Set up a dedicated render pass for the offscreen frame buffer
		// This is necessary as the offscreen frame buffer attachments use formats different to those from the example render pass
		// This render pass also takes care of the image layout transitions and saves us from doing manual synchronization
		VkAttachmentDescription attachmentDescription{};
		attachmentDescription.format = shadowMapFormat;
		attachmentDescription.samples = VK_SAMPLE_COUNT_1_BIT;
		// Clear depth at beginning of the render pass
		attachmentDescription.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		// We will read from the depth attachment when sampling it as the shadow map, so the contents of the attachment need to be stored for the following render pass
		attachmentDescription.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachmentDescription.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachmentDescription.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachmentDescription.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		// The attachment will be transitioned to shader read at the end of the render pass, so we don't need to manually transition it
		attachmentDescription.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

		VkAttachmentReference depthReference = {};
		depthReference.attachment = 0;
		// Attachment will be used as depth/stencil during the depth render pass
		depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;			

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 0;
		// Reference to our depth attachment
		subpass.pDepthStencilAttachment = &depthReference;

		// Use subpass dependencies for layout transitions
		std::array<VkSubpassDependency, 2> dependencies;

		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		dependencies[1].srcSubpass = 0;
		dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
		dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

		VkRenderPassCreateInfo renderPassCreateInfo = vks::initializers::renderPassCreateInfo();
		renderPassCreateInfo.attachmentCount = 1;
		renderPassCreateInfo.pAttachments = &attachmentDescription;
		renderPassCreateInfo.subpassCount = 1;
		renderPassCreateInfo.pSubpasses = &subpass;
		renderPassCreateInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
		renderPassCreateInfo.pDependencies = dependencies.data();

		VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassCreateInfo, nullptr, &shadowMapRenderPass));

		// Ceate the offscreen framebuffer for rendering the depth information from the light's point-of-view to
		// The depth attachment of that framebuffer will then be used to sample from in the fragment shader of the shadowing pass
		VkImageCreateInfo image = vks::initializers::imageCreateInfo();
		image.imageType = VK_IMAGE_TYPE_2D;
		image.extent.width = shadowMapExtent.width;
		image.extent.height = shadowMapExtent.height;
		image.extent.depth = 1;
		image.mipLevels = 1;
		image.arrayLayers = 1;
		image.samples = VK_SAMPLE_COUNT_1_BIT;
		image.tiling = VK_IMAGE_TILING_OPTIMAL;
		image.format = shadowMapFormat;
		image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &shadowMapFramebuffer.image));

		VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(device, shadowMapFramebuffer.image, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &shadowMapFramebuffer.memory));
		VK_CHECK_RESULT(vkBindImageMemory(device, shadowMapFramebuffer.image, shadowMapFramebuffer.memory, 0));

		// Create the image View
		VkImageViewCreateInfo depthStencilView = vks::initializers::imageViewCreateInfo();
		depthStencilView.viewType = VK_IMAGE_VIEW_TYPE_2D;
		depthStencilView.format = shadowMapFormat;
		depthStencilView.subresourceRange = {};
		depthStencilView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		depthStencilView.subresourceRange.baseMipLevel = 0;
		depthStencilView.subresourceRange.levelCount = 1;
		depthStencilView.subresourceRange.baseArrayLayer = 0;
		depthStencilView.subresourceRange.layerCount = 1;
		depthStencilView.image = shadowMapFramebuffer.image;
		VK_CHECK_RESULT(vkCreateImageView(device, &depthStencilView, nullptr, &shadowMapFramebuffer.view));

		// Create the frame buffer
		VkFramebufferCreateInfo fbufCreateInfo = vks::initializers::framebufferCreateInfo();
		fbufCreateInfo.renderPass = shadowMapRenderPass;
		fbufCreateInfo.attachmentCount = 1;
		fbufCreateInfo.pAttachments = &shadowMapFramebuffer.view;
		fbufCreateInfo.width = shadowMapExtent.width;
		fbufCreateInfo.height = shadowMapExtent.height;
		fbufCreateInfo.layers = 1;
		VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &shadowMapFramebuffer.handle));

		// Create the sampler used to sample from the depth attachment in the scene rendering pass
		// Check if the current implementation supports linear filtering for the desired shadow map format
		VkFilter shadowmapFilter = vks::tools::formatIsFilterable(physicalDevice, shadowMapFormat, VK_IMAGE_TILING_OPTIMAL) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
		VkSamplerCreateInfo sampler = vks::initializers::samplerCreateInfo();
		sampler.magFilter = shadowmapFilter;
		sampler.minFilter = shadowmapFilter;
		sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		sampler.addressModeV = sampler.addressModeU;
		sampler.addressModeW = sampler.addressModeU;
		sampler.mipLodBias = 0.0f;
		sampler.maxAnisotropy = 1.0f;
		sampler.minLod = 0.0f;
		sampler.maxLod = 1.0f;
		sampler.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
		VK_CHECK_RESULT(vkCreateSampler(device, &sampler, nullptr, &shadowMapSampler));
	}

	void loadAssets()
	{
		const uint32_t glTFLoadingFlags = vkglTF::FileLoadingFlags::PreTransformVertices | vkglTF::FileLoadingFlags::PreMultiplyVertexColors | vkglTF::FileLoadingFlags::FlipY;
		scene.loadFromFile(getAssetPath() + "models/vulkanscene_shadow.gltf", vulkanDevice, queue, glTFLoadingFlags);
	}

	void createDescriptors()
	{
		// Pool
		std::vector<VkDescriptorPoolSize> poolSizes = {
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * getFrameCount()),
			vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1)
		};
		VkDescriptorPoolCreateInfo descriptorPoolInfo = vks::initializers::descriptorPoolCreateInfo(poolSizes, getFrameCount() + 1);
		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));

		// Layouts
		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCI{};
		VkDescriptorSetLayoutBinding setLayoutBinding{};

		// Layout for the per-frame uniform buffers
		setLayoutBinding = vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT, 0);
		descriptorSetLayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBinding);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCI, nullptr, &descriptorSetLayouts.uniformbuffers));

		// Layout for the shadow map image
		setLayoutBinding = vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT, 0);
		descriptorSetLayoutCI = vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBinding);
		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCI, nullptr, &descriptorSetLayouts.shadowmap));

		// Sets
		// Per-frame for dynamic uniform buffers
		for (FrameObjects& frame : frameObjects) {
			VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayouts.uniformbuffers, 1);
			VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &frame.descriptorSet));
			VkWriteDescriptorSet writeDescriptorSet = vks::initializers::writeDescriptorSet(frame.descriptorSet, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &frame.uniformBuffer.descriptor);
			vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
		}

		// Global set for the shadow map image
		VkDescriptorSetAllocateInfo allocInfo = vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayouts.shadowmap, 1);
		VkDescriptorImageInfo shadowMapDescriptor = vks::initializers::descriptorImageInfo(shadowMapSampler, shadowMapFramebuffer.view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &shadowDescriptorSet));
		VkWriteDescriptorSet writeDescriptorSet = vks::initializers::writeDescriptorSet(shadowDescriptorSet, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 0, &shadowMapDescriptor);
		vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
	}

	void createPipelines()
	{
		// Layout
		std::vector<VkDescriptorSetLayout> setLayouts = { descriptorSetLayouts.uniformbuffers, descriptorSetLayouts.shadowmap };
		VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo = vks::initializers::pipelineLayoutCreateInfo(setLayouts.data(), 2);
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayout));
		// @todo
		pipelineLayoutCreateInfo = vks::initializers::pipelineLayoutCreateInfo(&descriptorSetLayouts.uniformbuffers, 1);
		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &shadowMapPipelineLayout));

		// Pipelines
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCI = vks::initializers::pipelineInputAssemblyStateCreateInfo(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, VK_FALSE);
		VkPipelineRasterizationStateCreateInfo rasterizationStateCI = vks::initializers::pipelineRasterizationStateCreateInfo(VK_POLYGON_MODE_FILL, VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, 0);
		VkPipelineColorBlendAttachmentState blendAttachmentState = vks::initializers::pipelineColorBlendAttachmentState(0xf, VK_FALSE);
		VkPipelineColorBlendStateCreateInfo colorBlendStateCI = vks::initializers::pipelineColorBlendStateCreateInfo(1, &blendAttachmentState);
		VkPipelineDepthStencilStateCreateInfo depthStencilStateCI = vks::initializers::pipelineDepthStencilStateCreateInfo(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL);
		VkPipelineViewportStateCreateInfo viewportStateCI = vks::initializers::pipelineViewportStateCreateInfo(1, 1, 0);
		VkPipelineMultisampleStateCreateInfo multisampleStateCI = vks::initializers::pipelineMultisampleStateCreateInfo(VK_SAMPLE_COUNT_1_BIT, 0);
		std::vector<VkDynamicState> dynamicStateEnables = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicStateCI = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages;

		VkGraphicsPipelineCreateInfo pipelineCI = vks::initializers::pipelineCreateInfo(pipelineLayout, renderPass, 0);
		pipelineCI.pInputAssemblyState = &inputAssemblyStateCI;
		pipelineCI.pRasterizationState = &rasterizationStateCI;
		pipelineCI.pColorBlendState = &colorBlendStateCI;
		pipelineCI.pMultisampleState = &multisampleStateCI;
		pipelineCI.pViewportState = &viewportStateCI;
		pipelineCI.pDepthStencilState = &depthStencilStateCI;
		pipelineCI.pDynamicState = &dynamicStateCI;
		pipelineCI.stageCount = static_cast<uint32_t>(shaderStages.size());
		pipelineCI.pStages = shaderStages.data();

		// Shadow mapping debug quad display
		rasterizationStateCI.cullMode = VK_CULL_MODE_NONE;
		shaderStages[0] = loadShader(getShadersPath() + "shadowmapping/quad.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "shadowmapping/quad.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		// Empty vertex input state
		VkPipelineVertexInputStateCreateInfo emptyInputState = vks::initializers::pipelineVertexInputStateCreateInfo();
		pipelineCI.pVertexInputState = &emptyInputState;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.debug));

		// Scene rendering with shadows applied
		pipelineCI.pVertexInputState  = vkglTF::Vertex::getPipelineVertexInputState({vkglTF::VertexComponent::Position, vkglTF::VertexComponent::UV, vkglTF::VertexComponent::Color, vkglTF::VertexComponent::Normal});
		rasterizationStateCI.cullMode = VK_CULL_MODE_BACK_BIT;
		shaderStages[0] = loadShader(getShadersPath() + "shadowmapping/scene.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		shaderStages[1] = loadShader(getShadersPath() + "shadowmapping/scene.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
		// Use specialization constants to select between horizontal and vertical blur
		uint32_t enablePCF = 0;
		VkSpecializationMapEntry specializationMapEntry = vks::initializers::specializationMapEntry(0, 0, sizeof(uint32_t));
		VkSpecializationInfo specializationInfo = vks::initializers::specializationInfo(1, &specializationMapEntry, sizeof(uint32_t), &enablePCF);
		shaderStages[1].pSpecializationInfo = &specializationInfo;
		// Pipeline with basic shadow map filtering (if supported, see shadow map setup)
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.sceneShadow));
		// Pipeline with percentage closer filtering (PCF)
		enablePCF = 1;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.sceneShadowPCF));

		pipelineCI.layout = shadowMapPipelineLayout;
		// Offscreen pipeline (vertex shader only)
		shaderStages[0] = loadShader(getShadersPath() + "shadowmapping/offscreen.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
		pipelineCI.stageCount = 1;
		// No blend attachment states (no color attachments used)
		colorBlendStateCI.attachmentCount = 0;
		// Cull front faces
		depthStencilStateCI.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
		// Enable depth bias
		rasterizationStateCI.depthBiasEnable = VK_TRUE;
		// Add depth bias to dynamic state, so we can change it at runtime
		dynamicStateEnables.push_back(VK_DYNAMIC_STATE_DEPTH_BIAS);
		dynamicStateCI = vks::initializers::pipelineDynamicStateCreateInfo(dynamicStateEnables);

		pipelineCI.renderPass = shadowMapRenderPass;
		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipelines.offscreen));
	}

	void prepare()
	{
		VulkanExampleBase::prepare();
		// Prepare per-frame ressources
		frameObjects.resize(getFrameCount());
		for (FrameObjects& frame : frameObjects) {
			createBaseFrameObjects(frame);
			// Uniform buffers
			VK_CHECK_RESULT(vulkanDevice->createAndMapBuffer(VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &frame.uniformBuffer, sizeof(UniformData)));
		}
		loadAssets();
		createShadowMapObjects();
		createDescriptors();
		createPipelines();
		prepared = true;
	}

	virtual void render()
	{
		FrameObjects currentFrame = frameObjects[getCurrentFrameIndex()];

		VulkanExampleBase::prepareFrame(currentFrame);

		// Update uniform-buffers for the next frame
		if (!paused) {
			// Animate the light source
			uniformData.lightPos.x = cos(glm::radians(timer * 360.0f)) * 40.0f;
			uniformData.lightPos.y = -50.0f + sin(glm::radians(timer * 360.0f)) * 20.0f;
			uniformData.lightPos.z = 25.0f + sin(glm::radians(timer * 360.0f)) * 5.0f;
		}
		uniformData.projection = camera.matrices.perspective;
		uniformData.view = camera.matrices.view;
		uniformData.model = glm::mat4(1.0f);
		// Calcualte the matrix from light's point of view as the depth view model-view-projection matrix
		// The field-of-view should be kept as small as possible to maximize geometry to depth map resolution ratio
		glm::mat4 depthProjectionMatrix = glm::perspective(glm::radians(45.0f), 1.0f, zNear, zFar);
		glm::mat4 depthViewMatrix = glm::lookAt(glm::vec3(uniformData.lightPos), glm::vec3(0.0f), glm::vec3(0, 1, 0));
		glm::mat4 depthModelMatrix = glm::mat4(1.0f);
		uniformData.depthMVP = depthProjectionMatrix * depthViewMatrix * depthModelMatrix;
		memcpy(currentFrame.uniformBuffer.mapped, &uniformData, sizeof(uniformData));

		// Build the command buffer
		const VkCommandBuffer commandBuffer = currentFrame.commandBuffer;
		const VkCommandBufferBeginInfo commandBufferBeginInfo = getCommandBufferBeginInfo();
		VK_CHECK_RESULT(vkBeginCommandBuffer(commandBuffer, &commandBufferBeginInfo));

		// First render pass: Generate shadow map by rendering the scene from light's POV and storing it's depth in a framebuffer
		{
			VkClearValue clearValues[1];
			clearValues[0].depthStencil = { 1.0f, 0 };
			const VkViewport viewport = vks::initializers::viewport((float)shadowMapExtent.width, (float)shadowMapExtent.height, 0.0f, 1.0f);
			const VkRect2D scissor = vks::initializers::rect2D(shadowMapExtent.width, shadowMapExtent.height, 0, 0);

			VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
			renderPassBeginInfo.renderPass = shadowMapRenderPass;
			renderPassBeginInfo.framebuffer = shadowMapFramebuffer.handle;
			renderPassBeginInfo.renderArea.extent = shadowMapExtent;
			renderPassBeginInfo.clearValueCount = 1;
			renderPassBeginInfo.pClearValues = clearValues;

			vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
			vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
			// Set a depth bias (aka "Polygon offset") to avoid shadow mapping artifacts
			vkCmdSetDepthBias(commandBuffer, depthBiasConstant, 0.0f, depthBiasSlope);
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.offscreen);
			vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowMapPipelineLayout, 0, 1, &currentFrame.descriptorSet, 0, nullptr);
			scene.draw(commandBuffer);
			vkCmdEndRenderPass(commandBuffer);
		}

		// Second pass: Render the scene with shadows applied from the shadow map generated in the first pass
		// Note: Explicit synchronization is not required between the render pass, as this is done implicit via subpass dependencies specified in the shadow map renderpass
		{
			const VkRect2D renderArea = getRenderArea();
			const VkViewport viewport = getViewport();
			const VkRenderPassBeginInfo renderPassBeginInfo = getRenderPassBeginInfo(renderPass, defaultClearValues);
			vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
			vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
			vkCmdSetScissor(commandBuffer, 0, 1, &renderArea);

			// Bind uniform buffers for the current frame to set 0 and shadow map to set 1
			vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 0, 1, &currentFrame.descriptorSet, 0, nullptr);
			vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 1, 1, &shadowDescriptorSet, 0, nullptr);

			// Debug visualization of the shadow map
			if (displayShadowMap) {
				vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines.debug);
				vkCmdDraw(commandBuffer, 3, 1, 0, 0);
			}

			// Draw the scene with the shadow map applied
			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, (filterPCF) ? pipelines.sceneShadowPCF : pipelines.sceneShadow);
			scene.draw(commandBuffer);

			drawUI(commandBuffer);
			vkCmdEndRenderPass(commandBuffer);
		}

		VK_CHECK_RESULT(vkEndCommandBuffer(commandBuffer));

		VulkanExampleBase::submitFrame(currentFrame);
	}

	virtual void OnUpdateUIOverlay(vks::UIOverlay *overlay)
	{
		if (overlay->header("Settings")) {
			overlay->checkBox("Display shadow render target", &displayShadowMap);
			overlay->checkBox("Enable PCF shadow filtering", &filterPCF);
		}
	}
};

VULKAN_EXAMPLE_MAIN()
