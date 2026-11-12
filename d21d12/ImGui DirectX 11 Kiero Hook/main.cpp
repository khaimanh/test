#include "includes.h"
#include "include/xorstr/xorstr.hpp"
#include <cmath>
#include "TextEditor.hpp"
#include "cfx/ref_container.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_SUPPORT_GIF
#include "stb_image.h"
#include "icons/icon.h"
#include "icons/font.h"
#include "icons/font_awesome.h"

#include <concurrent_queue.h>
#include <atomic>
#include <string>
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

Present oPresent;
HWND window = NULL;
WNDPROC oWndProc;
ID3D11Device* pDevice = NULL;
ID3D11DeviceContext* pContext = NULL;
ID3D11RenderTargetView* mainRenderTargetView;

static bool guiVisible = true;
static TextEditor editor;
static std::vector<ID3D11ShaderResourceView*> logoFrames;
static int logoFrameCount = 0;
static int* logoDelays = nullptr;
static float logoAnimTime = 0.0f;
static int currentLogoFrame = 0;
static concurrency::concurrent_queue<std::string> g_execQueue;
static std::atomic<bool> g_onTickHooked{ false };
static size_t g_onTickCookie = (size_t)-1;

// Debug state tracking
struct DebugState {
    std::atomic<int> queuedCount{ 0 };
    std::atomic<int> executedCount{ 0 };
    std::atomic<int> tickCallCount{ 0 };
    std::atomic<bool> resourceManagerValid{ false };
    std::atomic<bool> eventCompValid{ false };
    std::atomic<bool> hookAttempted{ false };
    std::atomic<bool> hookSucceeded{ false };
    char lastError[256] = { 0 };
};
static DebugState g_debug;

