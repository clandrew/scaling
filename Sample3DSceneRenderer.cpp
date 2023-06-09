#include "pch.h"

#include "Sample3DSceneRenderer.h"

#include "DirectXHelper.h"

#include "scaling.h"

#include "Pass1VS.h"
#include "Pass1PS.h"
#include "Pass2_RgbToYuvCS.h"
#include "Pass2_TexturedQuadVS.h"
#include "Pass2_TexturedQuadPS.h"

using namespace scaling;

using namespace DirectX;
using namespace Microsoft::WRL;

// Loads vertex and pixel shaders from files and instantiates the cube geometry.
Sample3DSceneRenderer::Sample3DSceneRenderer(const std::shared_ptr<DX::DeviceResources>& deviceResources) :
	m_loadingComplete(false),
	m_radiansPerSecond(XM_PIDIV4),	// rotate 45 degrees per second
	m_angle(0),
	m_tracking(false),
	m_mappedConstantBuffer(nullptr),
	m_deviceResources(deviceResources),
	m_scalingType(ScalingType::DLSS),
	m_isSpinning(true),
	m_dlssSupported(false),
	m_dlssSharpness(1.0f),
	m_dlssReset(0)
{
	ZeroMemory(&m_constantBufferData, sizeof(m_constantBufferData));

	CreateDeviceDependentResources();
	CreateTargetSizeDependentResources();

	UpdateWindowTitleText();
}

Sample3DSceneRenderer::~Sample3DSceneRenderer()
{
	m_constantBuffer->Unmap(0, nullptr);
	m_mappedConstantBuffer = nullptr;
}

