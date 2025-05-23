/// \file Renderer.c
/// \author Paolo Mazzon
#include <vulkan/vulkan.h>
#include <SDL3/SDL_vulkan.h>
#include <time.h>
#include <stdio.h>
#include <math.h>

#ifndef __APPLE__
#include <malloc.h>
#else
#include <sys/cdefs.h>
#include <stdlib.h>
#include <unistd.h>
#include <malloc/_malloc.h>
#include <malloc/malloc.h>
#include <memory.h>
#endif

#include "VK2D/RendererMeta.h"
#include "VK2D/Renderer.h"
#include "VK2D/Validation.h"
#include "VK2D/Initializers.h"
#include "VK2D/Constants.h"
#include "VK2D/PhysicalDevice.h"
#include "VK2D/LogicalDevice.h"
#include "VK2D/Texture.h"
#include "VK2D/Shader.h"
#include "VK2D/Image.h"
#include "VK2D/Model.h"
#include "VK2D/DescriptorBuffer.h"
#include "VK2D/DescriptorControl.h"
#include "VK2D/Opaque.h"
#include "VK2D/Pipeline.h"

/******************************* Forward declarations *******************************/

bool _vk2dFileExists(const char *filename);
unsigned char* _vk2dLoadFile(const char *filename, uint32_t *size);

/******************************* Globals *******************************/

// For everything
VK2DRenderer gRenderer = NULL;
SDL_AtomicInt gRNG;
static const char *gHostMachineBuffer[4096];
static int gHostMachineBufferSize = 4096;

static VK2DStartupOptions DEFAULT_STARTUP_OPTIONS = {
    .enableDebug = false,
    .stdoutLogging = true,
    .quitOnError = true,
    .errorFile = "vk2derror.txt",
    .vramPageSize = 256 * 1000,
    .maxTextures = 10000
};

/******************************* User-visible functions *******************************/

