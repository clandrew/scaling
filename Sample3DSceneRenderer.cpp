#include "pch.h"
#include "Sample3DSceneRenderer.h"

#include "DirectXHelper.h"

#include "Pass1VS.h"
#include "Pass1PS.h"
#include "Pass2VS.h"
#include "Pass2PS.h"

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
	m_deviceResources(deviceResources)
{
	ZeroMemory(&m_constantBufferData, sizeof(m_constantBufferData));

	CreateDeviceDependentResources();
	CreateTargetSizeDependentResources();
}

Sample3DSceneRenderer::~Sample3DSceneRenderer()
{
	m_constantBuffer->Unmap(0, nullptr);
	m_mappedConstantBuffer = nullptr;
}

void Sample3DSceneRenderer::CreateDeviceDependentResources()
{
	auto d3dDevice = m_deviceResources->GetD3DDevice();

	{
		CD3DX12_DESCRIPTOR_RANGE ranges[2];
		CD3DX12_ROOT_PARAMETER parameter;

		{
			UINT numDescriptors = DX::c_frameCount;
			UINT baseShaderRegister = 0;
			ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, numDescriptors, baseShaderRegister);
		}
		{
			UINT numDescriptors = 1;
			UINT baseShaderRegister = 0;
			ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, numDescriptors, baseShaderRegister);
		}
		parameter.InitAsDescriptorTable(_countof(ranges), ranges, D3D12_SHADER_VISIBILITY_ALL);

		D3D12_ROOT_SIGNATURE_FLAGS rootSignatureFlags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | // Only the input assembler stage needs access to the constant buffer.
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;

		D3D12_STATIC_SAMPLER_DESC sampler = {};
		sampler.Filter = D3D12_FILTER_MIN_MAG_POINT_MIP_LINEAR;
		sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
		sampler.MipLODBias = 0;
		sampler.MaxAnisotropy = 0;
		sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
		sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		sampler.MinLOD = 0.0f;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.ShaderRegister = 0;
		sampler.RegisterSpace = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		CD3DX12_ROOT_SIGNATURE_DESC descRootSignature;
		descRootSignature.Init(1, &parameter, 1u, &sampler, rootSignatureFlags);

		ComPtr<ID3DBlob> pSignature;
		ComPtr<ID3DBlob> pError;
		DX::ThrowIfFailed(D3D12SerializeRootSignature(&descRootSignature, D3D_ROOT_SIGNATURE_VERSION_1, pSignature.GetAddressOf(), pError.GetAddressOf()));
		DX::ThrowIfFailed(d3dDevice->CreateRootSignature(0, pSignature->GetBufferPointer(), pSignature->GetBufferSize(), IID_PPV_ARGS(&m_commonRootSignature)));
        NAME_D3D12_OBJECT(m_commonRootSignature);
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
		state.pRootSignature = m_commonRootSignature.Get();
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
		state.pRootSignature = m_commonRootSignature.Get();
		state.VS = CD3DX12_SHADER_BYTECODE((void*)(g_Pass2VS), _countof(g_Pass2VS));
		state.PS = CD3DX12_SHADER_BYTECODE((void*)(g_Pass2PS), _countof(g_Pass2PS));
		state.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		state.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		state.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		state.SampleMask = UINT_MAX;
		state.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		state.NumRenderTargets = 1;
		state.RTVFormats[0] = m_deviceResources->GetBackBufferFormat();
		state.DSVFormat = DXGI_FORMAT_UNKNOWN;
		state.SampleDesc.Count = 1;

		DX::ThrowIfFailed(m_deviceResources->GetD3DDevice()->CreateGraphicsPipelineState(&state, IID_PPV_ARGS(&m_pass2PipelineState)));
	};

	// Create and upload cube and quad geometry resources to the GPU.
	{
		auto d3dDevice = m_deviceResources->GetD3DDevice();

		// Create a command list.
		DX::ThrowIfFailed(d3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_deviceResources->GetCommandAllocator(), m_pass1PipelineState.Get(), IID_PPV_ARGS(&m_commandList)));
        NAME_D3D12_OBJECT(m_commandList);

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
				{ XMFLOAT3(-1, -1, 1), XMFLOAT2(0.0f, 0.0f) }, // Top left
				{ XMFLOAT3(1,  -1, 1), XMFLOAT2(1.0f, 0.0f) }, // Top right
				{ XMFLOAT3(-1,  1, 1), XMFLOAT2(0.0f, 1.0f) }, // Bottom left
				{ XMFLOAT3(1,   1, 1), XMFLOAT2(1.0f, 1.0f) }, // Bottom right
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
			D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
			heapDesc.NumDescriptors = DX::c_frameCount + 1;
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
		m_deviceResources->WaitForGpu();
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
	if (m_loadingComplete)
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
bool Sample3DSceneRenderer::Render()
{
	// Loading is asynchronous. Only draw geometry after it's loaded.
	if (!m_loadingComplete)
	{
		return false;
	}

	DX::ThrowIfFailed(m_deviceResources->GetCommandAllocator()->Reset());

	// The command list can be reset anytime after ExecuteCommandList() is called.
	DX::ThrowIfFailed(m_commandList->Reset(m_deviceResources->GetCommandAllocator(), nullptr));

	ID3D12DescriptorHeap* ppHeaps[] = { m_cbvSrvHeap.Get() };
	m_commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

	m_commandList->SetGraphicsRootSignature(m_commonRootSignature.Get());

	// Bind the current frame's constant buffer to the pipeline.
	CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(m_cbvSrvHeap->GetGPUDescriptorHandleForHeapStart(), 0, m_cbvDescriptorSize);
	m_commandList->SetGraphicsRootDescriptorTable(0, gpuHandle);

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
	{
		m_commandList->SetPipelineState(m_pass2PipelineState.Get());

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
	}

	DX::ThrowIfFailed(m_commandList->Close());

	// Execute the command list.
	ID3D12CommandList* ppCommandLists[] = { m_commandList.Get() };
	m_deviceResources->GetCommandQueue()->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

	return true;
}