void Sample3DSceneRenderer::CreateDeviceDependentResources()
{
	auto d3dDevice = m_deviceResources->GetD3DDevice();

	// Create command lists
	DX::ThrowIfFailed(d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_deviceResources->GetDirectCommandAllocator(), m_pass1PipelineState.Get(), IID_PPV_ARGS(&m_commandList)));
	NAME_D3D12_OBJECT(m_commandList);

	DX::ThrowIfFailed(d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE, m_deviceResources->GetVideoEncodeCommandAllocator(), nullptr, IID_PPV_ARGS(&m_videoEncodeCommandList)));
	NAME_D3D12_OBJECT(m_videoEncodeCommandList);

	bool motionEstimationSupported = false;
	ComPtr<ID3D12VideoDevice1> videoDevice;
	if (SUCCEEDED(d3dDevice->QueryInterface(IID_PPV_ARGS(&videoDevice))))
	{
		D3D12_FEATURE_DATA_VIDEO_MOTION_ESTIMATOR motionEstimatorSupport = { 0u, DXGI_FORMAT_NV12 };
		if (SUCCEEDED(videoDevice->CheckFeatureSupport(D3D12_FEATURE_VIDEO_MOTION_ESTIMATOR, &motionEstimatorSupport, sizeof(motionEstimatorSupport))))
		{
			motionEstimationSupported = true;
		}
	}

	NVSDK_NGX_Result Status{};

	if (motionEstimationSupported)
	{
		NVSDK_NGX_Result Status = NVSDK_NGX_D3D12_Init(12341234, L"./", d3dDevice);
		DX::ThrowIfNGXFailed(Status);

		Status = NVSDK_NGX_D3D12_GetCapabilityParameters(&m_ngxParameters);
		DX::ThrowIfNGXFailed(Status);

		int DLSSAvailable = 0;
		Status = m_ngxParameters->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &DLSSAvailable);
		DX::ThrowIfNGXFailed(Status);
		m_dlssSupported = DLSSAvailable > 0;

		{
			int DlssCreateFeatureFlags = NVSDK_NGX_DLSS_Feature_Flags_None;

			NVSDK_NGX_DLSS_Create_Params dlssCreateParams{};
			dlssCreateParams.Feature.InTargetWidth = g_scaling_destWidth;
			dlssCreateParams.Feature.InTargetHeight = g_scaling_destHeight;
			dlssCreateParams.Feature.InWidth = g_scaling_sourceWidth;
			dlssCreateParams.Feature.InHeight = g_scaling_sourceHeight;
			dlssCreateParams.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_MaxQuality;
			dlssCreateParams.InFeatureCreateFlags = DlssCreateFeatureFlags;

			UINT creationNodeMask = 1;
			UINT visibilityNodeMask = 1;
			Status = NGX_D3D12_CREATE_DLSS_EXT(
				m_commandList.Get(),
				creationNodeMask,
				visibilityNodeMask,
				&m_dlssFeatureHandle,
				m_ngxParameters,
				&dlssCreateParams);

			DX::ThrowIfNGXFailed(Status);
		}

		D3D12_VIDEO_MOTION_ESTIMATOR_DESC motionEstimatorDesc = {
			0, //NodeIndex
			DXGI_FORMAT_NV12,
			D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_16X16,
			D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_QUARTER_PEL,
			{g_scaling_sourceWidth, g_scaling_sourceHeight, g_scaling_sourceWidth, g_scaling_sourceHeight} // D3D12_VIDEO_SIZE_RANGE
		};

		DX::ThrowIfFailed(videoDevice->CreateVideoMotionEstimator(
			&motionEstimatorDesc,
			nullptr,
			IID_PPV_ARGS(&m_videoMotionEstimator)));

		D3D12_VIDEO_MOTION_VECTOR_HEAP_DESC motionVectorHeapDesc = {
			0, // NodeIndex 
			DXGI_FORMAT_NV12,
			D3D12_VIDEO_MOTION_ESTIMATOR_SEARCH_BLOCK_SIZE_16X16,
			D3D12_VIDEO_MOTION_ESTIMATOR_VECTOR_PRECISION_QUARTER_PEL,
			{g_scaling_sourceWidth, g_scaling_sourceHeight, g_scaling_sourceWidth, g_scaling_sourceHeight} // D3D12_VIDEO_SIZE_RANGE
		};

		videoDevice->CreateVideoMotionVectorHeap(
			&motionVectorHeapDesc,
			nullptr,
			IID_PPV_ARGS(&m_videoMotionVectorHeap));
	}

	{
		D3D12_RESOURCE_DESC resourceDesc{};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		resourceDesc.Width = g_scaling_destWidth;
		resourceDesc.Height = g_scaling_destHeight;
		resourceDesc.MipLevels = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		resourceDesc.SampleDesc.Count = 1;
		resourceDesc.SampleDesc.Quality = 0;
		resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		auto defaultHeapType = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
			&defaultHeapType,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			nullptr,
			IID_PPV_ARGS(&m_dlssTarget)));
	}
	{
		D3D12_RESOURCE_DESC resourceDesc{};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		resourceDesc.Width = g_scaling_sourceWidth;
		resourceDesc.Height = g_scaling_sourceHeight;
		resourceDesc.MipLevels = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.Format = DXGI_FORMAT_R16G16_SINT;
		resourceDesc.SampleDesc.Count = 1;
		resourceDesc.SampleDesc.Quality = 0;

		auto defaultHeapType = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
			&defaultHeapType,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(&m_motionVectors)));
		DX::SetName(m_motionVectors.Get(), L"m_motionVectors");
	}
	{
		D3D12_RESOURCE_DESC resourceDesc{};
		resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		
		resourceDesc.Width = g_scaling_sourceWidth;
		assert(resourceDesc.Width % 2 == 0); // NV12 needs to have multiple-of-two size.

		resourceDesc.Height = g_scaling_sourceHeight;
		assert(resourceDesc.Height % 2 == 0);

		resourceDesc.MipLevels = 1;
		resourceDesc.DepthOrArraySize = 1;
		resourceDesc.Format = DXGI_FORMAT_NV12;
		resourceDesc.SampleDesc.Count = 1;
		resourceDesc.SampleDesc.Quality = 0;
		resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		auto defaultHeapType = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
		DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
			&defaultHeapType,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_COMMON,
			nullptr,
			IID_PPV_ARGS(&m_currentYuv)));
		DX::SetName(m_currentYuv.Get(), L"m_currentYuv");

		DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
			&defaultHeapType,
			D3D12_HEAP_FLAG_NONE,
			&resourceDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&m_previousYuv)));
		DX::SetName(m_previousYuv.Get(), L"m_previousYuv");
	}

	DX::ThrowIfNGXFailed(Status);

	// Graphics root sig
	{
		CD3DX12_DESCRIPTOR_RANGE ranges[2];
		CD3DX12_ROOT_PARAMETER parameters[2];

		{
			{
				UINT numDescriptors = DX::c_frameCount; // constant buffer for each frame
				UINT baseShaderRegister = 0;
				ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, numDescriptors, baseShaderRegister);
			}
			{
				UINT numDescriptors = 1;
				UINT baseShaderRegister = 0;
				ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, numDescriptors, baseShaderRegister);
			}
			parameters[0].InitAsDescriptorTable(_countof(ranges), ranges, D3D12_SHADER_VISIBILITY_ALL);
		}
		{
			UINT num32BitValues = 1;
			UINT shaderRegister = 3;
			UINT registerSpace = 0u;
			parameters[1].InitAsConstants(num32BitValues, shaderRegister, registerSpace, D3D12_SHADER_VISIBILITY_ALL);
		}

		D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | // Only the input assembler stage needs access to the constant buffer.
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;

		D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
		{
			samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
			samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
			samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
			samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
			samplers[0].MipLODBias = 0;
			samplers[0].MaxAnisotropy = 0;
			samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
			samplers[0].MinLOD = 0.0f;
			samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
			samplers[0].ShaderRegister = 0;
			samplers[0].RegisterSpace = 0;
			samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		}
		{
			samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			samplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
			samplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
			samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
			samplers[1].MipLODBias = 0;
			samplers[1].MaxAnisotropy = 0;
			samplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			samplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
			samplers[1].MinLOD = 0.0f;
			samplers[1].MaxLOD = D3D12_FLOAT32_MAX;
			samplers[1].ShaderRegister = 1;
			samplers[1].RegisterSpace = 0;
			samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
		}

		CD3DX12_ROOT_SIGNATURE_DESC descRootSignature;
		descRootSignature.Init(_countof(parameters), parameters, _countof(samplers), samplers, rootSignatureFlags);

		ComPtr<ID3DBlob> pSignature;
		ComPtr<ID3DBlob> pError;

		HRESULT serializeRootSignatureHR = D3D12SerializeRootSignature(&descRootSignature, D3D_ROOT_SIGNATURE_VERSION_1, pSignature.GetAddressOf(), pError.GetAddressOf());
		if (FAILED(serializeRootSignatureHR))
		{
			OutputDebugStringA(reinterpret_cast<char*>(pError->GetBufferPointer()));
			_com_issue_error(serializeRootSignatureHR);
		}
		DX::ThrowIfFailed(d3dDevice->CreateRootSignature(0, pSignature->GetBufferPointer(), pSignature->GetBufferSize(), IID_PPV_ARGS(&m_commonGraphicsRootSignature)));
        NAME_D3D12_OBJECT(m_commonGraphicsRootSignature);
	}

	// Compute root sig
	{
		CD3DX12_DESCRIPTOR_RANGE ranges[1];
		CD3DX12_ROOT_PARAMETER parameters[2];

		UINT descriptorCount = 3;
		ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, descriptorCount, 0);

		parameters[0].InitAsDescriptorTable(_countof(ranges), ranges);
		parameters[1].InitAsConstants(2, 0);

		D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS;

		CD3DX12_ROOT_SIGNATURE_DESC descRootSignature;
		descRootSignature.Init(_countof(parameters), parameters, 0, nullptr, rootSignatureFlags);

		ComPtr<ID3DBlob> pSignature;
		ComPtr<ID3DBlob> pError;

		HRESULT serializeRootSignatureHR = D3D12SerializeRootSignature(&descRootSignature, D3D_ROOT_SIGNATURE_VERSION_1, pSignature.GetAddressOf(), pError.GetAddressOf());
		if (FAILED(serializeRootSignatureHR))
		{
			OutputDebugStringA(reinterpret_cast<char*>(pError->GetBufferPointer()));
			_com_issue_error(serializeRootSignatureHR);
		}
		DX::ThrowIfFailed(d3dDevice->CreateRootSignature(0, pSignature->GetBufferPointer(), pSignature->GetBufferSize(), IID_PPV_ARGS(&m_commonComputeRootSignature)));
		NAME_D3D12_OBJECT(m_commonComputeRootSignature);
	}

	// Create the pipeline state once the shaders are loaded.
	{
		static const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};

		D3D12_GRAPHICS_PIPELINE_STATE_DESC state = {};
		state.InputLayout = { inputLayout, _countof(inputLayout) };
		state.pRootSignature = m_commonGraphicsRootSignature.Get();
        state.VS = CD3DX12_SHADER_BYTECODE((void*)(g_Pass1VS), _countof(g_Pass1VS));
        state.PS = CD3DX12_SHADER_BYTECODE((void*)(g_Pass1PS), _countof(g_Pass1PS));
		state.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		state.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		state.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		state.SampleMask = UINT_MAX;
		state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		state.NumRenderTargets = 1;
		state.RTVFormats[0] = m_deviceResources->GetBackBufferFormat();
		state.DSVFormat = m_deviceResources->GetDepthBufferFormat();
		state.SampleDesc.Count = 1;

		DX::ThrowIfFailed(m_deviceResources->GetD3DDevice()->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&m_pass1PipelineState)));
	};
	{

		static const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
		};

		D3D12_GRAPHICS_PIPELINE_STATE_DESC state = {};
		state.InputLayout = { inputLayout, _countof(inputLayout) };
		state.pRootSignature = m_commonGraphicsRootSignature.Get();
		state.VS = CD3DX12_SHADER_BYTECODE((void*)(g_Pass2_TexturedQuadVS), _countof(g_Pass2_TexturedQuadVS));
		state.PS = CD3DX12_SHADER_BYTECODE((void*)(g_Pass2_TexturedQuadPS), _countof(g_Pass2_TexturedQuadPS));
		state.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		state.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		state.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		state.SampleMask = UINT_MAX;
		state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		state.NumRenderTargets = 1;
		state.RTVFormats[0] = m_deviceResources->GetBackBufferFormat();
		state.DSVFormat = DXGI_FORMAT_UNKNOWN;
		state.SampleDesc.Count = 1;

		DX::ThrowIfFailed(m_deviceResources->GetD3DDevice()->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&m_pass2_TexturedQuad_PipelineState)));
	};
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC pipelineStateDesc{};
		pipelineStateDesc.pRootSignature = m_commonComputeRootSignature.Get();
		pipelineStateDesc.CS = CD3DX12_SHADER_BYTECODE((void*)(g_Pass2_RgbToYuvCS), _countof(g_Pass2_RgbToYuvCS));
		DX::ThrowIfFailed(m_deviceResources->GetD3DDevice()->CreateComputePipelineState(&pipelineStateDesc, IID_PPV_ARGS(&m_pass2_YuvConversion_PipelineState)));
	}

	// Create and upload cube and quad geometry resources to the GPU.
	{
		CD3DX12_HEAP_PROPERTIES uploadHeapProperties(D3D12_HEAP_TYPE_UPLOAD);
		CD3DX12_HEAP_PROPERTIES defaultHeapProperties(D3D12_HEAP_TYPE_DEFAULT);

		// Do not release these until the upload is finished
		Microsoft::WRL::ComPtr<ID3D12Resource> spinningCubeVertexBufferUpload;
		Microsoft::WRL::ComPtr<ID3D12Resource> spinningCubeIndexBufferUpload;
		Microsoft::WRL::ComPtr<ID3D12Resource> texturedQuadVertexBufferUpload;
		Microsoft::WRL::ComPtr<ID3D12Resource> texturedQuadIndexBufferUpload;

		// Cube
		{
			// Cube vertices. Each vertex has a position and a color.
			VertexPositionColor cubeVertices[] =
			{
				{ XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, 0.0f) },
				{ XMFLOAT3(-0.5f, -0.5f,  0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
				{ XMFLOAT3(-0.5f,  0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
				{ XMFLOAT3(-0.5f,  0.5f,  0.5f), XMFLOAT3(0.0f, 1.0f, 1.0f) },
				{ XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
				{ XMFLOAT3(0.5f, -0.5f,  0.5f), XMFLOAT3(1.0f, 0.0f, 1.0f) },
				{ XMFLOAT3(0.5f,  0.5f, -0.5f), XMFLOAT3(1.0f, 1.0f, 0.0f) },
				{ XMFLOAT3(0.5f,  0.5f,  0.5f), XMFLOAT3(1.0f, 1.0f, 1.0f) },
			};
			const UINT spinningCubeVertexBufferSize = sizeof(cubeVertices);

			CD3DX12_RESOURCE_DESC spinningCubeVertexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(spinningCubeVertexBufferSize);
			DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
				&defaultHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&spinningCubeVertexBufferDesc,
				D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr,
				IID_PPV_ARGS(&m_spinningCubeVertexBuffer)));
			NAME_D3D12_OBJECT(m_spinningCubeVertexBuffer);

			// Create vertex/index buffer views.
			m_spinningCubeVertexBufferView.BufferLocation = m_spinningCubeVertexBuffer->GetGPUVirtualAddress();
			m_spinningCubeVertexBufferView.StrideInBytes = sizeof(VertexPositionColor);
			m_spinningCubeVertexBufferView.SizeInBytes = sizeof(cubeVertices);

			DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
				&uploadHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&spinningCubeVertexBufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&spinningCubeVertexBufferUpload)));

			D3D12_SUBRESOURCE_DATA spinningCubeVertexData = {};
			spinningCubeVertexData.pData = reinterpret_cast<BYTE*>(cubeVertices);
			spinningCubeVertexData.RowPitch = spinningCubeVertexBufferSize;
			spinningCubeVertexData.SlicePitch = spinningCubeVertexData.RowPitch;

			UpdateSubresources(m_commandList.Get(), m_spinningCubeVertexBuffer.Get(), spinningCubeVertexBufferUpload.Get(), 0, 0, 1, &spinningCubeVertexData);

			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_spinningCubeVertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			m_commandList->ResourceBarrier(1, &barrier);

			// Load mesh indices. Each trio of indices represents a triangle to be rendered on the screen.
			// For example: 0,2,1 means that the vertices with indexes 0, 2 and 1 from the vertex buffer compose the
			// first triangle of this mesh.
			unsigned short cubeIndices[] =
			{
				0, 2, 1, // -x
				1, 2, 3,

				4, 5, 6, // +x
				5, 7, 6,

				0, 1, 5, // -y
				0, 5, 4,

				2, 6, 7, // +y
				2, 7, 3,

				0, 4, 6, // -z
				0, 6, 2,

				1, 3, 7, // +z
				1, 7, 5,
			};

			const UINT spinningCubeIndexBufferSize = sizeof(cubeIndices);

			CD3DX12_RESOURCE_DESC indexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(spinningCubeIndexBufferSize);
			DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
				&defaultHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&indexBufferDesc,
				D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr,
				IID_PPV_ARGS(&m_spinningCubeIndexBuffer)));

			m_spinningCubeIndexBufferView.BufferLocation = m_spinningCubeIndexBuffer->GetGPUVirtualAddress();
			m_spinningCubeIndexBufferView.SizeInBytes = sizeof(cubeIndices);
			m_spinningCubeIndexBufferView.Format = DXGI_FORMAT_R16_UINT;

			DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
				&uploadHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&indexBufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&spinningCubeIndexBufferUpload)));

			NAME_D3D12_OBJECT(m_spinningCubeIndexBuffer);

			D3D12_SUBRESOURCE_DATA indexData = {};
			indexData.pData = reinterpret_cast<BYTE*>(cubeIndices);
			indexData.RowPitch = spinningCubeIndexBufferSize;
			indexData.SlicePitch = indexData.RowPitch;

			UpdateSubresources(m_commandList.Get(), m_spinningCubeIndexBuffer.Get(), spinningCubeIndexBufferUpload.Get(), 0, 0, 1, &indexData);

			CD3DX12_RESOURCE_BARRIER indexBufferResourceBarrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_spinningCubeIndexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
			m_commandList->ResourceBarrier(1, &indexBufferResourceBarrier);
		}

		// Quad
		{
		VertexUV quadVertices[] =
			{
				{ XMFLOAT3(-1, -1, 1), XMFLOAT2(0.0f, 1.0f) }, // Top left
				{ XMFLOAT3(1,  -1, 1), XMFLOAT2(1.0f, 1.0f) }, // Top right
				{ XMFLOAT3(-1,  1, 1), XMFLOAT2(0.0f, 0.0f) }, // Bottom left
				{ XMFLOAT3(1,   1, 1), XMFLOAT2(1.0f, 0.0f) }, // Bottom right
			};
			const UINT quadVertexBufferSize = sizeof(quadVertices);

			CD3DX12_RESOURCE_DESC quadVertexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(quadVertexBufferSize);
			DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
				&defaultHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&quadVertexBufferDesc,
				D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr,
				IID_PPV_ARGS(&m_texturedQuadVertexBuffer)));
			NAME_D3D12_OBJECT(m_texturedQuadVertexBuffer);

			// Create vertex/index buffer views.
			m_texturedQuadVertexBufferView.BufferLocation = m_texturedQuadVertexBuffer->GetGPUVirtualAddress();
			m_texturedQuadVertexBufferView.StrideInBytes = sizeof(VertexUV);
			m_texturedQuadVertexBufferView.SizeInBytes = sizeof(quadVertices);

			DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
				&uploadHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&quadVertexBufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&texturedQuadVertexBufferUpload)));

			D3D12_SUBRESOURCE_DATA quadVertexData = {};
			quadVertexData.pData = reinterpret_cast<BYTE*>(quadVertices);
			quadVertexData.RowPitch = quadVertexBufferSize;
			quadVertexData.SlicePitch = quadVertexData.RowPitch;

			UpdateSubresources(m_commandList.Get(), m_texturedQuadVertexBuffer.Get(), texturedQuadVertexBufferUpload.Get(), 0, 0, 1, &quadVertexData);

			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_texturedQuadVertexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			m_commandList->ResourceBarrier(1, &barrier);

			unsigned short quadIndices[] =
			{
				0, 2, 1,
				1, 2, 3,
			};

			const UINT quadIndexBufferSize = sizeof(quadIndices);

			CD3DX12_RESOURCE_DESC indexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(quadIndexBufferSize);
			DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
				&defaultHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&indexBufferDesc,
				D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr,
				IID_PPV_ARGS(&m_texturedQuadIndexBuffer)));

			m_texturedQuadIndexBufferView.BufferLocation = m_texturedQuadIndexBuffer->GetGPUVirtualAddress();
			m_texturedQuadIndexBufferView.SizeInBytes = sizeof(quadIndices);
			m_texturedQuadIndexBufferView.Format = DXGI_FORMAT_R16_UINT;

			DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
				&uploadHeapProperties,
				D3D12_HEAP_FLAG_NONE,
				&indexBufferDesc,
				D3D12_RESOURCE_STATE_GENERIC_READ,
				nullptr,
				IID_PPV_ARGS(&texturedQuadIndexBufferUpload)));

			NAME_D3D12_OBJECT(m_texturedQuadIndexBuffer);

			D3D12_SUBRESOURCE_DATA indexData = {};
			indexData.pData = reinterpret_cast<BYTE*>(quadIndices);
			indexData.RowPitch = quadIndexBufferSize;
			indexData.SlicePitch = indexData.RowPitch;

			UpdateSubresources(m_commandList.Get(), m_texturedQuadIndexBuffer.Get(), texturedQuadIndexBufferUpload.Get(), 0, 0, 1, &indexData);

			CD3DX12_RESOURCE_BARRIER indexBufferResourceBarrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_texturedQuadIndexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
			m_commandList->ResourceBarrier(1, &indexBufferResourceBarrier);
		}

		// Create a descriptor heap for the constant buffers and SRV.
		{
			// Descriptor table has these contents:

			// frame 0 graphics constants
			// frame 1 graphics constants
			// frame 2 graphics constants
			// intermediate srv
			// intermediate uav rgb source
			// intermediate uav luminance plane
			// intermediate uav chrominance plane

			D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
			heapDesc.NumDescriptors = DX::c_frameCount + 4;
			heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			// This flag indicates that this descriptor heap can be bound to the pipeline and that descriptors contained in it can be referenced by a root table.
			heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			DX::ThrowIfFailed(d3dDevice->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_cbvSrvHeap)));

            NAME_D3D12_OBJECT(m_cbvSrvHeap);
		}

		CD3DX12_RESOURCE_DESC constantBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(DX::c_frameCount * c_alignedConstantBufferSize);
		DX::ThrowIfFailed(d3dDevice->CreateCommittedResource(
			&uploadHeapProperties,
			D3D12_HEAP_FLAG_NONE,
			&constantBufferDesc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&m_constantBuffer)));

        NAME_D3D12_OBJECT(m_constantBuffer);

		// Create constant buffer views to access the upload buffer.
		D3D12_GPU_VIRTUAL_ADDRESS cbvGpuAddress = m_constantBuffer->GetGPUVirtualAddress();
		CD3DX12_CPU_DESCRIPTOR_HANDLE cbvSrvCpuHandle(m_cbvSrvHeap->GetCPUDescriptorHandleForHeapStart());
		m_cbvDescriptorSize = d3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		for (int n = 0; n < DX::c_frameCount; n++)
		{
			D3D12_CONSTANT_BUFFER_VIEW_DESC desc;
			desc.BufferLocation = cbvGpuAddress;
			desc.SizeInBytes = c_alignedConstantBufferSize;
			d3dDevice->CreateConstantBufferView(&desc, cbvSrvCpuHandle);

			cbvGpuAddress += desc.SizeInBytes;
			cbvSrvCpuHandle.Offset(m_cbvDescriptorSize);
		}

		// Create SRV
		{
			D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			d3dDevice->CreateShaderResourceView(m_deviceResources->GetIntermediateRenderTarget(), &srvDesc, cbvSrvCpuHandle);
			cbvSrvCpuHandle.Offset(m_cbvDescriptorSize);
		}
		// Create UAV for rgb source
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			d3dDevice->CreateUnorderedAccessView(m_deviceResources->GetIntermediateRenderTarget(), nullptr, &uavDesc, cbvSrvCpuHandle);
			cbvSrvCpuHandle.Offset(m_cbvDescriptorSize);
		}
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DXGI_FORMAT_R8_UNORM; // Selects the luminance plane
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			d3dDevice->CreateUnorderedAccessView(m_currentYuv.Get(), nullptr, &uavDesc, cbvSrvCpuHandle);
			cbvSrvCpuHandle.Offset(m_cbvDescriptorSize);
		}
		{
			D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = DXGI_FORMAT_R8G8_UNORM; // Selects the chrominance plane
			uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.PlaneSlice = 1;
			d3dDevice->CreateUnorderedAccessView(m_currentYuv.Get(), nullptr, &uavDesc, cbvSrvCpuHandle);
			cbvSrvCpuHandle.Offset(m_cbvDescriptorSize);
		}

		// Map the constant buffers.
		CD3DX12_RANGE readRange(0, 0);		// We do not intend to read from this resource on the CPU.
		DX::ThrowIfFailed(m_constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&m_mappedConstantBuffer)));
		ZeroMemory(m_mappedConstantBuffer, DX::c_frameCount * c_alignedConstantBufferSize);
		// We don't unmap this until the app closes. Keeping things mapped for the lifetime of the resource is okay.

		// Close the command list and execute it to begin the vertex/index buffer copy into the GPU's default heap.
		DX::ThrowIfFailed(m_commandList->Close());
		ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
		m_deviceResources->GetCommandQueue()->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

		// Wait for the command list to finish executing; the vertex/index buffers need to be uploaded to the GPU before the upload resources go out of scope.
		m_deviceResources->WaitForGpuOnDirectQueue();

		DX::ThrowIfFailed(m_videoEncodeCommandList->Close());
	};

	m_loadingComplete = true;
}