VK2DResult vk2dRendererInit(void *window, VK2DRendererConfig config, VK2DStartupOptions *options) {
	gRenderer = calloc(1, sizeof(struct VK2DRenderer_t));
	VK2DResult errorCode = VK2D_SUCCESS;
	uint32_t i, sdlExtensionsCount;
	const char** sdlExtensions;

    // Find the startup options
    VK2DStartupOptions userOptions;
    if (options == NULL) {
        userOptions = DEFAULT_STARTUP_OPTIONS;
    } else {
        userOptions = *options;
        if (userOptions.vramPageSize == 0)
            userOptions.vramPageSize = DEFAULT_STARTUP_OPTIONS.vramPageSize;
        if (userOptions.maxTextures == 0)
            userOptions.maxTextures = DEFAULT_STARTUP_OPTIONS.maxTextures;
        if (userOptions.errorFile == NULL)
            userOptions.errorFile = DEFAULT_STARTUP_OPTIONS.errorFile;
    }

	// Validation initialization needs to happen right away
	vk2dValidationBegin(userOptions.errorFile, userOptions.quitOnError);

    if (vk2dRendererGetPointer() != NULL) {
        // Print all available layers
        VkLayerProperties *systemLayers;
        uint32_t systemLayerCount;
        VkResult result = vkEnumerateInstanceLayerProperties(&systemLayerCount, VK_NULL_HANDLE);

        if (result != VK_SUCCESS) {
            free(gRenderer);
            gRenderer = NULL;
            vk2dRaise(VK2D_STATUS_VULKAN_ERROR, "Failed to get layers, Vulkan error %i.", result);
            return VK2D_ERROR;
        }

        systemLayers = malloc(sizeof(VkLayerProperties) * systemLayerCount);
        result = vkEnumerateInstanceLayerProperties(&systemLayerCount, systemLayers);

        if (result != VK_SUCCESS) {
            free(gRenderer);
            gRenderer = NULL;
            vk2dRaise(VK2D_STATUS_VULKAN_ERROR, "Failed to get layers, Vulkan error %i.", result);
            return VK2D_ERROR;
        }

        // Find if specific extensions are available
        uint32_t instanceExtensionCount = 0;
        VkExtensionProperties *instanceExtensions = VK_NULL_HANDLE;
        vkEnumerateInstanceExtensionProperties(VK_NULL_HANDLE, &instanceExtensionCount, VK_NULL_HANDLE);
        instanceExtensions = malloc(instanceExtensionCount * sizeof(VkExtensionProperties));
        vkEnumerateInstanceExtensionProperties(VK_NULL_HANDLE, &instanceExtensionCount, instanceExtensions);
        for (i = 0; i < instanceExtensionCount; i++) {
            if (strcmp(instanceExtensions[i].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0)
                gRenderer->limits.supportsVRAMUsage = true;
        }
        free(instanceExtensions);

        // Create extension/layer lists
        const char *extensions[10] = {0};
        const char *layers[10] = {0};
        int extensionCount = 0;
        int layerCount = 0;

        if (userOptions.enableDebug) {
            extensions[extensionCount++] = VK_EXT_DEBUG_REPORT_EXTENSION_NAME;
            layers[layerCount++] = "VK_LAYER_KHRONOS_validation";
        }
        if (gRenderer->limits.supportsVRAMUsage) {
            extensions[extensionCount++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
        }

        // Find number of total number of extensions
        sdlExtensions = (void*)SDL_Vulkan_GetInstanceExtensions(&sdlExtensionsCount);

		// Copy user options
		gRenderer->options = userOptions;

		// Load extensions
		if (!sdlExtensions) {
            free(gRenderer);
            gRenderer = NULL;
            vk2dRaise(VK2D_STATUS_SDL_ERROR | VK2D_STATUS_VULKAN_ERROR, "Failed to get extensions, SDL error %s.", SDL_GetError());
            return VK2D_ERROR;
        }

        // Append SDL extensions to extension list
        for (i = 0; i < sdlExtensionsCount; i++) {
            extensions[extensionCount++] = sdlExtensions[i];
        }

		// Log all used extensions
        vk2dLog("Vulkan Enabled Instance Extensions: ");
		for (i = 0; i < extensionCount; i++)
            vk2dLog(" - %s", extensions[i]);
        vk2dLog(""); // Newline

        // List all used layers
        vk2dLog("Vulkan Enabled Instance Layers: ");
        for (i = 0; i < layerCount; i++)
            vk2dLog("  - %s", layers[i]);
        vk2dLog("");
        free(systemLayers);

		// Create instance, physical, and logical device
		VkInstanceCreateInfo instanceCreateInfo;
        instanceCreateInfo = vk2dInitInstanceCreateInfo(
                (void *) &VK2D_DEFAULT_CONFIG,
                layers,
                layerCount,
                extensions,
                extensionCount
        );
		result = vkCreateInstance(&instanceCreateInfo, VK_NULL_HANDLE, &gRenderer->vk);

        if (result != VK_SUCCESS) {
		    vk2dRaise(VK2D_STATUS_VULKAN_ERROR, "Failed to create Vulkan instance, Vulkan error %i.", result);
		    free(gRenderer);
		    gRenderer = NULL;
		    return VK2D_ERROR;
		}
		gRenderer->pd = vk2dPhysicalDeviceFind(gRenderer->vk, VK2D_DEVICE_BEST_FIT);
		if (vk2dStatusFatal()) {
            vk2dRaise(0, "\nFailed to initialize renderer.");
            vk2dRendererQuit();
            return VK2D_ERROR;
		}
		gRenderer->ld = vk2dLogicalDeviceCreate(gRenderer->pd, false, true, userOptions.enableDebug, &gRenderer->limits);
        if (vk2dStatusFatal()) {
            vk2dRendererQuit();
            return VK2D_ERROR;
        }
		gRenderer->window = window;

        // Make the host machine string
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(gRenderer->pd->dev, &props);
        snprintf((void*)gHostMachineBuffer, gHostMachineBufferSize,
                 "%s, SDL %i.%i.%i\nHost: %i logical cores, %0.2fgb RAM\nDevice: %s, Vulkan %i.%i.%i, Vulkan2D %i.%i.%i\n",
                 SDL_GetPlatform(),
                 SDL_MAJOR_VERSION,
                 SDL_MINOR_VERSION,
                 SDL_MICRO_VERSION,
                 SDL_GetNumLogicalCPUCores(),
                 (float)SDL_GetSystemRAM() / 1024.0f,
                 props.deviceName,
                 VK_VERSION_MAJOR(props.apiVersion),
                 VK_VERSION_MINOR(props.apiVersion),
                 VK_VERSION_PATCH(props.apiVersion),
                 VK2D_VERSION_MAJOR,
                 VK2D_VERSION_MINOR,
                 VK2D_VERSION_PATCH
        );

        vk2dValidationWriteHeader();

		// Assign user settings, except for screen mode which will be handled later
		gRenderer->config = config;
		gRenderer->config.msaa = gRenderer->limits.maxMSAA >= config.msaa ? config.msaa : gRenderer->limits.maxMSAA;
		gRenderer->newConfig = gRenderer->config;

		// Calculate the limit for shader uniform buffers
		gRenderer->limits.maxShaderBufferSize = gRenderer->pd->props.limits.maxUniformBufferRange < userOptions.vramPageSize ? gRenderer->pd->props.limits.maxUniformBufferRange : userOptions.vramPageSize;
		gRenderer->limits.maxGeometryVertices = (userOptions.vramPageSize / sizeof(VK2DVertexColour)) - 1;

		// Create the VMA
		VmaAllocatorCreateInfo allocatorCreateInfo = {0};
		allocatorCreateInfo.device = gRenderer->ld->dev;
		allocatorCreateInfo.physicalDevice = gRenderer->pd->dev;
		allocatorCreateInfo.instance = gRenderer->vk;
		allocatorCreateInfo.vulkanApiVersion = VK_MAKE_VERSION(1, 1, 0);
		allocatorCreateInfo.flags = gRenderer->limits.supportsVRAMUsage ? VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT : 0;
		result = vmaCreateAllocator(&allocatorCreateInfo, &gRenderer->vma);
        if (result != VK_SUCCESS) {
            vk2dRaise(VK2D_STATUS_VULKAN_ERROR, "\nFailed to initialize VMA, Vulkan error %i.", result);
            vk2dRendererQuit();
            return VK2D_ERROR;
        }

        // Create budget list
        gRenderer->vmaBudgets = malloc(gRenderer->pd->mem.memoryHeapCount * sizeof(VmaBudget));

		// Initialize subsystems
		_vk2dRendererCreateDebug();
		_vk2dRendererCreateWindowSurface();
		_vk2dRendererCreateSwapchain();
		_vk2dRendererCreateColourResources();
		_vk2dRendererCreateDepthBuffer();
		_vk2dRendererCreateRenderPass();
		_vk2dRendererCreateDescriptorSetLayouts();
		_vk2dRendererCreatePipelines();
		_vk2dRendererCreateFrameBuffer();
		_vk2dRendererCreateDescriptorPool(false);
		_vk2dRendererCreateDescriptorBuffers();
		_vk2dRendererCreateUniformBuffers(true);
		_vk2dRendererCreateSampler();
		_vk2dRendererCreateUnits();
		_vk2dRendererCreateSynchronization();
		_vk2dRendererCreateSpriteBatching();

		// Quit if something failed
        if (vk2dStatusFatal()) {
            vk2dRendererQuit();
            return VK2D_ERROR;
        }

		vk2dRendererSetColourMod((void*)VK2D_DEFAULT_COLOUR_MOD);
		gRenderer->viewport.x = 0;
		gRenderer->viewport.y = 0;
		gRenderer->viewport.width = gRenderer->surfaceWidth;
		gRenderer->viewport.height = gRenderer->surfaceHeight;
		gRenderer->viewport.minDepth = 0;
		gRenderer->viewport.maxDepth = 1;

		// Initialize the random seed
		SDL_SetAtomicInt(&gRNG, time(0));
	} else {
		errorCode = VK2D_ERROR;
		vk2dRaise(VK2D_STATUS_OUT_OF_RAM, "Failed to allocate renderer struct.");
	}

	return errorCode;
}

void vk2dRendererQuit() {
	if (vk2dRendererGetPointer() != NULL) {
	    if (gRenderer->ld != NULL && gRenderer->ld->queue != NULL)
		    vkQueueWaitIdle(gRenderer->ld->queue);

		// Destroy subsystems
        _vk2dRendererDestroySpriteBatching();
		_vk2dRendererDestroySynchronization();
		_vk2dRendererDestroyTargetsList();
		_vk2dRendererDestroyUnits();
		_vk2dRendererDestroySampler();
		_vk2dRendererDestroyDescriptorPool(false);
		_vk2dRendererDestroyDescriptorBuffers();
		_vk2dRendererDestroyUniformBuffers();
		_vk2dRendererDestroyFrameBuffer();
		_vk2dRendererDestroyPipelines(false);
		_vk2dRendererDestroyDescriptorSetLayout();
		_vk2dRendererDestroyRenderPass();
		_vk2dRendererDestroyDepthBuffer();
		_vk2dRendererDestroyColourResources();
		_vk2dRendererDestroySwapchain();
		_vk2dRendererDestroyWindowSurface();
		_vk2dRendererDestroyDebug();
		vmaDestroyAllocator(gRenderer->vma);

		// Destroy core bits
		vk2dLogicalDeviceFree(gRenderer->ld);
		vk2dPhysicalDeviceFree(gRenderer->pd);
		free(gRenderer->vmaBudgets);

        vk2dLog("VK2D has been uninitialized.");
		vk2dValidationEnd();
		free(gRenderer);
		gRenderer = NULL;
	}
}

const char *vk2dHostInformation() {
    return (void*)gHostMachineBuffer;
}

void vk2dRendererWait() {
	if (vk2dRendererGetPointer() != NULL)
		vkQueueWaitIdle(gRenderer->ld->queue);
}

VK2DRenderer vk2dRendererGetPointer() {
    if (gRenderer == NULL) {
        vk2dRaise(VK2D_STATUS_RENDERER_NOT_INITIALIZED, "Renderer not initialized.");
    }
	return gRenderer;
}

void vk2dRendererResetSwapchain() {
	if (vk2dRendererGetPointer() != NULL)
		gRenderer->resetSwapchain = true;
}

VK2DRendererConfig vk2dRendererGetConfig() {
	if (vk2dRendererGetPointer() != NULL)
		return gRenderer->config;
	VK2DRendererConfig c = {0};
	return c;
}

void vk2dRendererSetConfig(VK2DRendererConfig config) {
	if (vk2dRendererGetPointer() != NULL) {
		gRenderer->newConfig = config;
		gRenderer->newConfig.msaa = gRenderer->limits.maxMSAA >= config.msaa ? config.msaa : gRenderer->limits.maxMSAA;
		vk2dRendererResetSwapchain();
	}
}

void vk2dRendererGetVRAMUsage(float *inUse, float *total) {
    *inUse = 0;
    *total = 0;
    vmaGetHeapBudgets(gRenderer->vma, gRenderer->vmaBudgets);
    for (int i = 0; i < gRenderer->pd->mem.memoryHeapCount; i++) {
        if ((gRenderer->pd->mem.memoryHeaps[i].flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0) continue;
        *total += gRenderer->vmaBudgets[i].budget;
        *inUse += gRenderer->vmaBudgets[i].usage;
    }
    *total /= 1048576;
    *inUse /= 1048576;
}

void vk2dRendererStartFrame(const vec4 clearColour) {
	if (vk2dRendererGetPointer() != NULL) {
		if (!gRenderer->procedStartFrame) {
			gRenderer->procedStartFrame = true;

			/*********** Get image and synchronization ***********/

			gRenderer->previousTime = SDL_GetPerformanceCounter();

			// Wait for previous rendering to be finished
			vkWaitForFences(gRenderer->ld->dev, 1, &gRenderer->inFlightFences[gRenderer->currentFrame], VK_TRUE,
							UINT64_MAX);

			// Acquire image
			VkResult result = vkAcquireNextImageKHR(gRenderer->ld->dev, gRenderer->swapchain, UINT64_MAX,
								  gRenderer->imageAvailableSemaphores[gRenderer->currentFrame], VK_NULL_HANDLE,
								  &gRenderer->scImageIndex);

			if (result < 0) {
			    if (result == VK_ERROR_DEVICE_LOST) {
                    vk2dRaise(VK2D_STATUS_DEVICE_LOST, "Vulkan device lost.");
			    } else {
                    vk2dRaise(VK2D_STATUS_VULKAN_ERROR, "Failed to acquire next image, Vulkan error %i.", result);
			    }
			    return;
			}

			if (gRenderer->imagesInFlight[gRenderer->scImageIndex] != VK_NULL_HANDLE) {
				vkWaitForFences(gRenderer->ld->dev, 1, &gRenderer->imagesInFlight[gRenderer->scImageIndex], VK_TRUE,
								UINT64_MAX);
			}
			gRenderer->imagesInFlight[gRenderer->scImageIndex] = gRenderer->inFlightFences[gRenderer->currentFrame];

			/*********** Start-of-frame tasks ***********/

			// Reset currently bound items
			_vk2dRendererResetBoundPointers();

			// Update VMA's frame
            vmaSetCurrentFrameIndex(gRenderer->vma, gRenderer->currentFrame);

			// Reset current render targets
			gRenderer->targetFrameBuffer = gRenderer->framebuffers[gRenderer->scImageIndex];
			gRenderer->targetRenderPass = gRenderer->renderPass;
			gRenderer->targetSubPass = 0;
			gRenderer->targetImage = gRenderer->swapchainImages[gRenderer->scImageIndex];
			gRenderer->targetUBOSet = gRenderer->uboDescriptorSets[gRenderer->currentFrame]; // TODO: Should prob be reworked
			gRenderer->target = VK2D_TARGET_SCREEN;
			_vk2dRendererResetBatch();

			// Start the render pass
			VkCommandBufferBeginInfo beginInfo = vk2dInitCommandBufferBeginInfo(
					VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
					VK_NULL_HANDLE);
			result = vkResetCommandBuffer(gRenderer->commandBuffer[gRenderer->scImageIndex], 0);
            VkResult result2 = vkResetCommandBuffer(gRenderer->dbCommandBuffer[gRenderer->scImageIndex], 0);
            VkResult result3 = vkResetCommandBuffer(gRenderer->computeCommandBuffer[gRenderer->scImageIndex], 0);
            if (result != VK_SUCCESS || result2 != VK_SUCCESS || result3 != VK_SUCCESS) {
			    vk2dRaise(VK2D_STATUS_OUT_OF_VRAM, "Failed to reset command buffer at start of frame.");
			    return;
			}
			result = vkBeginCommandBuffer(gRenderer->commandBuffer[gRenderer->scImageIndex], &beginInfo);
            result2 = vkBeginCommandBuffer(gRenderer->dbCommandBuffer[gRenderer->scImageIndex], &beginInfo);
            result3 = vkBeginCommandBuffer(gRenderer->computeCommandBuffer[gRenderer->scImageIndex], &beginInfo);
            if (result != VK_SUCCESS || result2 != VK_SUCCESS || result3 != VK_SUCCESS) {
                vk2dRaise(VK2D_STATUS_VULKAN_ERROR, "Failed to begin command buffer at start of frame, Vulkan error %i/%i/%i.", result, result2, result3);
                return;
            }

			// Begin descriptor buffer and sprite batching
            vk2dDescriptorBufferBeginFrame(gRenderer->descriptorBuffers[gRenderer->currentFrame], gRenderer->dbCommandBuffer[gRenderer->scImageIndex]);
            //gRenderer->spriteBatchCount = 0;

			// Flush the current ubo into its buffer for the frame
            for (int i = 0; i < VK2D_MAX_CAMERAS; i++)
                if (gRenderer->cameras[i].state == VK2D_CAMERA_STATE_NORMAL)
                    _vk2dCameraUpdateUBO(&gRenderer->workingUBO, &gRenderer->cameras[i].spec, i);
			_vk2dRendererFlushUBOBuffers();

			// Desc cons
			vk2dDescConReset(gRenderer->descConShaders[gRenderer->currentFrame]);
            vk2dDescConReset(gRenderer->descConCompute[gRenderer->currentFrame]);
            vk2dDescConReset(gRenderer->descConSBO[gRenderer->currentFrame]);

            // Setup render pass
			VkRect2D rect = {0};
			rect.extent.width = gRenderer->surfaceWidth;
			rect.extent.height = gRenderer->surfaceHeight;
			const uint32_t clearCount = 2;
			VkClearValue clearValues[2] = {0};
			clearValues[0].color.float32[0] = clearColour[0];
			clearValues[0].color.float32[1] = clearColour[1];
			clearValues[0].color.float32[2] = clearColour[2];
			clearValues[0].color.float32[3] = clearColour[3];
			clearValues[1].depthStencil.depth = 1;
			VkRenderPassBeginInfo renderPassBeginInfo = vk2dInitRenderPassBeginInfo(
					gRenderer->renderPass,
					gRenderer->framebuffers[gRenderer->scImageIndex],
					rect,
					clearValues,
					clearCount);

			vkCmdBeginRenderPass(gRenderer->commandBuffer[gRenderer->scImageIndex], &renderPassBeginInfo,
								 VK_SUBPASS_CONTENTS_INLINE);

			// Bind compute pipeline to the compute buffer
            vkCmdBindPipeline(gRenderer->computeCommandBuffer[gRenderer->scImageIndex], VK_PIPELINE_BIND_POINT_COMPUTE, vk2dPipelineGetCompute(gRenderer->spriteBatchPipe));
		}
	}
}

VK2DResult vk2dRendererEndFrame() {
	VK2DResult res = VK2D_SUCCESS;
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
		if (gRenderer->procedStartFrame) {
		    // Flush whatevers on batch
		    vk2dRendererFlushSpriteBatch();

			gRenderer->procedStartFrame = false;

			// Make sure we're not in the wrong pipeline
			if (gRenderer->target != VK2D_TARGET_SCREEN) {
				vk2dRendererSetTarget(VK2D_TARGET_SCREEN);
			}

			// Dispatch compute and end the descriptor buffer frame
            vkCmdEndRenderPass(gRenderer->commandBuffer[gRenderer->scImageIndex]);
			//_vk2dRendererDispatchCompute();
            vk2dDescriptorBufferEndFrame(gRenderer->descriptorBuffers[gRenderer->currentFrame], gRenderer->dbCommandBuffer[gRenderer->scImageIndex]);

            // Record necessary pipeline barriers to the copy and compute buffers
            vk2dDescriptorBufferRecordCopyPipelineBarrier(gRenderer->descriptorBuffers[gRenderer->currentFrame], gRenderer->dbCommandBuffer[gRenderer->scImageIndex]);
            vk2dDescriptorBufferRecordComputePipelineBarrier(gRenderer->descriptorBuffers[gRenderer->currentFrame], gRenderer->computeCommandBuffer[gRenderer->scImageIndex]);

            // PRESENT
            VkResult result = vkEndCommandBuffer(gRenderer->commandBuffer[gRenderer->scImageIndex]);
            VkResult result2 = vkEndCommandBuffer(gRenderer->dbCommandBuffer[gRenderer->scImageIndex]);
            VkResult result3 = vkEndCommandBuffer(gRenderer->computeCommandBuffer[gRenderer->scImageIndex]);
            if (result != VK_SUCCESS || result2 != VK_SUCCESS || result3 != VK_SUCCESS) {
                vk2dRaise(VK2D_STATUS_VULKAN_ERROR, "Failed to begin command buffer at start of frame, Vulkan error %i/%i/%i.", result, result2, result3);
                return VK2D_ERROR;
            }

			// Wait for image before doing things
			VkPipelineStageFlags waitStage[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
			VkCommandBuffer bufs[] = {gRenderer->dbCommandBuffer[gRenderer->scImageIndex], gRenderer->computeCommandBuffer[gRenderer->scImageIndex], gRenderer->commandBuffer[gRenderer->scImageIndex]};
			VkSubmitInfo submitInfo = vk2dInitSubmitInfo(
					bufs,
					3,
					&gRenderer->renderFinishedSemaphores[gRenderer->currentFrame],
					1,
					&gRenderer->imageAvailableSemaphores[gRenderer->currentFrame],
					1,
					waitStage);

			// Submit queue
			if (vkResetFences(gRenderer->ld->dev, 1, &gRenderer->inFlightFences[gRenderer->currentFrame]) != VK_SUCCESS) {
			    vk2dRaise(VK2D_STATUS_OUT_OF_VRAM, "Failed to reset fences.");
                return VK2D_ERROR;
			}
			result = vkQueueSubmit(gRenderer->ld->queue, 1, &submitInfo,
										 gRenderer->inFlightFences[gRenderer->currentFrame]);
			if (result < 0) {
			    if (result == VK_ERROR_DEVICE_LOST)
			        vk2dRaise(VK2D_STATUS_DEVICE_LOST, "Vulkan device lost.");
			    else
                    vk2dRaise(VK2D_STATUS_VULKAN_ERROR, "Failed to submit queue, Vulkan error %i.", result);
                return VK2D_ERROR;
			}

			// Final present info bit
			VkPresentInfoKHR presentInfo = vk2dInitPresentInfoKHR(&gRenderer->swapchain, 1, &gRenderer->scImageIndex,
																  &result,
																  &gRenderer->renderFinishedSemaphores[gRenderer->currentFrame],
																  1);
			VkResult queueRes = vkQueuePresentKHR(gRenderer->ld->queue, &presentInfo);
			if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || gRenderer->resetSwapchain ||
				queueRes == VK_ERROR_OUT_OF_DATE_KHR) {
				_vk2dRendererResetSwapchain();
				gRenderer->resetSwapchain = false;
				res = VK2D_RESET_SWAPCHAIN;
			} else if (result < 0 || queueRes < 0) {
                vk2dRaise(VK2D_STATUS_VULKAN_ERROR, "Failed to present frame, Vulkan error %i/%i.", queueRes, result);
			}

			gRenderer->currentFrame = (gRenderer->currentFrame + 1) % VK2D_MAX_FRAMES_IN_FLIGHT;

			// Calculate time
			gRenderer->accumulatedTime += (((double) SDL_GetPerformanceCounter() - gRenderer->previousTime) /
										   (double) SDL_GetPerformanceFrequency()) * 1000;
			gRenderer->amountOfFrames++;
			if (gRenderer->accumulatedTime >= 1000) {
				gRenderer->frameTimeAverage = gRenderer->accumulatedTime / gRenderer->amountOfFrames;
				gRenderer->accumulatedTime = 0;
				gRenderer->amountOfFrames = 0;
			}
		}
	}

	return res;
}

VK2DLogicalDevice vk2dRendererGetDevice() {
	if (vk2dRendererGetPointer() != NULL)
		return gRenderer->ld;
	return NULL;
}

void vk2dRendererSetTarget(VK2DTexture target) {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
		if (target != gRenderer->target) {
            vk2dRendererFlushSpriteBatch();

			// In case the user attempts to switch targets from one texture to another
			if (target != VK2D_TARGET_SCREEN && gRenderer->target != VK2D_TARGET_SCREEN) {
				vk2dRendererSetTarget(VK2D_TARGET_SCREEN);
			}

			// Dont let the user bind textures that are not targets
			if (target != VK2D_TARGET_SCREEN && !vk2dTextureIsTarget(target)) {
                vk2dLog("Texture cannot be used as a target.");
				return;
			}

			gRenderer->target = target;

			// Figure out which render pass to use
			VkRenderPass pass = target == VK2D_TARGET_SCREEN ? gRenderer->midFrameSwapRenderPass
															 : gRenderer->externalTargetRenderPass;
			VkFramebuffer framebuffer =
					target == VK2D_TARGET_SCREEN ? gRenderer->framebuffers[gRenderer->scImageIndex] : target->fbo;
			VkImage image = target == VK2D_TARGET_SCREEN ? gRenderer->swapchainImages[gRenderer->scImageIndex]
														 : target->img->img;
			VkDescriptorSet buffer =
					target == VK2D_TARGET_SCREEN ? gRenderer->uboDescriptorSets[gRenderer->currentFrame]
												 : target->uboSet;

			vkCmdEndRenderPass(gRenderer->commandBuffer[gRenderer->scImageIndex]);

			// Now we either have to transition the image layout depending on whats going in and whats poppin out
			if (target == VK2D_TARGET_SCREEN)
				_vk2dTransitionImageLayout(gRenderer->targetImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
										   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			else
				_vk2dTransitionImageLayout(target->img->img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
										   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

			// Assign new render targets
			gRenderer->targetRenderPass = pass;
			gRenderer->targetFrameBuffer = framebuffer;
			gRenderer->targetImage = image;
			gRenderer->targetUBOSet = buffer;

			// Setup new render pass
			VkRect2D rect = {0};
			rect.extent.width = target == VK2D_TARGET_SCREEN ? gRenderer->surfaceWidth : target->img->width;
			rect.extent.height = target == VK2D_TARGET_SCREEN ? gRenderer->surfaceHeight : target->img->height;
			VkClearValue clear[2] = {0};
			clear[1].depthStencil.depth = 1;
			VkRenderPassBeginInfo renderPassBeginInfo = vk2dInitRenderPassBeginInfo(
					pass,
					framebuffer,
					rect,
					clear,
					2);

			vkCmdBeginRenderPass(gRenderer->commandBuffer[gRenderer->scImageIndex], &renderPassBeginInfo,
								 VK_SUBPASS_CONTENTS_INLINE);

			_vk2dRendererResetBoundPointers();
		}
	}
}

void vk2dRendererSetColourMod(const vec4 mod) {
	if (vk2dRendererGetPointer() != NULL) {
		gRenderer->colourBlend[0] = mod[0];
		gRenderer->colourBlend[1] = mod[1];
		gRenderer->colourBlend[2] = mod[2];
		gRenderer->colourBlend[3] = mod[3];
	}
}

void vk2dRendererGetColourMod(vec4 dst) {
	if (vk2dRendererGetPointer() != NULL) {
		dst[0] = gRenderer->colourBlend[0];
		dst[1] = gRenderer->colourBlend[1];
		dst[2] = gRenderer->colourBlend[2];
		dst[3] = gRenderer->colourBlend[3];
	}
}

void vk2dRendererSetBlendMode(VK2DBlendMode blendMode) {
    vk2dRendererFlushSpriteBatch();
	if (vk2dRendererGetPointer() != NULL)
		gRenderer->blendMode = blendMode;
}

VK2DBlendMode vk2dRendererGetBlendMode() {
	if (vk2dRendererGetPointer() != NULL)
		return gRenderer->blendMode;
	return VK2D_BLEND_MODE_NONE;
}

void vk2dRendererSetCamera(VK2DCameraSpec camera) {
	if (vk2dRendererGetPointer() != NULL) {
		gRenderer->cameras[VK2D_DEFAULT_CAMERA].spec = camera;
		gRenderer->cameras[VK2D_DEFAULT_CAMERA].spec.wOnScreen = gRenderer->surfaceWidth;
		gRenderer->cameras[VK2D_DEFAULT_CAMERA].spec.hOnScreen = gRenderer->surfaceHeight;
		gRenderer->cameras[VK2D_DEFAULT_CAMERA].spec.xOnScreen = 0;
		gRenderer->cameras[VK2D_DEFAULT_CAMERA].spec.yOnScreen = 0;
	}
}

VK2DCameraSpec vk2dRendererGetCamera() {
	if (vk2dRendererGetPointer() != NULL)
		return gRenderer->defaultCameraSpec;
	VK2DCameraSpec c = {0};
	return c;
}

void vk2dRendererSetTextureCamera(bool useCameraOnTextures) {
    vk2dRendererFlushSpriteBatch();
	if (vk2dRendererGetPointer() != NULL)
		gRenderer->enableTextureCameraUBO = useCameraOnTextures;
}

void vk2dRendererLockCameras(VK2DCameraIndex cam) {
    vk2dRendererFlushSpriteBatch();
	if (vk2dRendererGetPointer() != NULL)
		gRenderer->cameraLocked = cam;
}

void vk2dRendererUnlockCameras() {
    vk2dRendererFlushSpriteBatch();
	if (vk2dRendererGetPointer() != NULL)
		gRenderer->cameraLocked = VK2D_INVALID_CAMERA;
}

double vk2dRendererGetAverageFrameTime() {
	if (vk2dRendererGetPointer() != NULL)
		return gRenderer->frameTimeAverage;
	return 0;
}

void vk2dRendererClear() {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
        vk2dRendererFlushSpriteBatch();

		VkDescriptorSet set = gRenderer->uboDescriptorSets[gRenderer->currentFrame];
		_vk2dRendererDrawRaw(&set, 1, gRenderer->unitSquare, gRenderer->primFillPipe, 0, 0, 1, 1, 0, 0, 0, 1, 0, 0, 0,
							 0, VK2D_INVALID_CAMERA);
	}
}

void vk2dRendererEmpty() {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
        vk2dRendererFlushSpriteBatch();

		// Save old renderer state to revert back
		VK2DBlendMode bm = vk2dRendererGetBlendMode();
		vec4 c;
		vk2dRendererGetColourMod(c);

		// Set the render mode to be blend mode none, and the colour to be a flat 0
		const vec4 clearColour = {0, 0, 0, 0};
		vk2dRendererSetColourMod(clearColour);
		vk2dRendererSetBlendMode(VK2D_BLEND_MODE_NONE);
		vk2dRendererClear();

		vk2dRendererSetColourMod(c);
		vk2dRendererSetBlendMode(bm);
	}
}

VK2DRendererLimits vk2dRendererGetLimits() {
	if (vk2dRendererGetPointer() != NULL) {
		return gRenderer->limits;
	}
	VK2DRendererLimits l = {0};
	return l;
}

void vk2dRendererDrawRectangle(float x, float y, float w, float h, float r, float ox, float oy) {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
        vk2dRendererFlushSpriteBatch();
		vk2dRendererDrawPolygon(gRenderer->unitSquare, x, y, true, 1, w, h, r, ox / (w / 3), oy / (h / 3));
	}
}

