#include "FrameRender.h"
#include "FrameRenderPose.hpp"
#include "surfxr_server/Logger.h"
#include "surfxr_server/Utils.h"
#include "surfxr_server/bindings.h"

extern uint64_t g_DriverTestMode;

using namespace d3d_render_utils;

static const DirectX::XMFLOAT4X4 _identityMat = DirectX::XMFLOAT4X4(
    1.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f
);

static DirectX::XMMATRIX HmdMatrix_AsDxMatPosOnly(const vr::HmdMatrix34_t& m) {
    DirectX::XMFLOAT4X4 f = DirectX::XMFLOAT4X4(
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        m.m[0][3],
        m.m[1][3],
        m.m[2][3],
        1.0f
    );
    return DirectX::XMLoadFloat4x4(&f);
}

void FrameRender::SetViewParams(
    vr::HmdRect2_t projLeft,
    vr::HmdMatrix34_t eyeToHeadLeft,
    vr::HmdRect2_t projRight,
    vr::HmdMatrix34_t eyeToHeadRight
) {
    m_viewProj[0] = projLeft;
    m_eyeToHead[0] = eyeToHeadLeft;
    m_viewProj[1] = projRight;
    m_eyeToHead[1] = eyeToHeadRight;
}

bool FrameRender::RenderFrame(
    ID3D11Texture2D* pTexture[][2],
    vr::VRTextureBounds_t bounds[][2],
    vr::HmdMatrix34_t poses[],
    const vr::HmdMatrix34_t& targetPose,
    int layerCount,
    bool recentering,
    uint64_t targetTimestampNs,
    const std::string& message,
    const std::string& debugText
) {
    // Set render target
    m_pD3DRender->GetContext()->OMSetRenderTargets(1, m_pRenderTargetView.GetAddressOf(), NULL);

    m_pD3DRender->GetContext()->OMSetDepthStencilState(m_depthStencilState.Get(), 0);

    // Clear the back buffer
    m_pD3DRender->GetContext()->ClearRenderTargetView(
        m_pRenderTargetView.Get(), DirectX::Colors::MidnightBlue
    );

    // Overlay recentering texture on top of all layers.
    int recenterLayer = -1;
    if (recentering) {
        recenterLayer = layerCount;
        layerCount++;
    }

    // Set up our projection, HMD, and HMD-to-eye transforms once
    const auto nearZ = 0.001f;
    const auto farZ = 1.0f;
    DirectX::XMMATRIX projectionMatL = DirectX::XMMatrixPerspectiveOffCenterRH(
        m_viewProj[0].vTopLeft.v[0] * nearZ,
        m_viewProj[0].vBottomRight.v[0] * nearZ,
        -m_viewProj[0].vTopLeft.v[1] * nearZ,
        -m_viewProj[0].vBottomRight.v[1] * nearZ,
        nearZ,
        farZ
    );
    DirectX::XMMATRIX projectionMatR = DirectX::XMMatrixPerspectiveOffCenterRH(
        m_viewProj[1].vTopLeft.v[0] * nearZ,
        m_viewProj[1].vBottomRight.v[0] * nearZ,
        -m_viewProj[1].vTopLeft.v[1] * nearZ,
        -m_viewProj[1].vBottomRight.v[1] * nearZ,
        nearZ,
        farZ
    );
    DirectX::XMMATRIX hmdToEyeMatL
        = DirectX::XMMatrixInverse(nullptr, HmdMatrix_AsDxMatPosOnly(m_eyeToHead[0]));
    DirectX::XMMATRIX hmdToEyeMatR
        = DirectX::XMMatrixInverse(nullptr, HmdMatrix_AsDxMatPosOnly(m_eyeToHead[1]));
    // I think the negative Y basis is a handedness thing?
    DirectX::XMMATRIX identityMat = DirectX::XMLoadFloat4x4(&_identityMat);

    for (int i = 0; i < layerCount; i++) {
        ID3D11Texture2D* textures[2];
        vr::VRTextureBounds_t bound[2];

        if (i == recenterLayer) {
            textures[0] = (ID3D11Texture2D*)m_recenterTexture.Get();
            textures[1] = (ID3D11Texture2D*)m_recenterTexture.Get();
            bound[0].uMin = bound[0].vMin = bound[1].uMin = bound[1].vMin = 0.0f;
            bound[0].uMax = bound[0].vMax = bound[1].uMax = bound[1].vMax = 1.0f;
        } else {
            textures[0] = pTexture[i][0];
            textures[1] = pTexture[i][1];
            bound[0] = bounds[i][0];
            bound[1] = bounds[i][1];
        }
        if (textures[0] == NULL || textures[1] == NULL) {
            Debug(
                "Ignore NULL layer. layer=%d/%d%s%s\n",
                i,
                layerCount,
                recentering ? L" (recentering)" : L"",
                !message.empty() ? L" (message)" : L""
            );
            continue;
        }

        ComPtr<ID3D11ShaderResourceView> pShaderResourceView[2];
        uint32_t inputColorAdjust[2]{};
        HRESULT hr = S_OK;
        for (unsigned eye = 0; eye < 2; ++eye) {
            hr = m_colorInputs[eye].Prepare(
                m_pD3DRender->GetDevice(), textures[eye],
                Settings_Instance()->m_enableHdr, Settings_Instance()->m_forceHdrSrgbCorrection,
                Settings_Instance()->m_clampHdrExtendedRange, pShaderResourceView[eye], inputColorAdjust[eye]
            );
            if (FAILED(hr)) {
                Error("FrameRender input view eye=%u %ls\n", eye, GetErrorStr(hr).c_str());
                return false;
            }
        }
        if (i == 0) {
            m_pD3DRender->GetContext()->OMSetBlendState(m_pBlendStateFirst.Get(), NULL, 0xffffffff);
        } else {
            m_pD3DRender->GetContext()->OMSetBlendState(m_pBlendState.Get(), NULL, 0xffffffff);
        }

        //
        // Update uv-coordinates in vertex buffer according to bounds.
        //

        DirectX::XMMATRIX viewMatDiff
            = (i == recenterLayer) ? identityMat : surfxr::frame_render_rotation(poses[i], targetPose);

        DirectX::XMMATRIX transformMatL = viewMatDiff * hmdToEyeMatL * projectionMatL;
        DirectX::XMMATRIX transformMatR = viewMatDiff * hmdToEyeMatR * projectionMatR;

        if (i == recenterLayer) {
            transformMatL = identityMat;
            transformMatR = identityMat;
        }

        const auto depth = 700.0f;
        const auto m = 1.0f;
        DirectX::XMFLOAT4 vertsL[4];
        DirectX::XMFLOAT4 vertsR[4];

        DirectX::XMVECTORF32 vertsL_VF32[4]
            = { { { { -1.0f * -m_viewProj[0].vTopLeft.v[0] * depth * m,
                      1.0f * -m_viewProj[0].vTopLeft.v[1] * depth * m,
                      -depth,
                      1.0f } } },
                { { { 1.0f * m_viewProj[0].vBottomRight.v[0] * depth * m,
                      -1.0f * m_viewProj[0].vBottomRight.v[1] * depth * m,
                      -depth,
                      1.0f } } },
                { { { 1.0f * m_viewProj[0].vBottomRight.v[0] * depth * m,
                      1.0f * -m_viewProj[0].vTopLeft.v[1] * depth * m,
                      -depth,
                      1.0f } } },
                { { { -1.0f * -m_viewProj[0].vTopLeft.v[0] * depth * m,
                      -1.0f * m_viewProj[0].vBottomRight.v[1] * depth * m,
                      -depth,
                      1.0f } } } };

        for (int i = 0; i < 4; i++) {
            DirectX::XMStoreFloat4(
                &vertsL[i], DirectX::XMVector3Transform(vertsL_VF32[i], transformMatL)
            );
        }

        DirectX::XMVECTORF32 vertsR_VF32[4]
            = { { { { -1.0f * -m_viewProj[1].vTopLeft.v[0] * depth * m,
                      1.0f * -m_viewProj[1].vTopLeft.v[1] * depth * m,
                      -depth,
                      1.0f } } },
                { { { 1.0f * m_viewProj[1].vBottomRight.v[0] * depth * m,
                      -1.0f * m_viewProj[1].vBottomRight.v[1] * depth * m,
                      -depth,
                      1.0f } } },
                { { { 1.0f * m_viewProj[1].vBottomRight.v[0] * depth * m,
                      1.0f * -m_viewProj[1].vTopLeft.v[1] * depth * m,
                      -depth,
                      1.0f } } },
                { { { -1.0f * -m_viewProj[1].vTopLeft.v[0] * depth * m,
                      -1.0f * m_viewProj[1].vBottomRight.v[1] * depth * m,
                      -depth,
                      1.0f } } } };

        for (int i = 0; i < 4; i++) {
            DirectX::XMStoreFloat4(
                &vertsR[i], DirectX::XMVector3Transform(vertsR_VF32[i], transformMatR)
            );
        }

        // We discard the z value because we never want any clipping,
        // but we do want the w value for perspective correction.
        SimpleVertex vertices[] = {
            // Left View
            { DirectX::XMFLOAT4(vertsL[0].x, vertsL[0].y, 0.5, vertsL[0].w),
              DirectX::XMFLOAT2(bound[0].uMin, bound[0].vMax),
              0 + (inputColorAdjust[0] * 2) },
            { DirectX::XMFLOAT4(vertsL[1].x, vertsL[1].y, 0.5, vertsL[1].w),
              DirectX::XMFLOAT2(bound[0].uMax, bound[0].vMin),
              0 + (inputColorAdjust[0] * 2) },
            { DirectX::XMFLOAT4(vertsL[2].x, vertsL[2].y, 0.5, vertsL[2].w),
              DirectX::XMFLOAT2(bound[0].uMax, bound[0].vMax),
              0 + (inputColorAdjust[0] * 2) },
            { DirectX::XMFLOAT4(vertsL[3].x, vertsL[3].y, 0.5, vertsL[3].w),
              DirectX::XMFLOAT2(bound[0].uMin, bound[0].vMin),
              0 + (inputColorAdjust[0] * 2) },
            // Right View
            { DirectX::XMFLOAT4(vertsR[0].x, vertsR[0].y, 0.5, vertsR[0].w),
              DirectX::XMFLOAT2(bound[1].uMin, bound[1].vMax),
              1 + (inputColorAdjust[1] * 2) },
            { DirectX::XMFLOAT4(vertsR[1].x, vertsR[1].y, 0.5, vertsR[1].w),
              DirectX::XMFLOAT2(bound[1].uMax, bound[1].vMin),
              1 + (inputColorAdjust[1] * 2) },
            { DirectX::XMFLOAT4(vertsR[2].x, vertsR[2].y, 0.5, vertsR[2].w),
              DirectX::XMFLOAT2(bound[1].uMax, bound[1].vMax),
              1 + (inputColorAdjust[1] * 2) },
            { DirectX::XMFLOAT4(vertsR[3].x, vertsR[3].y, 0.5, vertsR[3].w),
              DirectX::XMFLOAT2(bound[1].uMin, bound[1].vMin),
              1 + (inputColorAdjust[1] * 2) },
        };

        D3D11_MAPPED_SUBRESOURCE mapped = { 0 };
        hr = m_pD3DRender->GetContext()->Map(
            m_pVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped
        );
        if (FAILED(hr)) {
            Error("Map %p %ls\n", hr, GetErrorStr(hr).c_str());
            return false;
        }
        memcpy(mapped.pData, vertices, sizeof(vertices));

        m_pD3DRender->GetContext()->Unmap(m_pVertexBuffer.Get(), 0);

        // Set the input layout
        m_pD3DRender->GetContext()->IASetInputLayout(m_pVertexLayout.Get());

        //
        // Set buffers
        //

        UINT stride = sizeof(SimpleVertex);
        UINT offset = 0;
        m_pD3DRender->GetContext()->IASetVertexBuffers(
            0, 1, m_pVertexBuffer.GetAddressOf(), &stride, &offset
        );

        m_pD3DRender->GetContext()->IASetIndexBuffer(m_pIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
        m_pD3DRender->GetContext()->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_pD3DRender->GetContext()->PSSetConstantBuffers(
            0, 1, m_pFrameRenderCBuffer.GetAddressOf()
        );

        //
        // Set shaders
        //

        m_pD3DRender->GetContext()->VSSetShader(m_pVertexShader.Get(), nullptr, 0);
        m_pD3DRender->GetContext()->PSSetShader(m_pPixelShader.Get(), nullptr, 0);

        ID3D11ShaderResourceView* shaderResourceView[2]
            = { pShaderResourceView[0].Get(), pShaderResourceView[1].Get() };
        m_pD3DRender->GetContext()->PSSetShaderResources(0, 2, shaderResourceView);

        m_pD3DRender->GetContext()->PSSetSamplers(0, 1, m_pSamplerLinear.GetAddressOf());

        //
        // Draw
        //

        // Left eye
        m_pD3DRender->GetContext()->RSSetViewports(1, &m_viewportL);
        m_pD3DRender->GetContext()->RSSetScissorRects(1, &m_scissorL);
        m_pD3DRender->GetContext()->DrawIndexed(VERTEX_INDEX_COUNT / 2, 0, 0);

        // Right eye
        m_pD3DRender->GetContext()->RSSetViewports(1, &m_viewportR);
        m_pD3DRender->GetContext()->RSSetScissorRects(1, &m_scissorR);
        m_pD3DRender->GetContext()->DrawIndexed(
            VERTEX_INDEX_COUNT / 2, (VERTEX_INDEX_COUNT / 2), 0
        );
    }

    // Restore full viewport/scissor rect for the rest
    m_pD3DRender->GetContext()->RSSetViewports(1, &m_viewport);
    m_pD3DRender->GetContext()->RSSetScissorRects(1, &m_scissor);

    if (enableColorCorrection) {
        m_colorCorrectionPipeline->Render();
    }

    if (enableFFE) {
        m_ffr->Render(targetTimestampNs);
    }
    if (m_gazeAtlas) m_gazeAtlas->Render(targetTimestampNs);

    if (Settings_Instance()->m_enableHdr) {
        m_yuvPipeline->Render();
    }

    m_pD3DRender->GetContext()->Flush();

    return true;
}