// Initializes view parameters when the window size changes.
void Sample3DSceneRenderer::CreateTargetSizeDependentResources()
{
	D3D12_VIEWPORT pass1Viewport = m_deviceResources->GetPass1Viewport();
	m_pass1ScissorRect = { 0, 0, static_cast<LONG>(pass1Viewport.Width), static_cast<LONG>(pass1Viewport.Height)};

	D3D12_VIEWPORT pass2Viewport = m_deviceResources->GetPass2Viewport();
	m_pass2ScissorRect = { 0, 0, static_cast<LONG>(pass2Viewport.Width), static_cast<LONG>(pass2Viewport.Height) };

	float aspectRatio = static_cast<float>(pass1Viewport.Width) / static_cast<float>(pass1Viewport.Height);
	float fovAngleY = 70.0f * XM_PI / 180.0f;
	if (aspectRatio < 1.0f)
	{
		fovAngleY *= 2.0f;
	}
	
	// This sample makes use of a right-handed coordinate system using row-major matrices.
	XMMATRIX perspectiveMatrix = XMMatrixPerspectiveFovRH(
		fovAngleY,
		aspectRatio,
		0.01f,
		100.0f
		);

	XMStoreFloat4x4(
		&m_constantBufferData.projection,
		XMMatrixTranspose(perspectiveMatrix)
		);

	// Eye is at (0,0.7,1.5), looking at point (0,-0.1,0) with the up-vector along the y-axis.
	static const XMVECTORF32 eye = { 0.0f, 0.7f - 0.5, 1.5f, 0.0f };
	static const XMVECTORF32 at = { 0.0f, -0.1f + 0.1f, 0.0f, 0.0f };
	static const XMVECTORF32 up = { 0.0f, 1.0f, 0.0f, 0.0f };

	XMStoreFloat4x4(&m_constantBufferData.view, XMMatrixTranspose(XMMatrixLookAtRH(eye, at, up)));
}