void vk2dRendererDrawRectangleOutline(float x, float y, float w, float h, float r, float ox, float oy, float lineWidth) {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
        vk2dRendererFlushSpriteBatch();
		vk2dRendererDrawPolygon(gRenderer->unitSquareOutline, x, y, false, lineWidth, w, h, r, ox / (w / 3), oy / (h / 3));
	}
}

void vk2dRendererDrawCircle(float x, float y, float r) {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
        vk2dRendererFlushSpriteBatch();
		vk2dRendererDrawPolygon(gRenderer->unitCircle, x, y, true, 1, r * 2, r * 2, 0, 0, 0);
	}
}

void vk2dRendererDrawCircleOutline(float x, float y, float r, float lineWidth) {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
        vk2dRendererFlushSpriteBatch();
		vk2dRendererDrawPolygon(gRenderer->unitCircleOutline, x, y, false, lineWidth, r * 2, r * 2, 0, 0, 0);
	}
}

void vk2dRendererDrawLine(float x1, float y1, float x2, float y2) {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
        vk2dRendererFlushSpriteBatch();
		float x = sqrtf(powf(y2 - y1, 2) + powf(x2 - x1, 2));
		float r = atan2f(y2 - y1, x2 - x1);
		vk2dRendererDrawPolygon(gRenderer->unitLine, x1, y1, false, 1, x, 1, r, 0, 0);
	}
}

