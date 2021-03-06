#include <functional>
#include <random>

#include <OS/Interfaces/IApp.h>
#include <OS/Interfaces/ICameraController.h>
#include <OS/Interfaces/IInput.h>
#include <OS/Interfaces/IProfiler.h>
#include <OS/Interfaces/IThread.h>

#include <OS/Core/ThreadSystem.h>
#include <OS/Math/MathTypes.h>

#include <Renderer/IRenderer.h>
#include <Renderer/IResourceLoader.h>

#include <UI/AppUI.h>

constexpr size_t sphereCount = 10240;
constexpr float speed = 500.0f;

struct UniformBlock {
  mat4 mProjectView;
  mat4 mWorld;
  vec4 mColor;

  // Point Light Information
  vec3 mLightPosition;
  vec3 mLightColor;
};

Renderer *pRenderer = nullptr;
constexpr uint32_t gImageCount = 3;

constexpr int gSphereResolution = 10;
constexpr float gSphereDiameter = 1.0f;

Queue *pGraphicsQueue = nullptr;
CmdPool *pCmdPools[gImageCount] = {nullptr};
Cmd *pCmds[gImageCount] = {nullptr};

SwapChain *pSwapChain = nullptr;
RenderTarget *pDepthBuffer = nullptr;
Fence *pRenderCompleteFences[gImageCount] = {nullptr};
Semaphore *pImageAcquiredSemaphore = nullptr;
Semaphore *pRenderCompleteSemaphores[gImageCount] = {nullptr};

Shader *pShader = nullptr;
Buffer *pVertexBuffer = nullptr;
Pipeline *pPipeline = nullptr;

uint32_t gFrameIndex = 0;
ProfileToken gGpuProfileToken = PROFILE_INVALID_TOKEN;

GuiComponent *pGuiWindow;

UIApp gAppUI;

RootSignature *pRootSignature = nullptr;

ThreadSystem *pThreadSystem{nullptr};

DescriptorSet *pDescriptorSetUniforms[sphereCount] = {nullptr};
vec4 spherePos[sphereCount];
vec4 colors[sphereCount];
UniformBlock gUniformData[sphereCount];
Buffer *pProjViewUniformBuffer[gImageCount][sphereCount] = {nullptr};

ICameraController *pCameraController = {};

bool bToggleVSync = false;

TextDrawDesc gFrameTimeDraw = TextDrawDesc(0, 0xff00ffff, 18);

int gNumberOfSpherePoints;

vec4 RandomInsideUnitSphere() {
  static std::default_random_engine generator;
  static std::uniform_real_distribution<float> distribution(0, 1);
  static auto r = std::bind(distribution, generator);

  float x, y, z, d;
  do {
    x = r() * 2.0f - 1.0f;
    y = r() * 2.0f - 1.0f;
    z = r() * 2.0f - 1.0f;

    d = x * x + y * y + z * z;
  } while (d > 1.0);

  return {x, y, z, 1.0f};
}

vec4 RandomColor() {
  static std::default_random_engine generator;
  static std::uniform_real_distribution<float> distribution(0, 1);
  static auto r = std::bind(distribution, generator);

  return {r(), r(), r(), 1.0f};
}

struct updateSphereData {
  float deltaTime;
};

