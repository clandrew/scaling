#pragma once

#include "DeviceResources.h"
#include "ShaderStructures.h"
#include "StepTimer.h"

namespace scaling
{
	enum class ScalingType
	{
		Point,
		Linear,
		DLSS,
		XeSS,
		NumScalingTypes
	};


	// This sample renderer instantiates a basic rendering pipeline.
	class Sample3DSceneRenderer
	{
	public:
		Sample3DSceneRenderer(const std::shared_ptr<DX::DeviceResources>& deviceResources);
		~Sample3DSceneRenderer();
		void CreateDeviceDependentResources();
		void CreateTargetSizeDependentResources();
		void Update(DX::StepTimer const& timer);
		bool RenderAndPresent();
		void OnPressSpaceKey();
		void OnPressLeftKey();
		void OnPressRightKey();
		void OnPressUKey();

	private:
		void Rotate(float radians);
		void UpdateWindowTitleText();
		void EvaluateMotionVectors();
		void CopyCurrentMotionVectorsToPrevious();

	private:
		// Constant buffers must be 256-byte aligned.
		static const UINT c_alignedConstantBufferSize = (sizeof(ModelViewProjectionConstantBuffer) + 255) & ~255;

		// Cached pointer to device resources.
		std::shared_ptr<DX::DeviceResources> m_deviceResources;

		// Direct3D resources for cube geometry.
		Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList>	m_commandList;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>		m_srvHeap;
		Microsoft::WRL::ComPtr<ID3D12Resource>				m_constantBuffer;
		ModelViewProjectionConstantBuffer					m_constantBufferData;
		UINT8*												m_mappedConstantBuffer;
		UINT												m_cbvDescriptorSize;

		Microsoft::WRL::ComPtr<ID3D12RootSignature>			m_commonGraphicsRootSignature;
		Microsoft::WRL::ComPtr<ID3D12RootSignature>			m_commonComputeRootSignature;

		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>		m_cbvSrvHeap;

		D3D12_RECT											m_pass1ScissorRect;
		Microsoft::WRL::ComPtr<ID3D12PipelineState>			m_pass1PipelineState;
		Microsoft::WRL::ComPtr<ID3D12Resource>				m_spinningCubeVertexBuffer;
		Microsoft::WRL::ComPtr<ID3D12Resource>				m_spinningCubeIndexBuffer;
		D3D12_VERTEX_BUFFER_VIEW							m_spinningCubeVertexBufferView;
		D3D12_INDEX_BUFFER_VIEW								m_spinningCubeIndexBufferView;

		D3D12_RECT											m_pass2ScissorRect;
		Microsoft::WRL::ComPtr<ID3D12PipelineState>			m_pass2_TexturedQuad_PipelineState;
		Microsoft::WRL::ComPtr<ID3D12Resource>				m_texturedQuadVertexBuffer;
		Microsoft::WRL::ComPtr<ID3D12Resource>				m_texturedQuadIndexBuffer;
		D3D12_VERTEX_BUFFER_VIEW							m_texturedQuadVertexBufferView;
		D3D12_INDEX_BUFFER_VIEW								m_texturedQuadIndexBufferView;

		// Variables used with the rendering loop.
		bool	m_loadingComplete;
		float	m_radiansPerSecond;
		float	m_angle;
		bool	m_tracking;

		ScalingType m_scalingType;
		bool m_isSpinning;
		bool m_isUpdating;

		Microsoft::WRL::ComPtr<ID3D12Resource>				 m_upscaledTarget;
		Microsoft::WRL::ComPtr<ID3D12VideoEncodeCommandList> m_videoEncodeCommandList;
		Microsoft::WRL::ComPtr<ID3D12VideoMotionEstimator>   m_videoMotionEstimator;
		Microsoft::WRL::ComPtr<ID3D12VideoMotionVectorHeap>  m_videoMotionVectorHeap;
		Microsoft::WRL::ComPtr<ID3D12PipelineState>		     m_pass2_YuvConversion_PipelineState;
		Microsoft::WRL::ComPtr<ID3D12Resource>				 m_motionVectors;
		Microsoft::WRL::ComPtr<ID3D12Resource>				 m_currentYuv;
		Microsoft::WRL::ComPtr<ID3D12Resource>				 m_previousYuv;

		// DLSS-related things
		bool                                                 m_dlssSupported;
		NVSDK_NGX_Parameter*                                 m_ngxParameters{};
		NVSDK_NGX_Handle*                                    m_dlssFeatureHandle;
		float                                                m_dlssSharpness;
		int												     m_dlssReset;

		// XeSS-related things
		xess_context_handle_t								 m_xessContext = nullptr;
	};
}