void vk2dRendererDrawShader(VK2DShader shader, void *data, VK2DTexture tex, float x, float y, float xscale, float yscale, float rot, float originX, float originY, float xInTex, float yInTex, float texWidth, float texHeight) {
    if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
        if (shader != NULL) {
            _vk2dRendererFlushBatchIfNeeded(shader->pipe);

            VkDescriptorSet sets[4];
            sets[1] = gRenderer->samplerSet;
            sets[2] = gRenderer->texArrayDescriptorSet;

            // Create the data uniform
            uint32_t setCount = 3;
            if (shader->uniformSize != 0) {
                sets[3] = vk2dDescConGetSet(gRenderer->descConShaders[gRenderer->currentFrame]);
                VkBuffer buffer;
                VkDeviceSize offset;
                vk2dDescriptorBufferCopyData(gRenderer->descriptorBuffers[gRenderer->currentFrame], data, shader->uniformSize, &buffer, &offset);
                VkDescriptorBufferInfo bufferInfo = {buffer,offset,shader->uniformSize};
                VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.pBufferInfo = &bufferInfo;
                write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                write.dstBinding = 3;
                write.dstSet = sets[3];
                write.descriptorCount = 1;
                vkUpdateDescriptorSets(gRenderer->ld->dev, 1, &write, 0, VK_NULL_HANDLE);
                setCount = 4;
            }

            _vk2dRendererDrawShader(sets, setCount, tex, shader->pipe, x, y, xscale, yscale, rot, originX, originY, 1,
                              xInTex,
                              yInTex, texWidth, texHeight);
        } else {
            vk2dRaise(VK2D_STATUS_BAD_ASSET, "Shader does not exist.");
        }
    }
}