// Called once per frame, rotates the cube and calculates the model and view matrices.
void Sample3DSceneRenderer::Update(DX::StepTimer const& timer)
{
	if (m_loadingComplete && m_isSpinning)
	{
		if (!m_tracking)
		{
			// Rotate the cube a small amount.
			m_angle += static_cast<float>(timer.GetElapsedSeconds()) * m_radiansPerSecond;

			Rotate(m_angle);
		}

		// Update the constant buffer resource.
		UINT8* destination = m_mappedConstantBuffer + (m_deviceResources->GetCurrentFrameIndex() * c_alignedConstantBufferSize);
		memcpy(destination, &m_constantBufferData, sizeof(m_constantBufferData));
	}
}

// Rotate the 3D cube model a set amount of radians.
void Sample3DSceneRenderer::Rotate(float radians)
{
	// Prepare to pass the updated model matrix to the shader.
	XMStoreFloat4x4(&m_constantBufferData.model, XMMatrixTranspose(XMMatrixRotationY(radians)));
}

// Renders one frame using the vertex and pixel shaders.
bool Sample3DSceneRenderer::RenderAndPresent()
{
	if (!m_loadingComplete)
	{
		return false;
	}

	DX::ThrowIfFailed(m_deviceResources->GetDirectCommandAllocator()->Reset());

	// The command list can be reset anytime after ExecuteCommandList() is called.
	DX::ThrowIfFailed(m_commandList->Reset(m_deviceResources->GetDirectCommandAllocator(), nullptr));

	ID3D12DescriptorHeap* ppHeaps[] = { m_cbvSrvHeap.Get() };
	m_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

	m_commandList->SetGraphicsRootSignature(m_commonGraphicsRootSignature.Get());
	m_commandList->SetComputeRootSignature(m_commonComputeRootSignature.Get());
	
	// Set root params
	{
		CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart(), 0, m_cbvDescriptorSize);
		m_commandList->SetGraphicsRootDescriptorTable(0, gpuHandle);
	}
	{
		UINT samplerIndex = 0;
		if (m_scalingType == ScalingType::Point)
		{
			samplerIndex = 0;
		}
		else if (m_scalingType == ScalingType::Linear)
		{
			samplerIndex = 1;
		}
		m_commandList->SetGraphicsRoot32BitConstant(1, samplerIndex, 0);
	}

	// Pass 1
	{
		m_commandList->SetPipelineState(m_pass1PipelineState.Get());

		// Set the viewport and scissor rectangle.
		D3D12_VIEWPORT pass1Viewport = m_deviceResources->GetPass1Viewport();
		m_commandList->RSSetViewports(1, &pass1Viewport);
		m_commandList->RSSetScissorRects(1, &m_pass1ScissorRect);

		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_deviceResources->GetIntermediateRenderTarget(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			m_commandList->ResourceBarrier(1, &barrier);
		}

		{
			D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView = m_deviceResources->GetIntermediateRenderTargetCpuDescriptor();
			D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView = m_deviceResources->GetDepthStencilView();

			float cornflowerBlue[] = { 0.3f, 0.58f, 0.93f, 1.0f };
			m_commandList->ClearRenderTargetView(renderTargetView, cornflowerBlue, 0, nullptr);
			m_commandList->ClearDepthStencilView(depthStencilView, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
			m_commandList->OMSetRenderTargets(1, &renderTargetView, false, &depthStencilView);
		}

		m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_commandList->IASetVertexBuffers(0, 1, &m_spinningCubeVertexBufferView);
		m_commandList->IASetIndexBuffer(&m_spinningCubeIndexBufferView);
		m_commandList->DrawIndexedInstanced(36, 1, 0, 0, 0);

	}

	// Pass 2
	if (m_scalingType == ScalingType::Point || m_scalingType == ScalingType::Linear)
	{
		m_commandList->SetPipelineState(m_pass2_TexturedQuad_PipelineState.Get());

		// Set the viewport and scissor rectangle.
		D3D12_VIEWPORT pass2Viewport = m_deviceResources->GetPass2Viewport();
		m_commandList->RSSetViewports(1, &pass2Viewport);
		m_commandList->RSSetScissorRects(1, &m_pass2ScissorRect);

		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_deviceResources->GetIntermediateRenderTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			m_commandList->ResourceBarrier(1, &barrier);
		}
		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_deviceResources->GetSwapChainRenderTarget(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
			m_commandList->ResourceBarrier(1, &barrier);
		}
		{
			D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView = m_deviceResources->GetSwapChainRenderTargetCpuDescriptor();

			float cornflowerBlue[] = { 0.3f, 0.58f, 0.93f, 1.0f };
			m_commandList->ClearRenderTargetView(renderTargetView, cornflowerBlue, 0, nullptr);
			m_commandList->OMSetRenderTargets(1, &renderTargetView, false, nullptr);
		}
		m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_commandList->IASetVertexBuffers(0, 1, &m_texturedQuadVertexBufferView);
		m_commandList->IASetIndexBuffer(&m_texturedQuadIndexBufferView);
		m_commandList->DrawIndexedInstanced(6, 1, 0, 0, 0);

		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_deviceResources->GetSwapChainRenderTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
			m_commandList->ResourceBarrier(1, &barrier);
		}

		DX::ThrowIfFailed(m_commandList->Close());

		// Execute the command list.
		ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
		m_deviceResources->GetCommandQueue()->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

		m_deviceResources->Present();
	}
	else
	{
		assert(m_scalingType == ScalingType::DLSS);

		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_deviceResources->GetIntermediateRenderTarget(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE );
			m_commandList->ResourceBarrier(1, &barrier);
		}
		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_currentYuv.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			m_commandList->ResourceBarrier(1, &barrier);
		}

		// Convert Rgb to Yuv because motion estimation requires yuv
		{
			m_commandList->SetPipelineState(m_pass2_YuvConversion_PipelineState.Get());

			// First half of descriptor table is graphics stuff, second half is compute. Select the compute items
			CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart(), 4, m_cbvDescriptorSize);
			m_commandList->SetComputeRootDescriptorTable(0, gpuHandle);
			UINT rootConstants[2] = { static_cast<UINT>(g_scaling_sourceWidth), static_cast<UINT>(g_scaling_sourceHeight) };
			m_commandList->SetComputeRoot32BitConstants(1, 2, rootConstants, 0);

			UINT dispatchX = static_cast<UINT>(g_scaling_sourceWidth) / 64 + 1;
			UINT dispatchY = g_scaling_sourceHeight;
			m_commandList->Dispatch(dispatchX, dispatchY, 1);
		}
		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_previousYuv.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			m_commandList->ResourceBarrier(1, &barrier);
		}
		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_currentYuv.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
			m_commandList->ResourceBarrier(1, &barrier);
		}

		DX::ThrowIfFailed(m_commandList->Close());

		{
			ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
			m_deviceResources->GetCommandQueue()->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
		}

		m_deviceResources->WaitForGpuOnDirectQueue(); // Wait for graphics conversion to finish

		DX::ThrowIfFailed(m_deviceResources->GetVideoEncodeCommandAllocator()->Reset());
		DX::ThrowIfFailed(m_videoEncodeCommandList->Reset(m_deviceResources->GetVideoEncodeCommandAllocator()));

		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_previousYuv.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ);
			m_videoEncodeCommandList->ResourceBarrier(1, &barrier);
		}
		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_currentYuv.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ);
			m_videoEncodeCommandList->ResourceBarrier(1, &barrier);
		}
		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_motionVectors.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE);
			m_videoEncodeCommandList->ResourceBarrier(1, &barrier);
		}

		// Run motion estimation
		{
			const D3D12_VIDEO_MOTION_ESTIMATOR_INPUT inputArgs = {
				m_currentYuv.Get(),
				0,
				m_previousYuv.Get(),
				0,
				nullptr // pHintMotionVectorHeap
			};

			const D3D12_VIDEO_MOTION_ESTIMATOR_OUTPUT outputArgs = { m_videoMotionVectorHeap.Get() };

			m_videoEncodeCommandList->EstimateMotion(m_videoMotionEstimator.Get(), &outputArgs, &inputArgs);
		}
		// Resolve motion vectors
		{
			D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_INPUT inputArgs =
			{
				m_videoMotionVectorHeap.Get(),
				g_scaling_sourceWidth,
				g_scaling_sourceHeight
			};

			D3D12_RESOURCE_COORDINATE ouputCoordinate{};

			D3D12_RESOLVE_VIDEO_MOTION_VECTOR_HEAP_OUTPUT outputArgs =
			{
				m_motionVectors.Get(),
				ouputCoordinate
			};

			m_videoEncodeCommandList->ResolveMotionVectorHeap(&outputArgs, &inputArgs);
		}

		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_previousYuv.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, D3D12_RESOURCE_STATE_COMMON);
			m_videoEncodeCommandList->ResourceBarrier(1, &barrier);
		}
		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_currentYuv.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_READ, D3D12_RESOURCE_STATE_COMMON);
			m_videoEncodeCommandList->ResourceBarrier(1, &barrier);
		}
		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_motionVectors.Get(), D3D12_RESOURCE_STATE_VIDEO_ENCODE_WRITE, D3D12_RESOURCE_STATE_COMMON);
			m_videoEncodeCommandList->ResourceBarrier(1, &barrier);
		}

		DX::ThrowIfFailed(m_videoEncodeCommandList->Close());

		{
			ID3D12CommandList* ppCommandLists[] = { m_videoEncodeCommandList.Get() };
			m_deviceResources->GetVideoQueue()->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
		}
		m_deviceResources->WaitForGpuOnVideoQueue();

		// Reopen the graphics command list
		DX::ThrowIfFailed(m_deviceResources->GetDirectCommandAllocator()->Reset());
		DX::ThrowIfFailed(m_commandList->Reset(m_deviceResources->GetDirectCommandAllocator(), nullptr));

		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_dlssTarget.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			m_commandList->ResourceBarrier(1, &barrier);
		}

		NVSDK_NGX_Result Status{};

		NVSDK_NGX_D3D12_DLSS_Eval_Params dlssEvalParams{};
		dlssEvalParams.Feature.pInColor = m_deviceResources->GetIntermediateRenderTarget();
		dlssEvalParams.Feature.pInOutput = m_dlssTarget.Get(); // Looks like you can't upscale directly to a swapchain target.
		dlssEvalParams.pInDepth = m_deviceResources->GetDepthStencil();
		dlssEvalParams.Feature.InSharpness = m_dlssSharpness;
		dlssEvalParams.pInMotionVectors = m_motionVectors.Get();
		dlssEvalParams.pInExposureTexture = nullptr;
		dlssEvalParams.pInBiasCurrentColorMask = nullptr;
		dlssEvalParams.InJitterOffsetX = 0;
		dlssEvalParams.InJitterOffsetY = 0;
		dlssEvalParams.Feature.InSharpness = m_dlssSharpness;
		dlssEvalParams.InReset = m_dlssReset;
		dlssEvalParams.InMVScaleX = 1.0f;
		dlssEvalParams.InMVScaleY = 1.0f;
		dlssEvalParams.InRenderSubrectDimensions.Width = g_scaling_sourceWidth;
		dlssEvalParams.InRenderSubrectDimensions.Height = g_scaling_sourceHeight;

		Status = NGX_D3D12_EVALUATE_DLSS_EXT(
			m_commandList.Get(),
			m_dlssFeatureHandle,
			m_ngxParameters,
			&dlssEvalParams);

		DX::ThrowIfNGXFailed(Status);

		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_dlssTarget.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			m_commandList->ResourceBarrier(1, &barrier);
		}

		m_commandList->CopyResource(m_deviceResources->GetSwapChainRenderTarget(), m_dlssTarget.Get());

		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_deviceResources->GetSwapChainRenderTarget(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
			m_commandList->ResourceBarrier(1, &barrier);
		}

		// Copy current motion vectors to previous
		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_currentYuv.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
			m_commandList->ResourceBarrier(1, &barrier);
		}
		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_previousYuv.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
			m_commandList->ResourceBarrier(1, &barrier);
		}
		m_commandList->CopyResource(m_previousYuv.Get(), m_currentYuv.Get());
		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_currentYuv.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
			m_commandList->ResourceBarrier(1, &barrier);
		}
		{
			CD3DX12_RESOURCE_BARRIER barrier =
				CD3DX12_RESOURCE_BARRIER::Transition(m_previousYuv.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
			m_commandList->ResourceBarrier(1, &barrier);
		}


		DX::ThrowIfFailed(m_commandList->Close());

		// Execute the command list.
		ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
		m_deviceResources->GetCommandQueue()->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

		m_deviceResources->Present();
	}

	return true;
}

