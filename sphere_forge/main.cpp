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

constexpr int gSphereResolution = 10;
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

TextDrawDesc gFrameTimeDraw = TextDrawDesc(0, 0xff00ffff, 18);

vec4 spherePos = {0, 0, 0, 0};
constexpr float speed = 500.0f;

UniformBlock gUniformData = {};

int gNumberOfSpherePoints;

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

    float *pSpherePoints;
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
                              gImageCount};
    addDescriptorSet(pRenderer, &desc, &pDescriptorSetUniforms);

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
    vec3 lookAt{0.0f, 0.0f, 1000.0f};
    vec3 camPos{vec3(0)}; 

    pCameraController = createFpsCameraController(camPos, lookAt);

    pCameraController->setMotionParameters(cmp);

    if (!initInputSystem(pWindow))
      return false;

    // Initialize microprofiler and it's UI.
    initProfiler();

    // Gpu profiler can only be added after initProfile.
    gGpuProfileToken = addGpuProfiler(pRenderer, pGraphicsQueue, "Graphics");

    GuiDesc guiDesc = {};
    guiDesc.mStartPosition =
        vec2(mSettings.mWidth * 0.01f, mSettings.mHeight * 0.2f);
    pGuiWindow = gAppUI.AddGuiComponent(GetName(), &guiDesc);

    pGuiWindow->AddWidget(
        CheckboxWidget("Toggle VSync\t\t\t\t\t", &bToggleVSync));

    // App Actions
    InputActionDesc actionDesc = {InputBindings::BUTTON_DUMP,
                                  [](InputActionContext *ctx) {
                                    dumpProfileData(
                                        ((Renderer *)ctx->pUserData),
                                        ((Renderer *)ctx->pUserData)->pName);
                                    return true;
                                  },
                                  pRenderer};
    addInputAction(&actionDesc);
    actionDesc = {InputBindings::BUTTON_FULLSCREEN,
                  [](InputActionContext *ctx) {
                    toggleFullscreen(((IApp *)ctx->pUserData)->pWindow);
                    return true;
                  },
                  this};
    addInputAction(&actionDesc);
    actionDesc = {InputBindings::BUTTON_EXIT, [](InputActionContext *ctx) {
                    requestShutdown();
                    return true;
                  }};
    addInputAction(&actionDesc);
    actionDesc = {InputBindings::BUTTON_ANY,
                  [](InputActionContext *ctx) {
                    bool capture = gAppUI.OnButton(ctx->mBinding, ctx->mBool,
                                                   ctx->pPosition);
                    setEnableCaptureInput(
                        capture && INPUT_ACTION_PHASE_CANCELED != ctx->mPhase);
                    return true;
                  },
                  this};
    addInputAction(&actionDesc);

    waitForAllResourceLoads();

    // Need to free memory;
    tf_free(pSpherePoints);

    for (uint32_t i = 0; i < gImageCount; ++i) {
      DescriptorData params[1] = {};
      params[0].pName = "uniformBlock";
      params[0].ppBuffers = &pProjViewUniformBuffer[i];
      updateDescriptorSet(pRenderer, i, pDescriptorSetUniforms, 1, params);
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

    for (uint32_t i = 0; i < gImageCount; ++i) {
      removeResource(pProjViewUniformBuffer[i]);
    }

    removeDescriptorSet(pRenderer, pDescriptorSetUniforms);

    removeResource(pVertexBuffer);

    removeShader(pRenderer, pShader);

    removeRootSignature(pRenderer, pRootSignature);

    for (uint32_t i = 0; i < gImageCount; ++i) {
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

  bool addSwapChain() {
    SwapChainDesc swapChainDesc = {};
    swapChainDesc.mWindowHandle = pWindow->handle;
    swapChainDesc.mPresentQueueCount = 1;
    swapChainDesc.ppPresentQueues = &pGraphicsQueue;
    swapChainDesc.mWidth = mSettings.mWidth;
    swapChainDesc.mHeight = mSettings.mHeight;
    swapChainDesc.mImageCount = gImageCount;
    swapChainDesc.mColorFormat = getRecommendedSwapchainFormat(true);
    swapChainDesc.mEnableVsync = mSettings.mDefaultVSyncEnabled;
    ::addSwapChain(pRenderer, &swapChainDesc, &pSwapChain);

    return pSwapChain != NULL;
  }

  bool addDepthBuffer() {
    // Add depth buffer
    RenderTargetDesc depthRT = {};
    depthRT.mArraySize = 1;
    depthRT.mClearValue.depth = 0.0f;
    depthRT.mClearValue.stencil = 0;
    depthRT.mDepth = 1;
    depthRT.mFormat = TinyImageFormat_D32_SFLOAT;
    depthRT.mHeight = mSettings.mHeight;
    depthRT.mSampleCount = SAMPLE_COUNT_1;
    depthRT.mSampleQuality = 0;
    depthRT.mWidth = mSettings.mWidth;
    depthRT.mFlags = TEXTURE_CREATION_FLAG_ON_TILE;
    addRenderTarget(pRenderer, &depthRT, &pDepthBuffer);

    return pDepthBuffer != NULL;
  }

  virtual bool Load() override {
    if (!addSwapChain())
      return false;

    if (!addDepthBuffer())
      return false;

    if (!gAppUI.Load(pSwapChain->ppRenderTargets, 1))
      return false;

    loadProfilerUI(&gAppUI, mSettings.mWidth, mSettings.mHeight);

    // layout and pipeline for sphere draw
    VertexLayout vertexLayout = {};
    vertexLayout.mAttribCount = 2;
    vertexLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
    vertexLayout.mAttribs[0].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
    vertexLayout.mAttribs[0].mBinding = 0;
    vertexLayout.mAttribs[0].mLocation = 0;
    vertexLayout.mAttribs[0].mOffset = 0;
    vertexLayout.mAttribs[1].mSemantic = SEMANTIC_NORMAL;
    vertexLayout.mAttribs[1].mFormat = TinyImageFormat_R32G32B32_SFLOAT;
    vertexLayout.mAttribs[1].mBinding = 0;
    vertexLayout.mAttribs[1].mLocation = 1;
    vertexLayout.mAttribs[1].mOffset = 3 * sizeof(float);

    RasterizerStateDesc rasterizerStateDesc = {};
    rasterizerStateDesc.mCullMode = CULL_MODE_FRONT;

    DepthStateDesc depthStateDesc = {};
    depthStateDesc.mDepthTest = true;
    depthStateDesc.mDepthWrite = true;
    depthStateDesc.mDepthFunc = CMP_GEQUAL;

    PipelineDesc desc = {};
    desc.mType = PIPELINE_TYPE_GRAPHICS;
    GraphicsPipelineDesc &pipelineSettings = desc.mGraphicsDesc;
    pipelineSettings.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
    pipelineSettings.mRenderTargetCount = 1;
    pipelineSettings.pDepthState = &depthStateDesc;
    pipelineSettings.pColorFormats = &pSwapChain->ppRenderTargets[0]->mFormat;
    pipelineSettings.mSampleCount =
        pSwapChain->ppRenderTargets[0]->mSampleCount;
    pipelineSettings.mSampleQuality =
        pSwapChain->ppRenderTargets[0]->mSampleQuality;
    pipelineSettings.mDepthStencilFormat = pDepthBuffer->mFormat;
    pipelineSettings.pRootSignature = pRootSignature;
    pipelineSettings.pShaderProgram = pShader;
    pipelineSettings.pVertexLayout = &vertexLayout;
    pipelineSettings.pRasterizerState = &rasterizerStateDesc;
    addPipeline(pRenderer, &desc, &pPipeline);

    return true;
  }
  virtual void Unload() override {
    waitQueueIdle(pGraphicsQueue);

    unloadProfilerUI();
    gAppUI.Unload();

    removePipeline(pRenderer, pPipeline);
    removeSwapChain(pRenderer, pSwapChain);
    removeRenderTarget(pRenderer, pDepthBuffer);
  }

  virtual void Update(float deltaTime) override {
    if (pSwapChain->mEnableVsync != bToggleVSync) {
      waitQueueIdle(pGraphicsQueue);
      gFrameIndex = 0;
      ::toggleVSync(pRenderer, &pSwapChain);
    }

    updateInputSystem(mSettings.mWidth, mSettings.mHeight);

    pCameraController->update(deltaTime);

    spherePos.setZ(spherePos.getZ() - deltaTime * speed);
    if (spherePos.getZ() < 0) {
      spherePos.setZ(1000);
    }
    /************************************************************************/
    // Scene Update
    /************************************************************************/
    // update camera with time
    mat4 viewMat = pCameraController->getViewMatrix();

    const float aspectInverse =
        (float)mSettings.mHeight / (float)mSettings.mWidth;
    const float horizontal_fov = 60.0f * PI / 180.0f;

    mat4 projMat =
        mat4::perspective(horizontal_fov, aspectInverse, 0.3f, 1000.0f);
    gUniformData.mProjectView = projMat * viewMat;
    gUniformData.mWorld =
        mat4::translation({spherePos[0], spherePos[1], spherePos[2]});
    gUniformData.mColor = {0.5f, 0.5f, 1.0f, 1.0f};
    // point light parameters
    gUniformData.mLightPosition = vec3(0, 0, 0);
    gUniformData.mLightColor = vec3(0.9f, 0.9f, 0.7f); // Pale Yellow

    gAppUI.Update(deltaTime);
  }

  virtual void Draw() override {
    uint32_t swapchainImageIndex;
    acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, NULL,
                     &swapchainImageIndex);

    RenderTarget *pRenderTarget =
        pSwapChain->ppRenderTargets[swapchainImageIndex];
    Semaphore *pRenderCompleteSemaphore =
        pRenderCompleteSemaphores[gFrameIndex];
    Fence *pRenderCompleteFence = pRenderCompleteFences[gFrameIndex];

    // Stall if CPU is running "Swap Chain Buffer Count" frames ahead of GPU
    FenceStatus fenceStatus;
    getFenceStatus(pRenderer, pRenderCompleteFence, &fenceStatus);
    if (fenceStatus == FENCE_STATUS_INCOMPLETE)
      waitForFences(pRenderer, 1, &pRenderCompleteFence);

    // Update uniform buffers
    BufferUpdateDesc viewProjCbv = {pProjViewUniformBuffer[gFrameIndex]};
    beginUpdateResource(&viewProjCbv);
    *(UniformBlock *)viewProjCbv.pMappedData = gUniformData;
    endUpdateResource(&viewProjCbv, NULL);

    // Reset cmd pool for this frame
    resetCmdPool(pRenderer, pCmdPools[gFrameIndex]);

    Cmd *cmd = pCmds[gFrameIndex];
    beginCmd(cmd);

    cmdBeginGpuFrameProfile(cmd, gGpuProfileToken);

    RenderTargetBarrier barriers[] = {
        {pRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET},
    };
    cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);

    // simply record the screen cleaning command
    LoadActionsDesc loadActions = {};
    loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
    loadActions.mLoadActionDepth = LOAD_ACTION_CLEAR;
    loadActions.mClearDepth.depth = 0.0f;
    loadActions.mClearDepth.stencil = 0;
    cmdBindRenderTargets(cmd, 1, &pRenderTarget, pDepthBuffer, &loadActions,
                         NULL, NULL, -1, -1);
    cmdSetViewport(cmd, 0.0f, 0.0f, (float)pRenderTarget->mWidth,
                   (float)pRenderTarget->mHeight, 0.0f, 1.0f);
    cmdSetScissor(cmd, 0, 0, pRenderTarget->mWidth, pRenderTarget->mHeight);

    const uint32_t sphereVbStride = sizeof(float) * 6;
    const uint32_t skyboxVbStride = sizeof(float) * 4;

    ////// draw planets
    cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Draw Spheres");
    cmdBindPipeline(cmd, pPipeline);
    cmdBindDescriptorSet(cmd, gFrameIndex, pDescriptorSetUniforms);
    cmdBindVertexBuffer(cmd, 1, &pVertexBuffer, &sphereVbStride, NULL);
    cmdDraw(cmd, gNumberOfSpherePoints / 6, 0);
    cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);

    loadActions = {};
    loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
    cmdBindRenderTargets(cmd, 1, &pRenderTarget, NULL, &loadActions, NULL, NULL,
                         -1, -1);
    cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Draw UI");

    const float txtIndent = 8.f;
    float2 txtSizePx =
        cmdDrawCpuProfile(cmd, float2(txtIndent, 15.f), &gFrameTimeDraw);
    cmdDrawGpuProfile(cmd, float2(txtIndent, txtSizePx.y + 30.f),
                      gGpuProfileToken, &gFrameTimeDraw);

    cmdDrawProfilerUI();

    gAppUI.Gui(pGuiWindow);
    gAppUI.Draw(cmd);
    cmdBindRenderTargets(cmd, 0, NULL, NULL, NULL, NULL, NULL, -1, -1);
    cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);

    barriers[0] = {pRenderTarget, RESOURCE_STATE_RENDER_TARGET,
                   RESOURCE_STATE_PRESENT};
    cmdResourceBarrier(cmd, 0, NULL, 0, NULL, 1, barriers);

    cmdEndGpuFrameProfile(cmd, gGpuProfileToken);
    endCmd(cmd);

    QueueSubmitDesc submitDesc = {};
    submitDesc.mCmdCount = 1;
    submitDesc.mSignalSemaphoreCount = 1;
    submitDesc.mWaitSemaphoreCount = 1;
    submitDesc.ppCmds = &cmd;
    submitDesc.ppSignalSemaphores = &pRenderCompleteSemaphore;
    submitDesc.ppWaitSemaphores = &pImageAcquiredSemaphore;
    submitDesc.pSignalFence = pRenderCompleteFence;
    queueSubmit(pGraphicsQueue, &submitDesc);
    QueuePresentDesc presentDesc = {};
    presentDesc.mIndex = swapchainImageIndex;
    presentDesc.mWaitSemaphoreCount = 1;
    presentDesc.pSwapChain = pSwapChain;
    presentDesc.ppWaitSemaphores = &pRenderCompleteSemaphore;
    presentDesc.mSubmitDone = true;
    queuePresent(pGraphicsQueue, &presentDesc);
    flipProfiler();

    gFrameIndex = (gFrameIndex + 1) % gImageCount;
  }

  virtual const char *GetName() override { return "Sphere Test"; }
};

DEFINE_APPLICATION_MAIN(App)