void vk2dRendererAddBatch(VK2DDrawCommand *commands, uint32_t count) {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
        const VK2DPipeline pipe = gRenderer->instancedPipe;
        for (int i = 0; i < count; i++) {
            _vk2dRendererFlushBatchIfNeeded(pipe);
            _vk2dRendererAddDrawCommand(&commands[i]);
        }
	}
}

void vk2dRendererDrawTexture(VK2DTexture tex, float x, float y, float xscale, float yscale, float rot, float originX, float originY, float xInTex, float yInTex, float texWidth, float texHeight) {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
		if (tex != NULL) {
		    // Flush sprite batch if this is a pipeline change or commands at limit
		    const VK2DPipeline pipe = gRenderer->instancedPipe;
            _vk2dRendererFlushBatchIfNeeded(pipe);

		    VK2DDrawCommand command;
		    command.textureIndex = vk2dTextureGetID(tex);
            command.texturePos[0] = xInTex;
            command.texturePos[1] = yInTex;
            command.texturePos[2] = texWidth;
            command.texturePos[3] = texHeight;
            command.rotation = rot;
            command.colour[0] = gRenderer->colourBlend[0];
            command.colour[1] = gRenderer->colourBlend[1];
            command.colour[2] = gRenderer->colourBlend[2];
            command.colour[3] = gRenderer->colourBlend[3];
            command.origin[0] = originX;
            command.origin[1] = originY;
            command.scale[0] = xscale;
            command.scale[1] = yscale;
            command.pos[0] = x;
            command.pos[1] = y;
            _vk2dRendererAddDrawCommand(&command);
		} else {
            vk2dRaise(VK2D_STATUS_BAD_ASSET, "Texture does not exist.");
		}
	}
}