static bool HookResourceManagerTick()
{
    if (g_onTickHooked.load())
        return true;

    g_debug.hookAttempted = true;

    try
    {
        // Validate CFX modules are loaded
        HMODULE citizenCore = GetModuleHandleA("citizen-resources-core.dll");
        HMODULE coreRT = GetModuleHandleA("CoreRT.dll");

        if (!citizenCore)
        {
            strcpy_s(g_debug.lastError, "citizen-resources-core.dll not loaded");
            return false;
        }

        if (!coreRT)
        {
            strcpy_s(g_debug.lastError, "CoreRT.dll not loaded");
            return false;
        }

        auto resourceManager = fx::ResourceManager::getCurrent();
        if (!resourceManager)
        {
            strcpy_s(g_debug.lastError, "ResourceManager::getCurrent() returned null");
            return false;
        }

        g_debug.resourceManagerValid = true;

        g_onTickCookie = resourceManager->OnTick.Connect([]() -> bool
        {
            g_debug.tickCallCount++;

            auto rm = fx::ResourceManager::getCurrent();
            if (!rm)
            {
                g_debug.resourceManagerValid = false;
                return true;
            }
            g_debug.resourceManagerValid = true;

            auto eventComp = rm->getComp<fx::ResourceEventManagerComponent>();
            if (!eventComp)
            {
                g_debug.eventCompValid = false;
                return true;
            }
            g_debug.eventCompValid = true;

            std::string code;
            while (g_execQueue.try_pop(code))
            {
                try
                {
                    // Trigger the event using operator() - OnQueueEvent is an fwEvent, not a method
                    // The event signature is: fwEvent<const std::string&, const std::string&, const std::string&>
                    // Parameters are: resourceName, eventName, eventPayload

                    bool eventResult = false;

                    // Try triggering as a command fallback (how F8 console works)
                    eventResult = eventComp->OnQueueEvent("__cfx_internal:commandFallback", code, "");

                    if (eventResult)
                    {
                        g_debug.executedCount++;
                        strcpy_s(g_debug.lastError, "Command executed via __cfx_internal:commandFallback");
                    }
                    else
                    {
                        // If no listeners, try alternative: trigger as runcode event
                        eventResult = eventComp->OnQueueEvent("", "runcode", code);

                        if (eventResult)
                        {
                            g_debug.executedCount++;
                            strcpy_s(g_debug.lastError, "Command executed via runcode event");
                        }
                        else
                        {
                            strcpy_s(g_debug.lastError, "Event returned false - no listeners registered");
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    snprintf(g_debug.lastError, sizeof(g_debug.lastError), "OnQueueEvent exception: %s", e.what());
                }
                catch (...)
                {
                    strcpy_s(g_debug.lastError, "Unknown exception in OnQueueEvent");
                }
            }

            return true;
        });

        if (g_onTickCookie != (size_t)-1)
        {
            g_onTickHooked = true;
            g_debug.hookSucceeded = true;
            strcpy_s(g_debug.lastError, "Hook connected successfully");
        }
        else
        {
            strcpy_s(g_debug.lastError, "OnTick.Connect returned invalid cookie");
        }

        return g_onTickHooked.load();
    }
    catch (const std::exception& e)
    {
        snprintf(g_debug.lastError, sizeof(g_debug.lastError), "Exception: %s", e.what());
        return false;
    }
    catch (...)
    {
        strcpy_s(g_debug.lastError, "Unknown exception in HookResourceManagerTick");
        return false;
    }
}

static float buttonClickTime[4] = { -999.0f, -999.0f, -999.0f, -999.0f };

void ExecuteLuaCode(const std::string& code)
{
    g_execQueue.push(code);
    g_debug.queuedCount++;
}

bool ExecuteToResourceMonitor(const std::string& luaCode)
{
    auto resources = fx::ResourceManager::getCurrentIl()->getAllResources();
    
    fx::Resource* targetResource = nullptr;
    
    for (auto r : resources)
    {
        std::string name = r->get_impl()->GetName();
        if (name.find("monitor") != std::string::npos)
        {
            targetResource = r.GetRef();
            break;
        }
    }
    
    if (!targetResource)
    {
        return false;
    }
    
    static std::string scriptToExecute;
    scriptToExecute = luaCode;
    
    static int callCount = 0;
    callCount = 0;
    
    size_t connHandle = targetResource->Runtime.Connect(
        [](std::vector<char>* info)
        {
            static std::string* scriptPtr = &scriptToExecute;
            if (callCount == 4)
            {
                std::string buffer = *scriptPtr + ";";
                info->insert(info->begin(), buffer.begin(), buffer.end());
            }
            callCount++;
        }
    );
    
    targetResource->StopResource();
    targetResource->StartResource();
    targetResource->Runtime.Disconnect(connHandle);
    
    return true;
}

bool ExecuteToResource(const std::string& luaCode, const std::string& resourceName)
{
    auto resources = fx::ResourceManager::getCurrentIl()->getAllResources();
    
    fx::Resource* targetResource = nullptr;
    
    for (auto r : resources)
    {
        if (r->get_impl()->GetName() == resourceName)
        {
            targetResource = r.GetRef();
            break;
        }
    }
    
    if (!targetResource)
    {
        return false;
    }
    
    static std::string scriptToExecute;
    scriptToExecute = luaCode;
    
    static int callCount = 0;
    callCount = 0;
    
    size_t connHandle = targetResource->Runtime.Connect(
        [](std::vector<char>* info)
        {
            static std::string* scriptPtr = &scriptToExecute;
            if (callCount == 4)
            {
                std::string buffer = *scriptPtr + ";";
                info->insert(info->begin(), buffer.begin(), buffer.end());
            }
            callCount++;
        }
    );
    
    targetResource->StopResource();
    targetResource->StartResource();
    targetResource->Runtime.Disconnect(connHandle);
    
    return true;
}

ImVec4 LerpColor(ImVec4 a, ImVec4 b, float t)
{
	return ImVec4(
		a.x + (b.x - a.x) * t,
		a.y + (b.y - a.y) * t,
		a.z + (b.z - a.z) * t,
		a.w + (b.w - a.w) * t
	);
}

bool AnimatedButton(const char* label, ImVec2 size, int buttonIndex)
{
	float currentTime = (float)ImGui::GetTime();
	float timeSinceClick = currentTime - buttonClickTime[buttonIndex];
	float animDuration = 0.3f;

	ImVec4 normalColor = ImVec4(14 / 255.0f, 14 / 255.0f, 16 / 255.0f, 1.0f);
	ImVec4 clickColor = ImVec4(231 / 255.0f, 42 / 255.0f, 43 / 255.0f, 1.0f);

	ImVec4 currentColor = normalColor;
	if (timeSinceClick < animDuration)
	{
		float t = 1.0f - (timeSinceClick / animDuration);
		currentColor = LerpColor(normalColor, clickColor, t);
	}

	ImGui::PushStyleColor(ImGuiCol_Button, currentColor);
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, currentColor);
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, currentColor);

	bool clicked = ImGui::Button(label, size);

	ImGui::PopStyleColor(3);

	if (clicked)
	{
		buttonClickTime[buttonIndex] = currentTime;
	}

	return clicked;
}

