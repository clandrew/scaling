#pragma once

namespace DX
{
	static const UINT c_frameCount = 3;		// Use triple buffering.

	struct SizeF
	{
		float Width;
		float Height;
	};
	struct SizeI
	{
		int Width;
		int Height;
	};
	struct SizeU
	{
		unsigned int Width;
		unsigned int Height;
	};

	// Controls all the DirectX device resources.
	class DeviceResources
	{
	public:
		DeviceResources(DXGI_FORMAT backBufferFormat = DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT depthBufferFormat = DXGI_FORMAT_D32_FLOAT);
		void SetWindow(HWND window);
		void SetLogicalSize(SizeF logicalSize);
		void ValidateDevice();
		void Present();
		void WaitForGpu();

		bool						IsDeviceRemoved() const				{ return m_deviceRemoved; }

		HWND						GetWindow() const					{ return m_window; }

		// D3D Accessors.
		ID3D12Device*				GetD3DDevice() const				{ return m_d3dDevice.Get(); }
		IDXGISwapChain3*			GetSwapChain() const				{ return m_swapChain.Get(); }

		ID3D12Resource*				GetIntermediateRenderTarget() const { return m_intermediateRenderTarget.Get(); }
		ID3D12Resource*				GetSwapChainRenderTarget() const	{ return m_swapChainRenderTargets[m_currentFrame].Get(); }

		ID3D12Resource*				GetDepthStencil() const				{ return m_depthStencil.Get(); }
		ID3D12CommandQueue*			GetCommandQueue() const				{ return m_commandQueue.Get(); }
		ID3D12CommandAllocator*		GetCommandAllocator() const			{ return m_commandAllocators[m_currentFrame].Get(); }
		DXGI_FORMAT					GetBackBufferFormat() const			{ return m_backBufferFormat; }
		DXGI_FORMAT					GetDepthBufferFormat() const		{ return m_depthBufferFormat; }

		D3D12_VIEWPORT				GetPass1Viewport() const			{ return m_pass1Viewport; }
		D3D12_VIEWPORT				GetPass2Viewport() const			{ return m_pass2Viewport; }
		
		DirectX::XMFLOAT4X4			GetOrientationTransform3D() const	{ return m_orientationTransform3D; }
		UINT						GetCurrentFrameIndex() const		{ return m_currentFrame; }

		CD3DX12_CPU_DESCRIPTOR_HANDLE GetIntermediateRenderTargetCpuDescriptor() const
		{
			return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), c_frameCount, m_rtvDescriptorSize);
		}

		CD3DX12_CPU_DESCRIPTOR_HANDLE GetSwapChainRenderTargetCpuDescriptor() const
		{
			return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_rtvHeap->GetCPUDescriptorHandleForHeapStart(), m_currentFrame, m_rtvDescriptorSize);
		}
		CD3DX12_CPU_DESCRIPTOR_HANDLE GetDepthStencilView() const
		{
			return CD3DX12_CPU_DESCRIPTOR_HANDLE(m_dsvHeap->GetCPUDescriptorHandleForHeapStart());
		}

	private:
		void CreateDeviceIndependentResources();
		void CreateDeviceResources();
		void CreateTargetSizeDependentResources();
		void MoveToNextFrame();
		void GetHardwareAdapter(IDXGIAdapter1** ppAdapter);

		UINT											m_currentFrame;

		// Direct3D objects.
		Microsoft::WRL::ComPtr<ID3D12Device>			m_d3dDevice;
		Microsoft::WRL::ComPtr<IDXGIFactory4>			m_dxgiFactory;
		Microsoft::WRL::ComPtr<IDXGISwapChain3>			m_swapChain;

		Microsoft::WRL::ComPtr<ID3D12Resource>			m_intermediateRenderTarget;
		Microsoft::WRL::ComPtr<ID3D12Resource>			m_swapChainRenderTargets[c_frameCount];

		Microsoft::WRL::ComPtr<ID3D12Resource>			m_depthStencil;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>	m_rtvHeap;
		Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>	m_dsvHeap;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue>		m_commandQueue;
		Microsoft::WRL::ComPtr<ID3D12CommandAllocator>	m_commandAllocators[c_frameCount];
		Microsoft::WRL::ComPtr<ID3D12CommandAllocator>  m_videoEncodeCommandAllocator;
		DXGI_FORMAT										m_backBufferFormat;
		DXGI_FORMAT										m_depthBufferFormat;

		D3D12_VIEWPORT									m_pass1Viewport;
		D3D12_VIEWPORT									m_pass2Viewport;
		
		UINT											m_rtvDescriptorSize;
		bool											m_deviceRemoved;

		// CPU/GPU Synchronization.
		Microsoft::WRL::ComPtr<ID3D12Fence>				m_fence;
		UINT64											m_fenceValues[c_frameCount];
		HANDLE											m_fenceEvent;

		// Cached reference to the Window.
		HWND											m_window;

		// Transforms used for display orientation.
		DirectX::XMFLOAT4X4								m_orientationTransform3D;
	};
}