void vk2dRendererDrawPolygon(VK2DPolygon polygon, float x, float y, bool filled, float lineWidth, float xscale, float yscale, float rot, float originX, float originY) {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
        vk2dRendererFlushSpriteBatch();

        if (polygon != NULL) {
			VkDescriptorSet set;
			_vk2dRendererDraw(&set, 1, polygon, filled ? gRenderer->primFillPipe : gRenderer->primLinePipe, x, y,
							  xscale,
							  yscale, rot, originX, originY, lineWidth, 0, 0, 0, 0);
		} else {
            vk2dRaise(VK2D_STATUS_BAD_ASSET, "Polygon does not exist.");
		}
	}
}

void vk2dRendererDrawGeometry(VK2DVertexColour *vertices, int count, float x, float y, bool filled, float lineWidth, float xscale, float yscale, float rot, float originX, float originY) {
    if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
        if (vertices != NULL && count > 0) {
            vk2dRendererFlushSpriteBatch();

            if (count <= gRenderer->limits.maxGeometryVertices) {
                // Copy vertex data to the current descriptor buffer
                VkBuffer buffer;
                VkDeviceSize offset;
                vk2dDescriptorBufferCopyData(gRenderer->descriptorBuffers[gRenderer->currentFrame], vertices,
                                             count * sizeof(VK2DVertexColour), &buffer, &offset);
                struct VK2DBuffer_t buf;
                buf.buf = buffer;
                buf.offset = offset;
                struct VK2DPolygon_t poly;
                poly.vertexCount = count;
                poly.vertices = &buf;
                poly.type = VK2D_VERTEX_TYPE_SHAPE;
                VkDescriptorSet set;
                _vk2dRendererDraw(&set, 1, &poly, filled ? gRenderer->primFillPipe : gRenderer->primLinePipe, x, y,
                                  xscale,
                                  yscale, rot, originX, originY, lineWidth, 0, 0, 0, 0);
                _vk2dRendererResetBoundPointers();
            }
        } else {
            vk2dRaise(VK2D_STATUS_BAD_ASSET, "Vertices does not exist.");
        }
    }
}

