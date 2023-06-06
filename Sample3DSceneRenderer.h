#pragma once

#include "DeviceResources.h"
#include "ShaderStructures.h"
#include "StepTimer.h"

namespace scaling
{
	// This sample renderer instantiates a basic rendering pipeline.
	class Sample3DSceneRenderer
	{
	public:
		Sample3DSceneRenderer(const std::shared_ptr<DX::DeviceResources>& deviceResources);
		~Sample3DSceneRenderer();
		void CreateDeviceDependentResources();
		void CreateTargetSizeDependentResources();
		void Update(DX::StepTimer const& timer);
		bool Render();

	private:
		void Rotate(float radians);

	private:
		// Constant buffers must be 256-byte aligned.
		static const UINT c_alignedConstantBufferSize = (sizeof(ModelViewProjectionConstantBuffer) + 255) & ~255;

		// Cached pointer to device resources.
		std::shared_ptr<DX::DeviceResources> m_deviceResources;

		// Direct3D resources for cube geometry.
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>	m_commandList;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>		m_cbvHeap;
		Microsoft::WRL::ComPtr<ID3D12Resource>				m_constantBuffer;
		ModelViewProjectionConstantBuffer					m_constantBufferData;
		UINT8*												m_mappedConstantBuffer;
		UINT												m_cbvDescriptorSize;

		Microsoft::WRL::ComPtr<ID3D12RootSignature>			m_pass1RootSignature;
		D3D12_RECT											m_pass1ScissorRect;
		Microsoft::WRL::ComPtr<ID3D12PipelineState>			m_pass1PipelineState;
		Microsoft::WRL::ComPtr<ID3D12Resource>				m_spinningCubeVertexBuffer;
		Microsoft::WRL::ComPtr<ID3D12Resource>				m_spinningCubeIndexBuffer;
		D3D12_VERTEX_BUFFER_VIEW							m_spinningCubeVertexBufferView;
		D3D12_INDEX_BUFFER_VIEW								m_spinningCubeIndexBufferView;

		Microsoft::WRL::ComPtr<ID3D12RootSignature>			m_pass2RootSignature;
		D3D12_RECT											m_pass2ScissorRect;
		Microsoft::WRL::ComPtr<ID3D12PipelineState>			m_pass2PipelineState;
		Microsoft::WRL::ComPtr<ID3D12Resource>				m_texturedQuadVertexBuffer;
		Microsoft::WRL::ComPtr<ID3D12Resource>				m_texturedQuadIndexBuffer;
		D3D12_VERTEX_BUFFER_VIEW							m_texturedQuadVertexBufferView;
		D3D12_INDEX_BUFFER_VIEW								m_texturedQuadIndexBufferView;

		// Variables used with the rendering loop.
		bool	m_loadingComplete;
		float	m_radiansPerSecond;
		float	m_angle;
		bool	m_tracking;
	};
}

