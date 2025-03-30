#include "dependencies/imgui.h"
#include "dependencies/imgui_stdlib.h"
#include "dependencies/imgui_impl_win32.h"
#include "dependencies/imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <algorithm>
#include <portaudio.h>
#include <sndfile.h>
#include <iostream>
#include <filesystem>
#include <windows.h>
#include <stdio.h>
#include <psapi.h>
#include <atomic>
#include <vector>

#define SAMPLE_RATE 44100
#define NUM_CHANNELS 1
std::atomic<bool> stopRequested(false);
static int monoAudioCallback(const void* inputBuffer, void* outputBuffer,
    unsigned long framesPerBuffer,
    const PaStreamCallbackTimeInfo* timeInfo,
    PaStreamCallbackFlags statusFlags,
    void* userData)
{
    if (stopRequested.load())
        return paAbort;
    SNDFILE* audioFile = static_cast<SNDFILE*>(userData);
    float* out = static_cast<float*>(outputBuffer);

    unsigned long framesLeft = framesPerBuffer;

    while (framesLeft > 0)
    {
        sf_count_t framesRead = sf_readf_float(audioFile, out, framesLeft);

        if (framesRead < framesLeft)
        {
            // Reached the end of the file, rewind it.
            sf_seek(audioFile, 0, SEEK_SET);
            framesLeft -= framesRead;
            out += framesRead;
        }
        else
            framesLeft = 0;
    }
    return paContinue;
}
// Data
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

struct ProcessItem {
    const char* name;
    DWORD id;
};
std::vector<ProcessItem> processes;

__forceinline char* PreserveString(const char* src_str) noexcept {
    char* new_str = new char[std::strlen(src_str) + 1];
    std::strcpy(new_str, src_str);
    return new_str;
}

void AppendProcessToVector(DWORD processID)
{
    TCHAR szProcessName[MAX_PATH] = TEXT("<unknown>");

    // Get a handle to the process.

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION |
        PROCESS_VM_READ,
        FALSE, processID);

    if (NULL == hProcess)
        return;

    // Get the process name.

    HMODULE hMod;
    DWORD cbNeeded;

    if (EnumProcessModules(hProcess, &hMod, sizeof(hMod),
        &cbNeeded))
    {
        GetModuleBaseName(hProcess, hMod, szProcessName,
            sizeof(szProcessName) / sizeof(TCHAR));
    }

    // Print the process name and identifier.

    char normal[MAX_PATH];
    wcstombs_s(NULL, normal, szProcessName, MAX_PATH);
    ProcessItem item{ PreserveString(normal), processID};
    processes.push_back(item);

    // Release the handle to the process.

    CloseHandle(hProcess);
}

bool RefreshProcesses() {
    DWORD aProcesses[1024], cbNeeded, cProcesses;
    unsigned int i;

    if (!EnumProcesses(aProcesses, sizeof(aProcesses), &cbNeeded))
    {
        return false;
    }


    // Calculate how many process identifiers were returned.

    cProcesses = cbNeeded / sizeof(DWORD);

    // Print the name and process identifier for each process.
    processes.clear();
    for (i = 0; i < cProcesses; i++)
    {
        if (aProcesses[i] != 0)
        {
            AppendProcessToVector(aProcesses[i]);
        }
    }
    return true;
}

// Main code
int main(int, char**)
{
    // Create application window
    //ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"Toolbox Loader", nullptr };
    ::RegisterClassExW(&wc);
    ImVec2 window_size = ImVec2(600, 300);
    int x = GetSystemMetrics(SM_CXSCREEN) / 2 - window_size.x / 2;
    int y = GetSystemMetrics(SM_CYSCREEN) / 2 - window_size.y / 2;
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"YYC Toolbox Loader", WS_OVERLAPPEDWINDOW, x, y, window_size.x, window_size.y, nullptr, nullptr, wc.hInstance, nullptr);
    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

#if NDEBUG
    ::ShowWindow(::GetConsoleWindow(), SW_HIDE);