bool LoadAnimatedGIF(const unsigned char* data, size_t dataSize)
{
	int width, height, frames, channels;
	int* delays = nullptr;

	unsigned char* pixels = stbi_load_gif_from_memory(data, (int)dataSize, &delays, &width, &height, &frames, &channels, 4);

	if (!pixels || frames == 0)
	{
		if (pixels) stbi_image_free(pixels);
		if (delays) stbi_image_free(delays);
		return false;
	}

	logoFrameCount = frames;
	logoDelays = delays;
	logoFrames.clear();

	for (int i = 0; i < frames; i++)
	{
		unsigned char* frameData = pixels + (width * height * 4 * i);

		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = width;
		texDesc.Height = height;
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

		D3D11_SUBRESOURCE_DATA initData = {};
		initData.pSysMem = frameData;
		initData.SysMemPitch = width * 4;

		ID3D11Texture2D* texture = nullptr;
		pDevice->CreateTexture2D(&texDesc, &initData, &texture);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;

		ID3D11ShaderResourceView* srv = nullptr;
		pDevice->CreateShaderResourceView(texture, &srvDesc, &srv);
		logoFrames.push_back(srv);

		texture->Release();
	}

	stbi_image_free(pixels);

	return true;
}

void InitImGui()
{
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags = ImGuiConfigFlags_NoMouseCursorChange;
	ImGui_ImplWin32_Init(window);
	ImGui_ImplDX11_Init(pDevice, pContext);


	io.Fonts->AddFontFromMemoryTTF(inter_medium, sizeof(inter_medium), 16.0f);

	LoadAnimatedGIF(logo_anim2, sizeof(logo_anim2));
}