void updateSphere(void *pData, uintptr_t i) {
  auto pSphereData = static_cast<updateSphereData *>(pData);
  float z = spherePos[i].getZ();
  if (z < 0) {
    spherePos[i] = RandomInsideUnitSphere() * 500;
    z = spherePos[i].getZ() + 1000;
    colors[i] = RandomColor();
  } else {
    z -= pSphereData->deltaTime * speed;
  }
  spherePos[i].setZ(z);

  gUniformData[i].mWorld = mat4::translation(
      {spherePos[i].getX(), spherePos[i].getY(), spherePos[i].getZ()});
  gUniformData[i].mColor = colors[i];
}

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
    addResource(&sphereVbDesc, nullptr);

    ShaderLoadDesc shaderDesc = {};
    shaderDesc.mStages[0] = {"basic.vert", nullptr, 0};
    shaderDesc.mStages[1] = {"basic.frag", nullptr, 0};
    addShader(pRenderer, &shaderDesc, &pShader);

    Shader *shaders[] = {pShader};
    RootSignatureDesc rootDesc = {};
    rootDesc.mStaticSamplerCount = 0;
    rootDesc.mShaderCount = 1;
    rootDesc.ppShaders = shaders;
    addRootSignature(pRenderer, &rootDesc, &pRootSignature);

    DescriptorSetDesc desc = {pRootSignature, DESCRIPTOR_UPDATE_FREQ_PER_FRAME,
                              gImageCount};
    for (auto i = 0; i < sphereCount; i++) {
      addDescriptorSet(pRenderer, &desc, &pDescriptorSetUniforms[i]);
    }

    BufferLoadDesc ubDesc = {};
    ubDesc.mDesc.mDescriptors = DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    ubDesc.mDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
    ubDesc.mDesc.mSize = sizeof(UniformBlock);
    ubDesc.mDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
    ubDesc.pData = nullptr;
    for (uint32_t i = 0; i < gImageCount; ++i) {
      for (auto j = 0; j < sphereCount; j++) {
        ubDesc.ppBuffer = &pProjViewUniformBuffer[i][j];
        addResource(&ubDesc, nullptr);
      }
    }

    if (!gAppUI.Init(pRenderer))
      return false;

    gAppUI.LoadFont("TitilliumText/TitilliumText-Bold.otf");

    CameraMotionParameters cmp{160.0f, 600.0f, 200.0f};
    vec3 lookAt{0, 0, 500};
    vec3 camPos{0.0f, 0.0f, 0.0f};

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
    size_t size = sizeof(UniformBlock);
    for (auto i = 0; i < sphereCount; i++) {
      for (uint32_t j = 0; j < gImageCount; ++j) {
        DescriptorData params[1] = {};
        params[0].pName = "uniformBlock";
        params[0].ppBuffers = &pProjViewUniformBuffer[j][i];
        updateDescriptorSet(pRenderer, j, pDescriptorSetUniforms[i], 1, params);
      }
    }

    mat4 viewMat = pCameraController->getViewMatrix();
    const float aspectInverse =
        (float)mSettings.mHeight / (float)mSettings.mWidth;
    const float horizontal_fov = 120.0f * PI / 180.0f;
    mat4 projMat =
        mat4::perspective(horizontal_fov, aspectInverse, 1000.0f, 0.3f);
    auto projView = projMat * viewMat;

    for (int i = 0; i < sphereCount; i++) {
      gUniformData[i].mProjectView = projView;
      gUniformData[i].mWorld = mat4::translation(
          {spherePos[i].getX(), spherePos[i].getY(), spherePos[i].getZ()});
      gUniformData[i].mColor = colors[i];
      // point light parameters
      gUniformData[i].mLightPosition = vec3(0, 0, 0);
      gUniformData[i].mLightColor = vec3(0.9f, 0.9f, 0.7f); // Pale Yellow
    }

    initThreadSystem(&pThreadSystem);

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
      for (auto j = 0; j < sphereCount; j++) {
        removeResource(pProjViewUniformBuffer[i][j]);
      }
    }
    for (auto i = 0; i < sphereCount; i++) {
      removeDescriptorSet(pRenderer, pDescriptorSetUniforms[i]);
    }

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

    shutdownThreadSystem(pThreadSystem);
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

    return pSwapChain != nullptr;
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

    return pDepthBuffer != nullptr;
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

    /************************************************************************/
    // Scene Update
    /************************************************************************/
    // update camera with time

    PROFILER_SET_CPU_SCOPE("Spheres", "Update position", 0xFFE8E8);
    {
      updateSphereData data{deltaTime};

      addThreadSystemRangeTask(pThreadSystem, updateSphere, &data, sphereCount);
      waitThreadSystemIdle(pThreadSystem);
    }
    gAppUI.Update(deltaTime);
  }

  virtual void Draw() override {
    uint32_t swapchainImageIndex;
    acquireNextImage(pRenderer, pSwapChain, pImageAcquiredSemaphore, nullptr,
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
    for (auto i = 0; i < sphereCount; i++) {
      BufferUpdateDesc viewProjCbv = {pProjViewUniformBuffer[gFrameIndex][i]};
      beginUpdateResource(&viewProjCbv);
      viewProjCbv.mSize = sizeof(UniformBlock) * sphereCount;
      *(UniformBlock *)viewProjCbv.pMappedData = gUniformData[i];
      endUpdateResource(&viewProjCbv, nullptr);
    }
    // Reset cmd pool for this frame
    resetCmdPool(pRenderer, pCmdPools[gFrameIndex]);

    Cmd *cmd = pCmds[gFrameIndex];
    beginCmd(cmd);

    cmdBeginGpuFrameProfile(cmd, gGpuProfileToken);

    RenderTargetBarrier barriers[] = {
        {pRenderTarget, RESOURCE_STATE_PRESENT, RESOURCE_STATE_RENDER_TARGET},
    };
    cmdResourceBarrier(cmd, 0, nullptr, 0, nullptr, 1, barriers);

    // simply record the screen cleaning command
    LoadActionsDesc loadActions = {};
    loadActions.mLoadActionsColor[0] = LOAD_ACTION_CLEAR;
    loadActions.mLoadActionDepth = LOAD_ACTION_CLEAR;
    loadActions.mClearDepth.depth = 0.0f;
    loadActions.mClearDepth.stencil = 0;
    cmdBindRenderTargets(cmd, 1, &pRenderTarget, pDepthBuffer, &loadActions,
                         nullptr, nullptr, -1, -1);

    cmdSetViewport(cmd, 0.0f, 0.0f, (float)pRenderTarget->mWidth,
                   (float)pRenderTarget->mHeight, 0.0f, 1.0f);
    cmdSetScissor(cmd, 0, 0, pRenderTarget->mWidth, pRenderTarget->mHeight);

    const uint32_t sphereVbStride = sizeof(float) * 6;

    ////// draw planets
    cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Draw Spheres");
    {
      for (auto i = 0; i < sphereCount; i++) {
        cmdBindPipeline(cmd, pPipeline);
        cmdBindDescriptorSet(cmd, gFrameIndex, pDescriptorSetUniforms[i]);
        cmdBindVertexBuffer(cmd, 1, &pVertexBuffer, &sphereVbStride, nullptr);
        cmdDraw(cmd, gNumberOfSpherePoints / 6, 0);
      }
    }
    cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);

    loadActions = {};
    loadActions.mLoadActionsColor[0] = LOAD_ACTION_LOAD;
    cmdBindRenderTargets(cmd, 1, &pRenderTarget, nullptr, &loadActions, nullptr,
                         nullptr, -1, -1);
    cmdBeginGpuTimestampQuery(cmd, gGpuProfileToken, "Draw UI");
    {
      const float txtIndent = 8.f;
      float2 txtSizePx =
          cmdDrawCpuProfile(cmd, float2(txtIndent, 15.f), &gFrameTimeDraw);
      cmdDrawGpuProfile(cmd, float2(txtIndent, txtSizePx.y + 30.f),
                        gGpuProfileToken, &gFrameTimeDraw);

      cmdDrawProfilerUI();

      gAppUI.Gui(pGuiWindow);
      gAppUI.Draw(cmd);
      cmdBindRenderTargets(cmd, 0, nullptr, nullptr, nullptr, nullptr, nullptr,
                           -1, -1);
      cmdEndGpuTimestampQuery(cmd, gGpuProfileToken);

      barriers[0] = {pRenderTarget, RESOURCE_STATE_RENDER_TARGET,
                     RESOURCE_STATE_PRESENT};
      cmdResourceBarrier(cmd, 0, nullptr, 0, nullptr, 1, barriers);
    }
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
