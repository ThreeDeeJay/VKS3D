#pragma once
typedef struct StereoDevice StereoDevice;
typedef struct StereoSwapchain StereoSwapchain;
bool dxgi_device_init(StereoDevice *sd);
void dxgi_stereo_activate(StereoDevice *sd); 
void dxgi_device_destroy(StereoDevice *sd);
bool dxgi_sc_create(StereoDevice *sd, StereoSwapchain *sc, HANDLE *out_nt_handle);
void dxgi_sc_destroy(StereoSwapchain *sc);
VkResult dxgi_copy_and_present(StereoDevice *sd, StereoSwapchain *sc);