#endif

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
    io.IniFilename = NULL;
    io.LogFilename = NULL;

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return a nullptr. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
    //IM_ASSERT(font != nullptr);
    
    PaError err;
    SNDFILE* audioFile;
    PaStream* stream = nullptr;
    SF_INFO sfInfo;

    // Open the audio file.
    char buffer[MAX_PATH] = { 0 };
    GetModuleFileNameA(NULL, buffer, MAX_PATH);
    std::string::size_type pos = std::string(buffer).find_last_of("\\");
    std::string cwd = std::string(buffer).substr(0, pos) + "\\music.ogg";
    audioFile = sf_open(cwd.c_str(), SFM_READ, &sfInfo);
    if (!audioFile)
    {
        std::cout << cwd.c_str() << std::endl;
        std::cerr << "Error opening audio file: " << sf_strerror(nullptr) << std::endl;
        goto RENDERLOOP;
    }

    if (sfInfo.channels != 1)
    {
        std::cerr << "A mono audio file is required!" << std::endl;
        sf_close(audioFile);
        goto RENDERLOOP;
    }

    // Initialize PortAudio.
    err = Pa_Initialize();
    if (err != paNoError)
    {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        return -1;
    }

    // Open the PortAudio stream.
    err = Pa_OpenDefaultStream(&stream,
        0,
        NUM_CHANNELS,
        paFloat32,
        sfInfo.samplerate,
        256,
        monoAudioCallback,
        audioFile);
    if (err != paNoError)
    {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        sf_close(audioFile);
        Pa_Terminate();
        return -1;
    }

    // Start the stream.
    err = Pa_StartStream(stream);
    if (err != paNoError)
    {
        std::cerr << "PortAudio error: " << Pa_GetErrorText(err) << std::endl;
        sf_close(audioFile);
        Pa_Terminate();
        return -1;
    }
    RENDERLOOP:
    // Our state
    ImVec4 clear_color = ImVec4(0.f, 0.f, 0.f, 1.00f);

    // Main loop
    bool done = false;
    RefreshProcesses();
    while (!done)
    {
        // Poll and handle messages (inputs, window resize, etc.)
        // See the WndProc() function below for our to dispatch events to the Win32 backend.
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle window being minimized or screen locked
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // Handle window resize (we don't resize directly in the WM_SIZE handler)
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }
        
        // Start the Dear ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        {
            static float f = 0.0f;
            static int counter = 0;
            ImGuiStyle& style = ImGui::GetStyle();
            ImGui::Begin("YYC Toolbox Loader", NULL, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus);
            ImGui::SetWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y));
            ImGui::SetWindowPos(ImVec2(0, 0));
            style.Alpha = f;
            if (f < 1)
                f += 1.5 * io.DeltaTime;
            if (f > 1)
                f = 1;
            ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / io.Framerate, io.Framerate);
            ImGui::Text("Select process to inject into:");
            static ProcessItem selected;
            static std::string search;
            if (ImGui::BeginListBox("##processes")) {
                for (ProcessItem proc : processes) {
                    std::string name = proc.name + std::string(" PID: ") + std::to_string(proc.id);
                    if (name.find(search) == std::string::npos && !search.empty())
                        continue;
                    if (ImGui::Selectable(name.c_str(), selected.id == proc.id))
                        selected.id = proc.id;
                }
                ImGui::EndListBox();
            }
            ImGui::SameLine();
            if (ImGui::Button("Refresh"))
                RefreshProcesses();
            ImGui::InputText("Search", &search);
            if (ImGui::Button("Prank em john"))
            {
                HANDLE hTargetProcess = OpenProcess(PROCESS_ALL_ACCESS, false, selected.id);
                std::string cwd = std::string(buffer).substr(0, pos) + "\\InternalDLL.dll";
                const char* lpFullDLLPath = PreserveString(cwd.c_str());
                if (hTargetProcess) {
                    const LPVOID lpPathAddress = VirtualAllocEx(hTargetProcess, nullptr, lstrlenA(lpFullDLLPath) + 1, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                    if (lpPathAddress == nullptr)
                    {
                        printf("Unable to allocate memory.\n");
                        goto SHOW_ERROR;
                    }

                    printf("Memory allocated at 0x%X\n", (UINT)(uintptr_t)lpPathAddress);

                    const DWORD dwWriteResult = WriteProcessMemory(hTargetProcess, lpPathAddress, lpFullDLLPath, lstrlenA(lpFullDLLPath) + 1, nullptr);
                    if (dwWriteResult == 0)
                    {
                        printf("Unable to write to allocated memory.\n");
                        goto SHOW_ERROR;
                    }

                    const HMODULE hModule = GetModuleHandleA("kernel32.dll");
                    if (hModule == INVALID_HANDLE_VALUE || hModule == nullptr)
                        goto SHOW_ERROR;

                    const FARPROC lpFunctionAddress = GetProcAddress(hModule, "LoadLibraryA");
                    if (lpFunctionAddress == nullptr)
                    {
                        printf("Invalid \"LoadLibraryA\" address.\n");
                        goto SHOW_ERROR;
                    }

                    printf("LoadLibraryA address at 0x%X\n", (UINT)(uintptr_t)lpFunctionAddress);

                    const HANDLE hThreadCreationResult = CreateRemoteThread(hTargetProcess, nullptr, 0, (LPTHREAD_START_ROUTINE)lpFunctionAddress, lpPathAddress, 0, nullptr);
                    if (hThreadCreationResult == INVALID_HANDLE_VALUE)
                    {
                        printf("Invalid thread handle.\n");
                        goto SHOW_ERROR;
                    }
                    stopRequested.store(true);
                    Pa_AbortStream(stream);
                    Pa_CloseStream(stream);
                    sf_close(audioFile);
                    Pa_Terminate();
                    PlaySound(TEXT("C:\\Windows\\Media\\tada.wav"), NULL, SND_FILENAME);
                    done = true;
                }
                else {
                    SHOW_ERROR:
                    ImGui::OpenPopup("Oops...");
                }
            }

            if (ImGui::BeginPopupModal("Oops...", nullptr, ImGuiWindowFlags_NoResize)) {
                ImGui::Text("Unable to attach to process. Access may be denied?");
                if (ImGui::Button("OK"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::End();
        }

        // Rendering
        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Present
        HRESULT hr = g_pSwapChain->Present(1, 0);   // Present with vsync
        //HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// Helper functions

bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}