void Sample3DSceneRenderer::UpdateWindowTitleText()
{
	wchar_t const* titleText = L"";
	switch (m_scalingType)
	{
	case ScalingType::Point: titleText = L"Scaling type: Point"; break;
	case ScalingType::Linear: titleText = L"Scaling type: Linear"; break;
	case ScalingType::DLSS: titleText = L"Scaling type: DLSS"; break;
	default:
		assert(false);
		titleText = L"Scaling type: <error>";
	}
	SetWindowText(m_deviceResources->GetWindow(), titleText);
}

void Sample3DSceneRenderer::OnPressSpaceKey()
{
	m_isSpinning = !m_isSpinning;
}

void Sample3DSceneRenderer::OnPressLeftKey()
{
	if (m_scalingType == ScalingType::Point)
	{
		m_scalingType = static_cast<ScalingType>(static_cast<int>(ScalingType::NumScalingTypes) - 1); // The last one
	}
	else
	{
		m_scalingType = static_cast<ScalingType>((int)m_scalingType - 1);
	}
	UpdateWindowTitleText();
}

void Sample3DSceneRenderer::OnPressRightKey()
{
	if (static_cast<int>(m_scalingType) == static_cast<int>(ScalingType::NumScalingTypes) - 1)
	{
		m_scalingType = ScalingType::Point; // The first one
	}
	else
	{
		m_scalingType = static_cast<ScalingType>((int)m_scalingType + 1);
	}

	UpdateWindowTitleText();
}