void vk2dRendererDrawShadows(VK2DShadowEnvironment shadowEnvironment, vec4 colour, vec2 lightSource) {
    if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
        vk2dRendererFlushSpriteBatch();

        if (shadowEnvironment != NULL && shadowEnvironment->vbo != NULL) {
            _vk2dRendererDrawShadows(shadowEnvironment, colour, lightSource);
            _vk2dRendererResetBoundPointers();
        } else {
            vk2dRaise(VK2D_STATUS_BAD_ASSET, "Shadow environment not prepared.");
        }
    }
}

void vk2dRendererDrawModel(VK2DModel model, float x, float y, float z, float xscale, float yscale, float zscale, float rot, vec3 axis, float originX, float originY, float originZ) {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
		if (model != NULL) {
            vk2dRendererFlushSpriteBatch();

			VkDescriptorSet sets[3];
			sets[1] = gRenderer->modelSamplerSet;
			sets[2] = gRenderer->texArrayDescriptorSet;
			_vk2dRendererDraw3D(sets, 3, model, gRenderer->modelPipe, x, y, z, xscale, yscale, zscale, rot, axis, originX,
								originY, originZ, 1);
		} else {
            vk2dRaise(VK2D_STATUS_BAD_ASSET, "Model does not exist.");
		}
	}
}

void vk2dRendererDrawWireframe(VK2DModel model, float x, float y, float z, float xscale, float yscale, float zscale, float rot, vec3 axis, float originX, float originY, float originZ, float lineWidth) {
	if (vk2dRendererGetPointer() != NULL && !vk2dStatusFatal()) {
		if (model != NULL) {
		    vk2dRendererFlushSpriteBatch();

			VkDescriptorSet sets[3];
			sets[1] = gRenderer->modelSamplerSet;
			sets[2] = model->tex->img->set;
			_vk2dRendererDraw3D(sets, 3, model, gRenderer->wireframePipe, x, y, z, xscale, yscale, zscale, rot, axis, originX,
								originY, originZ, lineWidth);
		} else {
            vk2dRaise(VK2D_STATUS_BAD_ASSET, "Model does not exist.");
		}
	}
}

static void _vk2dRendererFlushPerCamera(VkCommandBuffer buf, int cameraIndex) {
    // Viewport/scissor
    const int cam = cameraIndex; // TODO: Fix this
    VK2DInstancedPushBuffer push = {
            .cameraIndex = cameraIndex
    };
    VkRect2D scissor;
    VkViewport viewport;
    if (gRenderer->target == NULL) {
        viewport.x = gRenderer->cameras[cam].spec.xOnScreen;
        viewport.y = gRenderer->cameras[cam].spec.yOnScreen;
        viewport.width = gRenderer->cameras[cam].spec.wOnScreen;
        viewport.height = gRenderer->cameras[cam].spec.hOnScreen;
        viewport.minDepth = 0;
        viewport.maxDepth = 1;
        scissor.extent.width = gRenderer->cameras[cam].spec.wOnScreen;
        scissor.extent.height = gRenderer->cameras[cam].spec.hOnScreen;
        scissor.offset.x = gRenderer->cameras[cam].spec.xOnScreen;
        scissor.offset.y = gRenderer->cameras[cam].spec.yOnScreen;
    } else {
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = gRenderer->target->img->width;
        viewport.height = gRenderer->target->img->height;
        viewport.minDepth = 0;
        viewport.maxDepth = 1;
        scissor.extent.width = gRenderer->target->img->width;
        scissor.extent.height = gRenderer->target->img->height;
        scissor.offset.x = 0;
        scissor.offset.y = 0;
    }
    vkCmdSetViewport(buf, 0, 1, &viewport);
    vkCmdSetScissor(buf, 0, 1, &scissor);
    vkCmdPushConstants(buf, gRenderer->currentBatchPipeline->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(struct VK2DInstancedPushBuffer), &push);
    vkCmdDraw(buf, 6 * gRenderer->drawCommandCount, 1, 0, 0);
}

