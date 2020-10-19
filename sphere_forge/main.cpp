#include <OS/Interfaces/IApp.h>
#include <OS/Interfaces/ICameraController.h>
#include <OS/Interfaces/IInput.h>
#include <OS/Interfaces/IProfiler.h>
#include <OS/Math/MathTypes.h>
#include <Renderer/IRenderer.h>
#include <Renderer/IResourceLoader.h>
#include <UI/AppUI.h>

struct UniformBlock {
  mat4 mProjectView;
  mat4 mWorld;
  vec4 mColor;

  // Point Light Information
  vec3 mLightPosition;
  vec3 mLightColor;
};

Renderer *pRenderer = NULL;
constexpr size_t gImageCount = 3;

constexpr int gSphereResolution = 30;
constexpr float gSphereDiameter = 1.0f;

Queue *pGraphicsQueue = NULL;
CmdPool *pCmdPools[gImageCount] = {NULL};
Cmd *pCmds[gImageCount] = {NULL};

SwapChain *pSwapChain = NULL;
RenderTarget *pDepthBuffer = NULL;
Fence *pRenderCompleteFences[gImageCount] = {NULL};
Semaphore *pImageAcquiredSemaphore = NULL;
Semaphore *pRenderCompleteSemaphores[gImageCount] = {NULL};

Shader *pShader = NULL;
Buffer *pVertexBuffer = NULL;
Pipeline *pPipeline = NULL;

Buffer *pProjViewUniformBuffer[gImageCount] = {NULL};

uint32_t gFrameIndex = 0;
ProfileToken gGpuProfileToken = PROFILE_INVALID_TOKEN;

GuiComponent *pGuiWindow;

UIApp gAppUI;

RootSignature *pRootSignature = NULL;
DescriptorSet *pDescriptorSetUniforms = {NULL};

ICameraController *pCameraController = {};

bool bToggleVSync = false;

