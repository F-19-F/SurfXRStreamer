#include "FrameRender.h"
#include "surfxr_server/Logger.h"
#include "surfxr_server/Utils.h"
#include "surfxr_server/bindings.h"

extern uint64_t g_DriverTestMode;

using namespace d3d_render_utils;

FrameRender::FrameRender(std::shared_ptr<CD3DRender> pD3DRender)
    : m_pD3DRender(pD3DRender) {
    // Set safe defaults for tangents and eye-to-HMD
    HmdMatrix_SetIdentity(&m_eyeToHead[0]);
    HmdMatrix_SetIdentity(&m_eyeToHead[1]);
    m_viewProj[0] = { -1.0f, 1.0f, 1.0f, -1.0f };
    m_viewProj[1] = { -1.0f, 1.0f, 1.0f, -1.0f };

    FrameRender::SetGpuPriority(m_pD3DRender->GetDevice());
}

FrameRender::~FrameRender() { }

bool FrameRender::Startup() {
    if (m_pStagingTexture) {
        return true;
    }

    //
    // Create staging texture
    // This is input texture of Video Encoder and is render target of both eyes.
    //

    D3D11_TEXTURE2D_DESC compositionTextureDesc;
    ZeroMemory(&compositionTextureDesc, sizeof(compositionTextureDesc));
    compositionTextureDesc.Width = Settings_Instance()->m_renderWidth;
    compositionTextureDesc.Height = Settings_Instance()->m_renderHeight;
    compositionTextureDesc.Format = Settings_Instance()->m_enableHdr
        ? DXGI_FORMAT_R16G16B16A16_FLOAT
        : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    compositionTextureDesc.MipLevels = 1;
    compositionTextureDesc.ArraySize = 1;
    compositionTextureDesc.SampleDesc.Count = 1;
    compositionTextureDesc.Usage = D3D11_USAGE_DEFAULT;
    compositionTextureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> compositionTexture;

    if (FAILED(m_pD3DRender->GetDevice()->CreateTexture2D(
            &compositionTextureDesc, NULL, &compositionTexture
        ))) {
        Error("Failed to create staging texture!\n");
        return false;
    }

    HRESULT hr = m_pD3DRender->GetDevice()->CreateRenderTargetView(
        compositionTexture.Get(), NULL, &m_pRenderTargetView
    );
    if (FAILED(hr)) {
        Error("CreateRenderTargetView %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    D3D11_DEPTH_STENCIL_DESC depthStencilDesc;
    depthStencilDesc.DepthEnable = FALSE;
    depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    depthStencilDesc.StencilEnable = FALSE;
    depthStencilDesc.StencilReadMask = 0xFF;
    depthStencilDesc.StencilWriteMask = 0xFF;

    // Stencil operations if pixel is front-facing
    depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_INCR;
    depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

    // Stencil operations if pixel is back-facing
    depthStencilDesc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
    depthStencilDesc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_DECR;
    depthStencilDesc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
    depthStencilDesc.BackFace.StencilFunc = D3D11_COMPARISON_ALWAYS;

    m_pD3DRender->GetDevice()->CreateDepthStencilState(&depthStencilDesc, &m_depthStencilState);

    // Left eye viewport

    m_viewportL.Width = (float)Settings_Instance()->m_renderWidth / 2.0;
    m_viewportL.Height = (float)Settings_Instance()->m_renderHeight;
    m_viewportL.MinDepth = 0.0f;
    m_viewportL.MaxDepth = 1.0f;
    m_viewportL.TopLeftX = 0;
    m_viewportL.TopLeftY = 0;

    // Right eye viewport
    m_viewportR.Width = (float)Settings_Instance()->m_renderWidth / 2.0;
    m_viewportR.Height = (float)Settings_Instance()->m_renderHeight;
    m_viewportR.MinDepth = 0.0f;
    m_viewportR.MaxDepth = 1.0f;
    m_viewportR.TopLeftX = (float)Settings_Instance()->m_renderWidth / 2.0;
    m_viewportR.TopLeftY = 0;

    // Final composition viewport
    m_viewport.Width = (float)Settings_Instance()->m_renderWidth;
    m_viewport.Height = (float)Settings_Instance()->m_renderHeight;
    m_viewport.MinDepth = 0.0f;
    m_viewport.MaxDepth = 1.0f;
    m_viewport.TopLeftX = 0;
    m_viewport.TopLeftY = 0;

    // Left eye scissor
    m_scissorL.bottom = 0.0f;
    m_scissorL.left = 0.0f;
    m_scissorL.right = (float)Settings_Instance()->m_renderWidth / 2.0f;
    m_scissorL.top = (float)Settings_Instance()->m_renderHeight;

    // Right eye scissor
    m_scissorR.bottom = 0.0f;
    m_scissorR.left = (float)Settings_Instance()->m_renderWidth / 2.0f;
    m_scissorR.right = (float)Settings_Instance()->m_renderWidth;
    m_scissorR.top = (float)Settings_Instance()->m_renderHeight;

    // Final composition scissor
    m_scissor.bottom = 0.0f;
    m_scissor.left = 0.0f;
    m_scissor.right = (float)Settings_Instance()->m_renderWidth;
    m_scissor.top = (float)Settings_Instance()->m_renderHeight;

    //
    // Compile shaders
    //

    std::vector<uint8_t> vshader(
        FRAME_RENDER_VS_CSO_PTR, FRAME_RENDER_VS_CSO_PTR + FRAME_RENDER_VS_CSO_LEN
    );
    hr = m_pD3DRender->GetDevice()->CreateVertexShader(
        (const DWORD*)&vshader[0], vshader.size(), NULL, &m_pVertexShader
    );
    if (FAILED(hr)) {
        Error("CreateVertexShader %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    std::vector<uint8_t> pshader(
        FRAME_RENDER_PS_CSO_PTR, FRAME_RENDER_PS_CSO_PTR + FRAME_RENDER_PS_CSO_LEN
    );
    hr = m_pD3DRender->GetDevice()->CreatePixelShader(
        (const DWORD*)&pshader[0], pshader.size(), NULL, &m_pPixelShader
    );
    if (FAILED(hr)) {
        Error("CreatePixelShader %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    //
    // Create input layout
    //

    // Define the input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "VIEW", 0, DXGI_FORMAT_R32_UINT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    UINT numElements = ARRAYSIZE(layout);

    // Create the input layout
    hr = m_pD3DRender->GetDevice()->CreateInputLayout(
        layout, numElements, &vshader[0], vshader.size(), &m_pVertexLayout
    );
    if (FAILED(hr)) {
        Error("CreateInputLayout %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    // Set the input layout
    m_pD3DRender->GetContext()->IASetInputLayout(m_pVertexLayout.Get());

    //
    // Create frame render CBuffer
    //
    struct FrameRenderBuffer {
        float encodingGamma;
        float _align0;
        float _align1;
        float _align2;
    };
    FrameRenderBuffer frameRenderStruct
        = { (float)(1.0 / Settings_Instance()->m_encodingGamma), 0.0f, 0.0f, 0.0f };
    m_pFrameRenderCBuffer = CreateBuffer(m_pD3DRender->GetDevice(), frameRenderStruct);

    //
    // Create vertex buffer
    //

    // Src texture has various geometry and we should use the part of the textures.
    // That part are defined by uv-coordinates of "bounds" passed to
    // IVRDriverDirectModeComponent::SubmitLayer. So we should update uv-coordinates for every
    // frames and layers.
    D3D11_BUFFER_DESC bd;
    ZeroMemory(&bd, sizeof(bd));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = sizeof(SimpleVertex) * 8;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    hr = m_pD3DRender->GetDevice()->CreateBuffer(&bd, NULL, &m_pVertexBuffer);
    if (FAILED(hr)) {
        Error("CreateBuffer 1 %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    // Set vertex buffer
    UINT stride = sizeof(SimpleVertex);
    UINT offset = 0;
    m_pD3DRender->GetContext()->IASetVertexBuffers(
        0, 1, m_pVertexBuffer.GetAddressOf(), &stride, &offset
    );

    //
    // Create index buffer
    //

    WORD indices[] = { 0, 1, 2, 0, 3, 1,

                       4, 5, 6, 4, 7, 5 };

    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(indices);
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.CPUAccessFlags = 0;

    D3D11_SUBRESOURCE_DATA InitData;
    ZeroMemory(&InitData, sizeof(InitData));
    InitData.pSysMem = indices;

    hr = m_pD3DRender->GetDevice()->CreateBuffer(&bd, &InitData, &m_pIndexBuffer);
    if (FAILED(hr)) {
        Error("CreateBuffer 2 %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    // Set index buffer
    m_pD3DRender->GetContext()->IASetIndexBuffer(m_pIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);

    // Set primitive topology
    m_pD3DRender->GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Create the sample state
    D3D11_SAMPLER_DESC sampDesc;
    ZeroMemory(&sampDesc, sizeof(sampDesc));
    sampDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampDesc.MaxAnisotropy = D3D11_REQ_MAXANISOTROPY;
    sampDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampDesc.MinLOD = 0;
    sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = m_pD3DRender->GetDevice()->CreateSamplerState(&sampDesc, &m_pSamplerLinear);
    if (FAILED(hr)) {
        Error("CreateSamplerState %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    //
    // Create alpha blend state
    // We need alpha blending to support layer.
    //

    // BlendState for first layer.
    // Some VR apps (like SteamVR Home beta) submit the texture that alpha is zero on all pixels.
    // So we need to ignore alpha of first layer.
    D3D11_BLEND_DESC BlendDesc;
    ZeroMemory(&BlendDesc, sizeof(BlendDesc));
    BlendDesc.AlphaToCoverageEnable = FALSE;
    BlendDesc.IndependentBlendEnable = FALSE;
    for (int i = 0; i < 8; i++) {
        BlendDesc.RenderTarget[i].BlendEnable = TRUE;
        BlendDesc.RenderTarget[i].SrcBlend = D3D11_BLEND_ONE;
        BlendDesc.RenderTarget[i].DestBlend = D3D11_BLEND_ZERO;
        BlendDesc.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
        BlendDesc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
        BlendDesc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
        BlendDesc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        BlendDesc.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED
            | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    }

    hr = m_pD3DRender->GetDevice()->CreateBlendState(&BlendDesc, &m_pBlendStateFirst);
    if (FAILED(hr)) {
        Error("CreateBlendState %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    // BleandState for other layers than first.
    BlendDesc.AlphaToCoverageEnable = FALSE;
    BlendDesc.IndependentBlendEnable = FALSE;
    for (int i = 0; i < 8; i++) {
        BlendDesc.RenderTarget[i].BlendEnable = TRUE;
        BlendDesc.RenderTarget[i].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        BlendDesc.RenderTarget[i].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        BlendDesc.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
        BlendDesc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ONE;
        BlendDesc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ZERO;
        BlendDesc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        BlendDesc.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }

    hr = m_pD3DRender->GetDevice()->CreateBlendState(&BlendDesc, &m_pBlendState);
    if (FAILED(hr)) {
        Error("CreateBlendState %p %ls\n", hr, GetErrorStr(hr).c_str());
        return false;
    }

    m_pStagingTexture = compositionTexture;

    std::vector<uint8_t> quadShaderCSO(
        QUAD_SHADER_CSO_PTR, QUAD_SHADER_CSO_PTR + QUAD_SHADER_CSO_LEN
    );
    ComPtr<ID3D11VertexShader> quadVertexShader
        = CreateVertexShader(m_pD3DRender->GetDevice(), quadShaderCSO);

    enableColorCorrection = Settings_Instance()->m_enableColorCorrection;
    if (enableColorCorrection) {
        std::vector<uint8_t> colorCorrectionShaderCSO(
            COLOR_CORRECTION_CSO_PTR, COLOR_CORRECTION_CSO_PTR + COLOR_CORRECTION_CSO_LEN
        );

        ComPtr<ID3D11Texture2D> colorCorrectedTexture = CreateTexture(
            m_pD3DRender->GetDevice(),
            Settings_Instance()->m_renderWidth,
            Settings_Instance()->m_renderHeight,
            Settings_Instance()->m_enableHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                             : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
        );

        struct ColorCorrection {
            float renderWidth;
            float renderHeight;
            float brightness;
            float contrast;
            float saturation;
            float gamma;
            float sharpening;
            float _align;
        };
        ColorCorrection colorCorrectionStruct = {
            (float)Settings_Instance()->m_renderWidth, (float)Settings_Instance()->m_renderHeight,
            Settings_Instance()->m_brightness,         Settings_Instance()->m_contrast + 1.f,
            Settings_Instance()->m_saturation + 1.f,   Settings_Instance()->m_gamma,
            Settings_Instance()->m_sharpening
        };
        ComPtr<ID3D11Buffer> colorCorrectionBuffer
            = CreateBuffer(m_pD3DRender->GetDevice(), colorCorrectionStruct);

        m_colorCorrectionPipeline = std::make_unique<RenderPipeline>(m_pD3DRender->GetDevice());
        m_colorCorrectionPipeline->Initialize(
            { m_pStagingTexture.Get() },
            quadVertexShader.Get(),
            colorCorrectionShaderCSO,
            colorCorrectedTexture.Get(),
            colorCorrectionBuffer.Get()
        );

        m_pStagingTexture = colorCorrectedTexture;
    }

    enableFFE = Settings_Instance()->m_enableFoveatedEncoding;
    if (Settings_Instance()->m_frameGaze.enabled) {
        m_gazeAtlas = std::make_unique<GazeAtlas>(m_pD3DRender->GetDevice(), m_pStagingTexture.Get());
        m_pStagingTexture = m_gazeAtlas->Texture();
    }
    if (enableFFE) {
        m_ffr = std::make_unique<FFR>(m_pD3DRender->GetDevice());
        m_ffr->Initialize(m_pStagingTexture.Get());

        m_pStagingTexture = m_ffr->GetOutputTexture();
    }

    if (Settings_Instance()->m_enableHdr) {
        std::vector<uint8_t> yuv420ShaderCSO(
            RGBTOYUV420_CSO_PTR, RGBTOYUV420_CSO_PTR + RGBTOYUV420_CSO_LEN
        );
        uint32_t texWidth, texHeight;
        GetEncodingResolution(&texWidth, &texHeight);

        ComPtr<ID3D11Texture2D> yuvTexture = CreateTexture(
            m_pD3DRender->GetDevice(),
            texWidth,
            texHeight,
            Settings_Instance()->m_use10bitEncoder ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12
        );

        struct YUVParams {
            float offset[4];
            float yCoeff[4];
            float uCoeff[4];
            float vCoeff[4];

            float renderWidth;
            float renderHeight;
            float _padding0;
            float _padding1;
        };

        // Bless this page for ending my stint of plugging in random values
        // from other projects:
        // https://kdashg.github.io/misc/colors/from-coeffs.html
        YUVParams paramStruct_bt2020_8bit_full
            = { { 0.0000000f, 0.5019608f, 0.5019608f, 0.0f }, // offset
                { 0.2627000f, 0.6780000f, 0.0593000f, 0.0f }, // yCoeff
                { -0.1390825f, -0.3589567f, 0.4980392f, 0.0f }, // uCoeff
                { 0.4980392f, -0.4579826f, -0.0400566f, 0.0f }, // vCoeff
                (float)texWidth,
                (float)texHeight,
                0.0,
                0.0 };

        YUVParams paramStruct_bt2020_10bit_full
            = { { 0.0000000f, 0.5004888f, 0.5004888f, 0.0f }, // offset
                { 0.2627000f, 0.6780000f, 0.0593000f, 0.0f }, // yCoeff
                { -0.1394936f, -0.3600177f, 0.4995112f, 0.0f }, // uCoeff
                { 0.4995112f, -0.4593363f, -0.0401750f, 0.0f }, // vCoeff
                (float)texWidth,
                (float)texHeight,
                0.0,
                0.0 };

        YUVParams paramStruct_bt2020_8bit_limited
            = { { 0.0627451f, 0.5019608f, 0.5019608f, 0.0f }, // offset
                { 0.2256129f, 0.5822824f, 0.0509282f, 0.0f }, // yCoeff
                { -0.1226554f, -0.3165603f, 0.4392157f, 0.0f }, // uCoeff
                { 0.4392157f, -0.4038902f, -0.0353255f, 0.0f }, // vCoeff
                (float)texWidth,
                (float)texHeight,
                0.0,
                0.0 };

        YUVParams paramStruct_bt2020_10bit_limited
            = { { 0.0625611f, 0.5004888f, 0.5004888f, 0.0f }, // offset
                { 0.2249513f, 0.5805748f, 0.0507789f, 0.0f }, // yCoeff
                { -0.1222957f, -0.3156319f, 0.4379277f, 0.0f }, // uCoeff
                { 0.4379277f, -0.4027058f, -0.0352219f, 0.0f }, // vCoeff
                (float)texWidth,
                (float)texHeight,
                0.0,
                0.0 };

        YUVParams& paramStruct = paramStruct_bt2020_8bit_full;
        if (Settings_Instance()->m_use10bitEncoder) {
            paramStruct = paramStruct_bt2020_10bit_full;
        } else {
            paramStruct = paramStruct_bt2020_8bit_full;
        }

        ComPtr<ID3D11Buffer> paramBuffer = CreateBuffer(m_pD3DRender->GetDevice(), paramStruct);

        m_yuvPipeline = std::make_unique<RenderPipelineYUV>(m_pD3DRender->GetDevice());
        m_yuvPipeline->Initialize(
            { m_pStagingTexture.Get() },
            quadVertexShader.Get(),
            yuv420ShaderCSO,
            yuvTexture.Get(),
            paramBuffer.Get()
        );

        m_pStagingTexture = yuvTexture;
    }

    Debug("Staging Texture created\n");

    return true;
}

ComPtr<ID3D11Texture2D> FrameRender::GetTexture() { return m_pStagingTexture; }

void FrameRender::GetEncodingResolution(uint32_t* width, uint32_t* height) {
    if (m_gazeAtlas) {
        m_gazeAtlas->Resolution(width, height);
    } else if (enableFFE) {
        m_ffr->GetOptimizedResolution(width, height);
    } else {
        *width = Settings_Instance()->m_renderWidth;
        *height = Settings_Instance()->m_renderHeight;
    }
}