void vk2dRendererFlushSpriteBatch() {
    // This function does several things
    //  1. Copies the current sprite batch to the descriptor buffer
    //  2. Reserves space on the descriptor buffer for the compute output
    //  3. Dispatch the compute shader on the compute command buffer
    //  4. Send out the draw command that uses the soon-to-be-filled compute output as vertex input
    if (gRenderer->currentBatchPipeline != NULL && gRenderer->drawCommandCount > 0) {
        // Copy the draw commands into a buffer
        VkBuffer drawCommands, drawInstances;
        VkDeviceSize drawCommandsOffset, drawInstancesOffset;
        vk2dDescriptorBufferCopyData(
                gRenderer->descriptorBuffers[gRenderer->currentFrame],
                gRenderer->drawCommands,
                gRenderer->drawCommandCount * sizeof(struct VK2DDrawCommand),
                &drawCommands,
                &drawCommandsOffset
        );

        // Reserve space for the draw instances
        vk2dDescriptorBufferReserveSpace(
                gRenderer->descriptorBuffers[gRenderer->currentFrame],
                gRenderer->drawCommandCount * sizeof(VK2DDrawInstance),
                &drawInstances,
                &drawInstancesOffset
        );

        // Create descriptor sets
        const uint32_t drawCount = gRenderer->drawCommandCount;
        VkDescriptorSet descriptorSet = vk2dDescConGetSet(gRenderer->descConCompute[gRenderer->currentFrame]);
        VkDescriptorSet vertexShaderSBOSet = vk2dDescConGetSet(gRenderer->descConSBO[gRenderer->currentFrame]);
        VkDescriptorBufferInfo bufferInfos[2] = {
                {
                        .buffer = drawCommands,
                        .offset = drawCommandsOffset,
                        .range = drawCount * sizeof(struct VK2DDrawCommand)
                },
                {
                        .buffer = drawInstances,
                        .offset = drawInstancesOffset,
                        .range = drawCount * sizeof(struct VK2DDrawInstance)
                }
        };
        VkWriteDescriptorSet writes[] = {{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = descriptorSet,
                    .dstBinding = 0,
                    .descriptorCount = 2,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = bufferInfos
            },
            {
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstSet = vertexShaderSBOSet,
                    .dstBinding = 3,
                    .descriptorCount = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                    .pBufferInfo = &bufferInfos[1]
            }
        };
        vkUpdateDescriptorSets(gRenderer->ld->dev, 2, writes, 0, VK_NULL_HANDLE);

        // Queue compute dispatches to the compute command buffer, synchronization will be recorded at the end of the frame
        VkCommandBuffer computeBuf = gRenderer->computeCommandBuffer[gRenderer->scImageIndex];
        VK2DComputePushBuffer push = { .drawCount = drawCount };
        vkCmdPushConstants(computeBuf, gRenderer->spriteBatchPipe->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(VK2DComputePushBuffer), &push);
        vkCmdBindDescriptorSets(computeBuf, VK_PIPELINE_BIND_POINT_COMPUTE, gRenderer->spriteBatchPipe->layout, 0, 1, &descriptorSet, 0, VK_NULL_HANDLE);
        vkCmdDispatch(computeBuf, (drawCount / 64) + 1, 1, 1);

        // Dispatch compute and draw command
        VkCommandBuffer buf = gRenderer->commandBuffer[gRenderer->scImageIndex];
        _vk2dRendererResetBoundPointers();
        vkCmdBindPipeline(buf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk2dPipelineGetPipe(gRenderer->instancedPipe, gRenderer->blendMode));
        VkDescriptorSet sets[] = {
            gRenderer->target != NULL && !gRenderer->enableTextureCameraUBO ? gRenderer->targetUBOSet : gRenderer->uboDescriptorSets[gRenderer->currentFrame],
            gRenderer->samplerSet,
            gRenderer->texArrayDescriptorSet,
            vertexShaderSBOSet
        };
        // These things are the same across every camera, so they are only bound once
        vkCmdBindDescriptorSets(buf, VK_PIPELINE_BIND_POINT_GRAPHICS, gRenderer->instancedPipe->layout, 0, 4, sets, 0, VK_NULL_HANDLE);
        vkCmdSetLineWidth(buf, 1);

        // Draw once per camera
        if (gRenderer->target != VK2D_TARGET_SCREEN && !gRenderer->enableTextureCameraUBO) {
            _vk2dRendererFlushPerCamera(buf, 0);
        } else {
            // Only render to 2D cameras
            for (int i = 0; i < VK2D_MAX_CAMERAS; i++) {
                if (gRenderer->cameras[i].state == VK2D_CAMERA_STATE_NORMAL && gRenderer->cameras[i].spec.type == VK2D_CAMERA_TYPE_DEFAULT && (i == gRenderer->cameraLocked || gRenderer->cameraLocked == VK2D_INVALID_CAMERA)) {
                    _vk2dRendererFlushPerCamera(buf, i);
                }
            }
        }

        // Reset the current batch
        gRenderer->drawCommandCount = 0;
        gRenderer->currentBatchPipeline = NULL;
        gRenderer->currentBatchPipelineID = VK2D_PIPELINE_ID_NONE;
    }
}

static inline float _getHexValue(char c) {
	switch (c) {
		case '1': return 1;
		case '2': return 2;
		case '3': return 3;
		case '4': return 4;
		case '5': return 5;
		case '6': return 6;
		case '7': return 7;
		case '8': return 8;
		case '9': return 9;
		case 'A': return 10;
		case 'B': return 11;
		case 'C': return 12;
		case 'D': return 13;
		case 'E': return 14;
		case 'F': return 15;
		case 'a': return 10;
		case 'b': return 11;
		case 'c': return 12;
		case 'd': return 13;
		case 'e': return 14;
		case 'f': return 15;
		default: return 0;
	}
}

void vk2dColourHex(vec4 dst, const char *hex) {
	if (strlen(hex) == 7 && hex[0] == '#') {
		dst[0] = ((_getHexValue(hex[1]) * 16) + _getHexValue(hex[2])) / 255;
		dst[1] = ((_getHexValue(hex[3]) * 16) + _getHexValue(hex[4])) / 255;
		dst[2] = ((_getHexValue(hex[5]) * 16) + _getHexValue(hex[6])) / 255;
		dst[3] = 1;
	} else {
		dst[0] = dst[1] = dst[2] = dst[3] = 0;
	}
}

void vk2dColourInt(vec4 dst, uint32_t colour) {
	dst[0] = (float)((colour>>24) & 0xFF) / 255.0f;
	dst[1] = (float)((colour>>16) & 0xFF) / 255.0f;
	dst[2] = (float)((colour>>8) & 0xFF) / 255.0f;
	dst[3] = (float)((colour) & 0xFF) / 255.0f;
}

void vk2dColourRGBA(vec4 dst, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
	dst[0] = (float)r / 255.0f;
	dst[1] = (float)g / 255.0f;
	dst[2] = (float)b / 255.0f;
	dst[3] = (float)a / 255.0f;
}

float vk2dRandom(float min, float max) {
	uint32_t r = SDL_GetAtomicInt(&gRNG);
	const int64_t a = 1103515245;
	const int64_t c = 12345;
	const int64_t m = 2147483648;
	r = (a * r + c) % m;
	SDL_SetAtomicInt(&gRNG, r);
	const int64_t resolution = 5000;
	float n = (float)(r % (resolution + 1));
	return min + ((max - min) * (n / (float)resolution));
}