class App : public IApp {
  virtual bool Init() {
    // FILE PATHS
    fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_SHADER_SOURCES,
                            "Shaders");
    fsSetPathForResourceDir(pSystemFileIO, RM_DEBUG, RD_SHADER_BINARIES,
                            "CompiledShaders");
    fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_GPU_CONFIG, "GPUCfg");
    fsSetPathForResourceDir(pSystemFileIO, RM_CONTENT, RD_FONTS, "Fonts");

    // window and renderer setup
    RendererDesc settings = {0};
    initRenderer(GetName(), &settings, &pRenderer);
    // check for init success
    if (!pRenderer)
      return false;

    QueueDesc queueDesc = {};
    queueDesc.mType = QUEUE_TYPE_GRAPHICS;
    queueDesc.mFlag = QUEUE_FLAG_INIT_MICROPROFILE;
    addQueue(pRenderer, &queueDesc, &pGraphicsQueue);

    for (uint32_t i = 0; i < gImageCount; ++i) {
      CmdPoolDesc cmdPoolDesc = {};
      cmdPoolDesc.pQueue = pGraphicsQueue;
      addCmdPool(pRenderer, &cmdPoolDesc, &pCmdPools[i]);
      CmdDesc cmdDesc = {};
      cmdDesc.pPool = pCmdPools[i];
      addCmd(pRenderer, &cmdDesc, &pCmds[i]);

      addFence(pRenderer, &pRenderCompleteFences[i]);
      addSemaphore(pRenderer, &pRenderCompleteSemaphores[i]);
    }
    addSemaphore(pRenderer, &pImageAcquiredSemaphore);

    initResourceLoaderInterface(pRenderer);

    ShaderLoadDesc shaderDesc = {};
    shaderDesc.mStages[0] = {"basic.vert", NULL, 0};
    shaderDesc.mStages[1] = {"basic.frag", NULL, 0};

    addShader(pRenderer, &shaderDesc, &pShader);

    Shader *shaders[] = {pShader};
    RootSignatureDesc rootDesc = {};
    rootDesc.mStaticSamplerCount = 0;
    rootDesc.mShaderCount = 1;
    rootDesc.ppShaders = shaders;
    addRootSignature(pRenderer, &rootDesc, &pRootSignature);

    DescriptorSetDesc desc = {pRootSignature, DESCRIPTOR_UPDATE_FREQ_PER_FRAME,
                              gImageCount * 2};
    addDescriptorSet(pRenderer, &desc, &pDescriptorSetUniforms);

    float *pSpherePoints;
    int gNumberOfSpherePoints;
    generateSpherePoints(&pSpherePoints, &gNumberOfSpherePoints,
                         gSphereResolution, gSphereDiameter);

    uint64_t sphereDataSize = gNumberOfSpherePoints * sizeof(float);
    BufferLoadDesc sphereVbDesc = {};
    sphereVbDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_VERTEX_BUFFER;
    sphereVbDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_GPU_ONLY;
    sphereVbDesc.mDesc.mSize = sphereDataSize;
    sphereVbDesc.pData = pSpherePoints;
    sphereVbDesc.ppBuffer = &pVertexBuffer;
    addResource(&sphereVbDesc, NULL);

    BufferLoadDesc ubDesc = {};
    ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
    ubDesc.mDesc.mSize = sizeof(UniformBlock);
    ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
    ubDesc.pData = NULL;
    for (uint32_t i = 0; i < gImageCount; ++i) {
      ubDesc.ppBuffer = &pProjViewUniformBuffer[i];
      addResource(&ubDesc, NULL);
    }

    if (!gAppUI.Init(pRenderer))
      return false;

    gAppUI.LoadFont("TitilliumText/TitilliumText-Bold.otf");

    CameraMotionParameters cmp{160.0f, 600.0f, 200.0f};
    vec3 camPos{48.0f, 48.0f, 20.0f};
    vec3 lookAt{vec3(0)};

    pCameraController = createFpsCameraController(camPos, lookAt);

    pCameraController->setMotionParameters(cmp);

    if (!initInputSystem(pWindow))
      return false;

	// Initialize microprofiler and it's UI.
	initProfiler();

	// Gpu profiler can only be added after initProfile.
	gGpuProfileToken = addGpuProfiler(pRenderer, pGraphicsQueue, "Graphics");

	GuiDesc guiDesc = {};
	guiDesc.mStartPosition = vec2(mSettings.mWidth * 0.01f, mSettings.mHeight * 0.2f);
	pGuiWindow = gAppUI.AddGuiComponent(GetName(), &guiDesc);

	pGuiWindow->AddWidget(CheckboxWidget("Toggle VSync\t\t\t\t\t", &bToggleVSync));

	waitForAllResourceLoads();

	// Need to free memory;
	tf_free(pSpherePoints);

	for (uint32_t i = 0; i < gImageCount; ++i)
	{
		DescriptorData params[1] = {};
		params[0].pName = "uniformBlock";
		params[0].ppBuffers = &pProjViewUniformBuffer[i];
		updateDescriptorSet(pRenderer, i * 2 + 1, pDescriptorSetUniforms, 1, params);
	}

    return true;
  }

  virtual void Exit() {
	  waitQueueIdle(pGraphicsQueue);

	  exitInputSystem();

	  destroyCameraController(pCameraController);

	  gAppUI.Exit();

	  // Exit profile
	  exitProfiler();

	  for (uint32_t i = 0; i < gImageCount; ++i)
	  {
		  removeResource(pProjViewUniformBuffer[i]);
	  }

	  removeDescriptorSet(pRenderer, pDescriptorSetUniforms);

	  removeResource(pVertexBuffer);

	  removeShader(pRenderer, pShader);

	  removeRootSignature(pRenderer, pRootSignature);

	  for (uint32_t i = 0; i < gImageCount; ++i)
	  {
		  removeFence(pRenderer, pRenderCompleteFences[i]);
		  removeSemaphore(pRenderer, pRenderCompleteSemaphores[i]);

		  removeCmd(pRenderer, pCmds[i]);
		  removeCmdPool(pRenderer, pCmdPools[i]);
	  }
	  removeSemaphore(pRenderer, pImageAcquiredSemaphore);

	  exitResourceLoaderInterface(pRenderer);
	  removeQueue(pRenderer, pGraphicsQueue);
	  removeRenderer(pRenderer);
  }

  virtual bool Load() override { return true; }
  virtual void Unload() override {}

  virtual void Update(float deltaTime) override {}
  virtual void Draw() override {}

  virtual const char *GetName() override { return "Sphere Test"; }
};

DEFINE_APPLICATION_MAIN(App)
