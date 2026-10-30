/*
	Copyright 2025 flyinghead

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
#include "settings.h"
#include "gui.h"
#include "wsi/context.h"

void gui_settings_video()
{
	// Deadcast: Vulkan is the only renderer: no API to choose, no per-pixel sorting
    float innerSpacing = ImGui::GetStyle().ItemInnerSpacing.x;
    header(T("Transparent Sorting"));
    {
    	int renderer = config::PerStripSorting ? 1 : 0;
    	ImGui::Columns(2, "renderers", false);
    	ImGui::RadioButton(T("Per Triangle"), &renderer, 0);
        ImGui::SameLine();
        ShowHelpMarker(T("Sort transparent polygons per triangle. Fast but may produce graphical glitches"));
    	ImGui::NextColumn();
    	ImGui::RadioButton(T("Per Strip"), &renderer, 1);
        ImGui::SameLine();
        ShowHelpMarker(T("Sort transparent polygons per strip. Faster but may produce graphical glitches"));
    	ImGui::Columns(1, nullptr, false);
    	config::PerStripSorting.set(renderer == 1);
    }
	ImGui::Spacing();

    header(T("Rendering Options"));
    {
        const std::array<float, 13> scalings{ 0.5f, 1.f, 1.5f, 2.f, 2.5f, 3.f, 4.f, 4.5f, 5.f, 6.f, 7.f, 8.f, 9.f };
        const std::array<std::string, 13> scalingsText{ T("Half"), T("Native"), "x1.5", "x2", "x2.5", "x3", "x4", "x4.5", "x5", "x6", "x7", "x8", "x9" };
        std::array<int, scalings.size()> vres;
        std::array<std::string, scalings.size()> resLabels;
        u32 selected = 0;
        for (u32 i = 0; i < scalings.size(); i++)
        {
        	vres[i] = scalings[i] * 480;
        	if (vres[i] == config::RenderResolution)
        		selected = i;
        	if (!config::Widescreen)
        		resLabels[i] = std::to_string((int)(scalings[i] * 640)) + "x" + std::to_string((int)(scalings[i] * 480));
        	else
        		resLabels[i] = std::to_string((int)(scalings[i] * 480 * 16 / 9)) + "x" + std::to_string((int)(scalings[i] * 480));
        	resLabels[i] += " (" + scalingsText[i] + ")";
        }

        ImGui::PushItemWidth(ImGui::CalcItemWidth() - innerSpacing * 2.0f - ImGui::GetFrameHeight() * 2.0f);
        if (ImGui::BeginCombo("##Resolution", resLabels[selected].c_str(), ImGuiComboFlags_NoArrowButton))
        {
        	for (u32 i = 0; i < scalings.size(); i++)
            {
                bool is_selected = vres[i] == config::RenderResolution;
                if (ImGui::Selectable(resLabels[i].c_str(), is_selected))
                	config::RenderResolution = vres[i];
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine(0, innerSpacing);

        if (ImGui::ArrowButton("##Decrease Res", ImGuiDir_Left))
        {
            if (selected > 0)
            	config::RenderResolution = vres[selected - 1];
        }
        ImGui::SameLine(0, innerSpacing);
        if (ImGui::ArrowButton("##Increase Res", ImGuiDir_Right))
        {
            if (selected < vres.size() - 1)
            	config::RenderResolution = vres[selected + 1];
        }
        ImGui::SameLine(0, innerSpacing);

        ImGui::Text("%s", T("Internal Resolution"));
        ImGui::SameLine();
        ShowHelpMarker(T("Internal render resolution. Higher is better, but more demanding on the GPU. Values higher than your display resolution (but no more than double your display resolution) can be used for supersampling, which provides high-quality antialiasing without reducing sharpness."));
		OptionCheckbox(T("Integer Scaling"), config::IntegerScale, T("Scales the output by the maximum integer multiple allowed by the display resolution."));
		OptionCheckbox(T("Linear Interpolation"), config::LinearInterpolation, T("Scales the output with linear interpolation. Will use nearest neighbor interpolation otherwise. Disable with integer scaling."));
#ifndef TARGET_IPHONE
    	OptionCheckbox(T("VSync"), config::VSync, T("Synchronizes the frame rate with the screen refresh rate. Recommended"));
    	ImGui::Indent();
		{
			DisabledScope scope(!config::VSync);
#ifdef __ANDROID__
			OptionCheckbox(T("Frame Pacing (Experimental)"), config::FramePacing,
					T("Provide the GPU with presentation timing for each frame to replicate original frame pacing."));
#else
	    	if (isVulkan(config::RendererType))
	    		OptionCheckbox(T("Duplicate frames"), config::DupeFrames, T("Duplicate frames on high refresh rate monitors (120 Hz and higher)"));
#endif
    	}
    	ImGui::Unindent();
#endif
    	OptionCheckbox(T("Show VMU In-game"), config::FloatVMUs, T("Show the VMU LCD screens while in-game"));
    	OptionCheckbox(T("Full Framebuffer Emulation"), config::EmulateFramebuffer,
    			T("Fully accurate VRAM framebuffer emulation. Helps games that directly access the framebuffer for special effects. "
    			"Very slow and incompatible with upscaling and wide screen."));
		{
			DisabledScope scope(game_started);
			OptionCheckbox(T("Load Custom Textures"), config::CustomTextures,
					T("Load custom/high-res textures from data/textures/<game id>"));
			ImGui::Indent();
			{
				DisabledScope scope(!config::CustomTextures.get());
				OptionCheckbox(T("Preload Custom Textures"), config::PreloadCustomTextures,
						T("Preload custom textures at game start. May improve performance but increases memory usage"));
			}
			ImGui::Unindent();
		}
    }
	ImGui::Spacing();
    header(T("Aspect Ratio"));
    {
    	OptionCheckbox(T("Widescreen"), config::Widescreen,
    			T("Draw geometry outside of the normal 4:3 aspect ratio. May produce graphical glitches in the revealed areas.\nAspect Fit and shows the full 16:9 content."));
		{
			DisabledScope scope(!config::Widescreen || config::IntegerScale);

			ImGui::Indent();
			OptionCheckbox(T("Super Widescreen"), config::SuperWidescreen,
					T("Use the full width of the screen or window when its aspect ratio is greater than 16:9.\nAspect Fill and remove black bars. Not compatible with integer scaling."));
			ImGui::Unindent();
    	}
    	OptionCheckbox(T("Widescreen Game Cheats"), config::WidescreenGameHacks,
    			T("Modify the game so that it displays in 16:9 anamorphic format and use horizontal screen stretching. Only some games are supported."));
    	OptionSlider(T("Horizontal Stretching"), config::ScreenStretching, 100, 250,
    			T("Stretch the screen horizontally"), "%d%%");
    	OptionCheckbox(T("Rotate Screen 90°"), config::Rotate90, T("Rotate the screen 90° counterclockwise"));
    }
	ImGui::Spacing();
    header(T("Performance"));
    {
    	ImGui::Text("%s", T("Automatic Frame Skipping:"));
    	ImGui::Columns(3, "autoskip", false);
    	OptionRadioButton(T("Disabled"), config::AutoSkipFrame, 0, T("No frame skipping"));
    	ImGui::NextColumn();
    	OptionRadioButton(T("Normal"), config::AutoSkipFrame, 1, T("Skip a frame when the GPU and CPU are both running slow"));
    	ImGui::NextColumn();
    	OptionRadioButton(T("Maximum"), config::AutoSkipFrame, 2, T("Skip a frame when the GPU is running slow"));
    	ImGui::Columns(1, nullptr, false);

    	OptionArrowButtons(T("Frame Skipping"), config::SkipFrame, 0, 6,
    			T("Number of frames to skip between two actually rendered frames"));
    	OptionCheckbox(T("Shadows"), config::ModifierVolumes,
    			T("Enable modifier volumes, usually used for shadows"));
    	OptionCheckbox(T("Fog"), config::Fog, T("Enable fog effects"));
    }
    ImGui::Spacing();
	header(T("Advanced"));
    {
    	OptionCheckbox(T("Delay Frame Swapping"), config::DelayFrameSwapping,
    			T("Useful to avoid flashing screen or glitchy videos. Not recommended on slow platforms"));
    	OptionCheckbox(T("Fix Upscale Bleeding Edge"), config::FixUpscaleBleedingEdge,
    			T("Helps with texture bleeding case when upscaling. Disabling it can help if pixels are warping when upscaling in 2D games (MVC2, CVS, KOF, etc.)"));
    	OptionCheckbox(T("Native Depth Interpolation"), config::NativeDepthInterpolation,
    			T("Helps with texture corruption and depth issues on AMD GPUs. Can also help Intel GPUs in some cases."));
    	OptionCheckbox(T("Copy Rendered Textures to VRAM"), config::RenderToTextureBuffer,
    			T("Copy rendered-to textures back to VRAM. Slower but accurate"));
		const std::array<int, 5> aniso{ 1, 2, 4, 8, 16 };
        const std::array<std::string, 5> anisoText{ T("Disabled"), "2x", "4x", "8x", "16x" };
        u32 afSelected = 0;
        for (u32 i = 0; i < aniso.size(); i++)
        {
        	if (aniso[i] == config::AnisotropicFiltering)
        		afSelected = i;
        }

        ImGui::PushItemWidth(ImGui::CalcItemWidth() - innerSpacing * 2.0f - ImGui::GetFrameHeight() * 2.0f);
        if (ImGui::BeginCombo("##Anisotropic Filtering", anisoText[afSelected].c_str(), ImGuiComboFlags_NoArrowButton))
        {
        	for (u32 i = 0; i < aniso.size(); i++)
            {
                bool is_selected = aniso[i] == config::AnisotropicFiltering;
                if (ImGui::Selectable(anisoText[i].c_str(), is_selected))
                	config::AnisotropicFiltering = aniso[i];
                if (is_selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine(0, innerSpacing);

        if (ImGui::ArrowButton("##Decrease Anisotropic Filtering", ImGuiDir_Left))
        {
            if (afSelected > 0)
            	config::AnisotropicFiltering = aniso[afSelected - 1];
        }
        ImGui::SameLine(0, innerSpacing);
        if (ImGui::ArrowButton("##Increase Anisotropic Filtering", ImGuiDir_Right))
        {
            if (afSelected < aniso.size() - 1)
            	config::AnisotropicFiltering = aniso[afSelected + 1];
        }
        ImGui::SameLine(0, innerSpacing);

        ImGui::Text("%s", T("Anisotropic Filtering"));
        ImGui::SameLine();
        ShowHelpMarker(T("Higher values make textures viewed at oblique angles look sharper, but are more demanding on the GPU. This option only has a visible impact on mipmapped textures."));

    	ImGui::Text("%s", T("Texture Filtering:"));
    	ImGui::Columns(3, "textureFiltering", false);
    	OptionRadioButton(T("Default"), config::TextureFiltering, 0, T("Use the game's default texture filtering"));
    	ImGui::NextColumn();
    	OptionRadioButton(T("Force Nearest-Neighbor"), config::TextureFiltering, 1, T("Force nearest-neighbor filtering for all textures. Crisper appearance, but may cause various rendering issues. This option usually does not affect performance."));
    	ImGui::NextColumn();
    	OptionRadioButton(T("Force Linear"), config::TextureFiltering, 2, T("Force linear filtering for all textures. Smoother appearance, but may cause various rendering issues. This option usually does not affect performance."));
    	ImGui::Columns(1, nullptr, false);

    	OptionCheckbox(T("Show FPS Counter"), config::ShowFPS, T("Show on-screen frame/sec counter"));
    }
}