LRESULT __stdcall WndProc(const HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {

	if (uMsg == WM_KEYDOWN && wParam == VK_INSERT)
	{
		guiVisible = !guiVisible;
		return true;
	}

	if (guiVisible && ImGui_ImplWin32_WndProcHandler(hWnd, uMsg, wParam, lParam))
		return true;

	return CallWindowProc(oWndProc, hWnd, uMsg, wParam, lParam);
}

bool init = false;
HRESULT __stdcall hkPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags)
{
	if (!init)
	{
		if (SUCCEEDED(pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&pDevice)))
		{
			pDevice->GetImmediateContext(&pContext);
			DXGI_SWAP_CHAIN_DESC sd;
			pSwapChain->GetDesc(&sd);
			window = sd.OutputWindow;
			ID3D11Texture2D* pBackBuffer;
			pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (LPVOID*)&pBackBuffer);
			pDevice->CreateRenderTargetView(pBackBuffer, NULL, &mainRenderTargetView);
			pBackBuffer->Release();
			oWndProc = (WNDPROC)SetWindowLongPtr(window, GWLP_WNDPROC, (LONG_PTR)WndProc);
			InitImGui();
			init = true;
		}

		else
			return oPresent(pSwapChain, SyncInterval, Flags);
	}

	ImGui_ImplDX11_NewFrame();
	ImGui_ImplWin32_NewFrame();
	ImGui::NewFrame();
	ImGuiIO& io = ImGui::GetIO();
	io.MouseDrawCursor = guiVisible;

	if (guiVisible)
	{
		if (logoFrameCount > 0 && logoDelays)
		{
			logoAnimTime += io.DeltaTime;
			float frameDuration = (float)logoDelays[currentLogoFrame] / 1000.0f;
			if (frameDuration < 0.001f) frameDuration = 0.016f;

			if (logoAnimTime >= frameDuration)
			{
				logoAnimTime -= frameDuration;
				currentLogoFrame = (currentLogoFrame + 1) % logoFrameCount;
			}
		}

		ImGui::SetNextWindowSize(ImVec2(775, 590), ImGuiCond_FirstUseEver);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
		ImGui::Begin("Window", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 windowPos = ImGui::GetWindowPos();
		ImVec2 windowSize = ImGui::GetWindowSize();

		float rounding = 15.0f;
		drawList->AddRectFilled(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y), IM_COL32(19, 18, 21, 208), rounding);

		float sidebarWidth = 90.0f;
		float sidebarHeight = windowSize.y;
		float gradientSteps = 20.0f;
		float stepHeight = sidebarHeight / gradientSteps;

		for (int i = 0; i < gradientSteps; i++)
		{
			float y1 = windowPos.y + i * stepHeight;
			float y2 = windowPos.y + (i + 1) * stepHeight;

			float progress = (float)i / (gradientSteps - 1);
			float alpha = 0.3f + 0.7f * (1.0f - fabs(progress - 0.5f) * 2.0f);

			ImU32 color = IM_COL32(17, 17, 20, (int)(alpha * 255));
			drawList->AddRectFilled(ImVec2(windowPos.x, y1), ImVec2(windowPos.x + sidebarWidth, y2), color);
		}

		float separatorX = windowPos.x + sidebarWidth;
		drawList->AddLine(ImVec2(separatorX, windowPos.y), ImVec2(separatorX, windowPos.y + windowSize.y), IM_COL32(32, 32, 32, 255), 1.0f);

		float horizontalLineY = windowPos.y + 70.0f;
		drawList->AddLine(ImVec2(windowPos.x + 15, horizontalLineY), ImVec2(windowPos.x + sidebarWidth - 15, horizontalLineY), IM_COL32(48, 48, 48, 255), 1.0f);

		if (logoFrameCount > 0 && currentLogoFrame < logoFrames.size())
		{
			float logoSize = 50.0f;
			float logoX = windowPos.x + (sidebarWidth - logoSize) / 2.0f;
			float logoY = horizontalLineY - 10.0f - logoSize;
			float rounding = 5.0f;

			drawList->PushClipRect(ImVec2(logoX, logoY), ImVec2(logoX + logoSize, logoY + logoSize), true);
			drawList->AddImageRounded((void*)logoFrames[currentLogoFrame],
				ImVec2(logoX, logoY),
				ImVec2(logoX + logoSize, logoY + logoSize),
				ImVec2(0, 0), ImVec2(1, 1),
				IM_COL32_WHITE,
				rounding);
			drawList->PopClipRect();
		}

		float contentBoxX = windowPos.x + sidebarWidth + 20.0f;
		float contentBoxY = windowPos.y + 95.0f;
		float contentBoxWidth = 645.0f;
		float contentBoxHeight = 445.0f;

		float boxRounding = 5.0f;

		drawList->AddRectFilled(
			ImVec2(contentBoxX, contentBoxY),
			ImVec2(contentBoxX + contentBoxWidth, contentBoxY + contentBoxHeight),
			IM_COL32(22, 22, 24, 255),
			boxRounding
		);

		drawList->AddRect(
			ImVec2(contentBoxX, contentBoxY),
			ImVec2(contentBoxX + contentBoxWidth, contentBoxY + contentBoxHeight),
			IM_COL32(49, 49, 51, 255),
			boxRounding,
			15,
			1.0f
		);

		ImGui::SetCursorScreenPos(ImVec2(contentBoxX + 5, contentBoxY + 5));
		editor.Render("##luaeditor", ImVec2(contentBoxWidth - 10, contentBoxHeight - 10), false);

		float buttonY = contentBoxY + contentBoxHeight + 10.0f;
		float buttonX = contentBoxX;
		float buttonWidth = 120.0f;
		float buttonHeight = 30.0f;
		float buttonSpacing = 5.0f;

		ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(35, 35, 37, 255));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);

		ImGui::SetCursorScreenPos(ImVec2(buttonX, buttonY));
		if (AnimatedButton("Execute", ImVec2(buttonWidth, buttonHeight), 0))
		{
			std::string code = editor.GetText();
			if (!ExecuteToResourceMonitor(code))
			{
				MessageBoxA(nullptr, "Resource monitor nie znaleziony!", "Error", MB_OK | MB_ICONERROR);
			}
		}

		ImGui::SetCursorScreenPos(ImVec2(buttonX + buttonWidth + buttonSpacing, buttonY));
		if (AnimatedButton("Clear", ImVec2(buttonWidth, buttonHeight), 1))
		{
			editor.SetText("");
		}

		ImGui::SetCursorScreenPos(ImVec2(buttonX + (buttonWidth + buttonSpacing) * 2, buttonY));
		if (AnimatedButton("Execute File", ImVec2(buttonWidth, buttonHeight), 2))
		{
			std::string code = editor.GetText();
			ExecuteLuaCode(code);
		}

		float fixBlinkingX = contentBoxX + contentBoxWidth - buttonWidth;
		ImGui::SetCursorScreenPos(ImVec2(fixBlinkingX, buttonY));
		if (AnimatedButton("Fix Blinking", ImVec2(buttonWidth, buttonHeight), 3))

		{
		}

		ImGui::PopStyleVar(2);
		ImGui::PopStyleColor(1);

		ImGui::End();

		ImDrawList* bgDrawList = ImGui::GetForegroundDrawList();
		ImVec2 bottomRectPos = ImVec2(windowPos.x, windowPos.y + windowSize.y + 5);
		ImVec2 bottomRectSize = ImVec2(775, 60);
		bgDrawList->AddRectFilled(bottomRectPos, ImVec2(bottomRectPos.x + bottomRectSize.x, bottomRectPos.y + bottomRectSize.y), IM_COL32(19, 18, 21, 255), 6.0f);

		// Debug status overlay
		char statusText[512];
		const char* hookStatus = g_debug.hookSucceeded ? "CONNECTED" : (g_debug.hookAttempted ? "FAILED" : "PENDING");
		ImU32 statusColor = g_debug.hookSucceeded ? IM_COL32(0, 255, 0, 255) : IM_COL32(255, 0, 0, 255);

		snprintf(statusText, sizeof(statusText),
			"CFX Hook: %s | RM: %s | EventComp: %s | Queued: %d | Executed: %d | Ticks: %d",
			hookStatus,
			g_debug.resourceManagerValid.load() ? "OK" : "NULL",
			g_debug.eventCompValid.load() ? "OK" : "NULL",
			g_debug.queuedCount.load(),
			g_debug.executedCount.load(),
			g_debug.tickCallCount.load()
		);

		ImGui::SetCursorScreenPos(ImVec2(bottomRectPos.x + 10, bottomRectPos.y + 5));
		ImGui::PushStyleColor(ImGuiCol_Text, statusColor);
		ImGui::Text("%s", statusText);
		ImGui::PopStyleColor();

		if (g_debug.lastError[0] != '\0')
		{
			ImGui::SetCursorScreenPos(ImVec2(bottomRectPos.x + 10, bottomRectPos.y + 25));
			ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
			ImGui::Text("Last: %s", g_debug.lastError);
			ImGui::PopStyleColor();
		}

		ImGui::PopStyleVar();
	}

	ImGui::Render();

	pContext->OMSetRenderTargets(1, &mainRenderTargetView, NULL);
	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
	return oPresent(pSwapChain, SyncInterval, Flags);
}

#include <chrono>
#include <ctime>
#include <windows.h>

DWORD WINAPI MainThread(LPVOID)
{
	bool initHook = false;

	while (true)
	{
		if (!initHook &&
			kiero::init(kiero::RenderType::D3D11) == kiero::Status::Success)
		{
			kiero::bind(8, (void**)&oPresent, hkPresent);
			initHook = true;
		}

            (void)HookResourceManagerTick();
		Sleep(100);
	}

	return TRUE;
}

BOOL WINAPI DllMain(HMODULE hMod, DWORD dwReason, LPVOID)
{
	switch (dwReason)
	{
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(hMod);
		CreateThread(nullptr, 0, MainThread, hMod, 0, nullptr);
		break;
	case DLL_PROCESS_DETACH:
		kiero::shutdown();
		break;
	}
	return TRUE;
}