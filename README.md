# Scaling toy application

This is a quick test application for demoing setting up two types of AI-based upscaling:
* [NVIDIA DLSS](https://developer.nvidia.com/rtx/dlss/get-started)
* [Intel XeSS](https://github.com/intel/xess/releases/tag/v1.1.0)

using the respective public SDKs linked above.

It's for my own quick testing purposes. The geometry is really simple- a spinning cube. It uses video motion estimation to apply motion vectors.

![Example image](https://raw.githubusercontent.com/clandrew/spinningcube12/master/Images/Image.gif "Example image.")

## Controls

* **Left and right keys**: Selects between the four rendering options, where the current one appears in the title bar
  * Point sampling
  * Linear Sampling
  * DLSS
  * XeSS
* **Space**: Toggles the spinning animation of the cube.
* **'U' key**: Toggles updating of the AI evaluation buffer. Only applicable to DLSS and XeSS above. 

## Build
The source code is organized as a Visual Studio 2019 built for x86-64 architecture. It uses the v142 toolset.

Shaders are compiled at build time as part of the solution against shader model 6_0. 
