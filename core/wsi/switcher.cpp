/*
    Created on: Oct 18, 2019

	Copyright 2019 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#ifndef LIBRETRO
#include "context.h"
#include "cfg/option.h"
#include "oslib/i18n.h"

#include "rend/vulkan/vulkan_context.h"

// Deadcast: Vulkan is the only graphics API
void initRenderApi(void *window, void *display)
{
	config::RendererType = RenderType::Vulkan;
	try {
		VulkanContext::Create(window, display);
	} catch (const std::exception& e) {
		ERROR_LOG(RENDERER, "Vulkan init failed: %s", e.what());
		throw FlycastException(i18n::T("Cannot initialize the graphics API"));
	}
}

void termRenderApi() {
	GraphicsContext::Term();
}

void switchRenderApi()
{
	void *window = nullptr;
	void *display = nullptr;
	if (GraphicsContext::Instance() != nullptr)
		GraphicsContext::Instance()->getWindow(&window,  &display);
	termRenderApi();
	initRenderApi(window, display);
}
#endif
