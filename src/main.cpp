#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <shellapi.h>
#include <initguid.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <tchar.h>
#include <psapi.h>
#include <setupapi.h>
#include <devguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <cfgmgr32.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propkey.h>
#include <set>
#include <thread>
#include <atomic>
#include <chrono>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// DirectX & Window Handles
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// WASAPI Audio Loopback Capture
static IMMDeviceEnumerator* g_pEnumerator = nullptr;
static IMMDevice* g_pAudioDevice = nullptr;
static IAudioClient* g_pAudioClient = nullptr;
static IAudioCaptureClient* g_pCaptureClient = nullptr;
static bool g_wasapiInitialized = false;

#define FFT_SIZE 512
static std::vector<float> g_audioRawBuffer(FFT_SIZE, 0.0f);
static std::vector<float> g_audioFFTBins(32, 0.0f);

// Data Models
struct DisplayLayoutInfo {
    std::string deviceName;
    RECT rect;
    bool isPrimary;
    int rotationDegrees;
    ImVec2 uiOffset; 
};

struct DeviceItem {
    std::string name;
    std::string category;
    std::string status;
};

struct PinnedApp {
    std::string name;
    std::string command;
};

struct DriveInfo {
    std::string letter;
    std::string label;
    UINT type;
    float freeGB;
    float totalGB;
};

// Global Telemetry State
static float g_cpuUsage = 0.0f;
static float g_ramUsagePct = 0.0f;
static DWORD g_totalRAM_MB = 0;
static DWORD g_usedRAM_MB = 0;
static DWORD g_processCount = 0;
static std::vector<DriveInfo> g_driveList;

// GPU Telemetry State
static std::string g_gpuName = "Detecting GPU...";
static float g_gpuVRAM_UsedMB = 0.0f;
static float g_gpuVRAM_TotalMB = 0.0f;
static float g_gpuUsagePct = 0.0f;

// PDH Query Handles for Calibrated Hardware Monitoring
static PDH_HQUERY g_pdhQuery = NULL;
static PDH_HCOUNTER g_cpuCounter = NULL;
static PDH_HCOUNTER g_gpuCounter = NULL;
static bool g_pdhInitialized = false;

// Media Info
static std::string g_currentTrackTitle = "No Active Song Playing";
static std::string g_currentTrackArtist = "System Audio";

static std::vector<float> g_cpuHistory(60, 0.0f);
static std::vector<float> g_gpuHistory(60, 0.0f);
static std::vector<DisplayLayoutInfo> g_monitors;
static std::vector<DeviceItem> g_deviceList;
static std::vector<PinnedApp> g_pinnedApps = {
    {"CMD", "cmd.exe"},
    {"PowerShell", "powershell.exe"},
    {"Task Manager", "taskmgr.exe"},
    {"Explorer", "explorer.exe"}
};

// Forward Declarations
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Initialize PDH counters using English counter path names to avoid localization issues
void InitPDHTelemetry() {
    if (g_pdhInitialized) return;

    if (PdhOpenQueryA(NULL, 0, &g_pdhQuery) == ERROR_SUCCESS) {
        // Use Processor Information(_Total)\% Processor Utility (Matches Win 10/11 Task Manager & Turbo Boost)
        HRESULT hr = PdhAddEnglishCounterA(g_pdhQuery, "\\Processor Information(_Total)\\% Processor Utility", 0, &g_cpuCounter);
        
        // Fallback for older legacy Windows builds if % Processor Utility isn't present
        if (hr != ERROR_SUCCESS) {
            PdhAddEnglishCounterA(g_pdhQuery, "\\Processor(_Total)\\% Processor Time", 0, &g_cpuCounter);
        }

        // Real-time GPU engine utilization counter across 3D/Compute
        PdhAddEnglishCounterA(g_pdhQuery, "\\GPU Engine(*)\\Utilization Percentage", 0, &g_gpuCounter);
        
        PdhCollectQueryData(g_pdhQuery);
        g_pdhInitialized = true;
    }
}





// =============================================================
// SILVERBENCH LIVE RAY TRACING ENGINE
// =============================================================

struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
    Vec3 operator+(const Vec3& b) const { return Vec3(x + b.x, y + b.y, z + b.z); }
    Vec3 operator-(const Vec3& b) const { return Vec3(x - b.x, y - b.y, z - b.z); }
    Vec3 operator*(float b) const { return Vec3(x * b, y * b, z * b); }
    Vec3 operator*(const Vec3& b) const { return Vec3(x * b.x, y * b.y, z * b.z); }
    float dot(const Vec3& b) const { return x * b.x + y * b.y + z * b.z; }
    Vec3 normalize() const { float len = std::sqrt(dot(*this)); return len > 0 ? Vec3(x / len, y / len, z / len) : Vec3(0, 0, 0); }
};

struct Sphere {
    Vec3 center;
    float radius;
    Vec3 color;
    float reflectivity;
    float glass; // Transparency / Refraction index
    
    bool intersect(const Vec3& orig, const Vec3& dir, float& t) const {
        Vec3 L = center - orig;
        float tca = L.dot(dir);
        if (tca < 0) return false;
        float d2 = L.dot(L) - tca * tca;
        float r2 = radius * radius;
        if (d2 > r2) return false;
        float thc = std::sqrt(r2 - d2);
        t = tca - thc;
        if (t < 0) t = tca + thc;
        return t > 0.001f;
    }
};


static ID3D11Texture2D* g_pBenchTexture = nullptr;
static ID3D11ShaderResourceView* g_pBenchSRV = nullptr;
static std::vector<uint32_t> g_pixelBuffer;

// Initialize GPU Texture Buffer
bool InitBenchmarkTexture(ID3D11Device* device, int width, int height) {
    if (!device) return false;

    if (g_pBenchSRV) { g_pBenchSRV->Release(); g_pBenchSRV = nullptr; }
    if (g_pBenchTexture) { g_pBenchTexture->Release(); g_pBenchTexture = nullptr; }

    g_pixelBuffer.assign(width * height, 0xFF181012); // Initial dark blue/purple backdrop

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = g_pixelBuffer.data();
    initData.SysMemPitch = width * sizeof(uint32_t);

    HRESULT hr = device->CreateTexture2D(&desc, &initData, &g_pBenchTexture);
    if (SUCCEEDED(hr)) {
        hr = device->CreateShaderResourceView(g_pBenchTexture, nullptr, &g_pBenchSRV);
        return SUCCEEDED(hr);
    }
    return false;
}

// Upload pixel array to GPU texture frame-by-frame
void UpdateBenchmarkTextureGPU(ID3D11DeviceContext* context, int width) {
    if (!g_pBenchTexture || !context) return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(context->Map(g_pBenchTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        memcpy(mapped.pData, g_pixelBuffer.data(), g_pixelBuffer.size() * sizeof(uint32_t));
        context->Unmap(g_pBenchTexture, 0);
    }
}

// Shader Math
struct PixelCoord {
    uint16_t x;
    uint16_t y;
};

struct CpuBenchmarkState {
    std::atomic<bool> isRunning{false};
    std::atomic<bool> stopRequested{false};
    std::atomic<uint64_t> totalRaysCast{0};
    std::atomic<uint32_t> currentWorkIndex{0};
    std::atomic<uint32_t> threadsFinished{0};
    std::atomic<uint32_t> completedPasses{0};

    bool isStressMode = false;
    float progress = 0.0f;
    float finalScore = 0.0f;
    float raysPerSecond = 0.0f;
    float elapsedTime = 0.0f;
    int numThreads = 0;
    int targetWidth = 1920;
    int targetHeight = 1080;
    int maxPasses = 0; // Will be set based on stress mode
    std::chrono::high_resolution_clock::time_point startTime;
};

static CpuBenchmarkState g_benchState;
static std::vector<PixelCoord> g_spiralPixelOrder;
static std::vector<Vec3> g_accumulationBuffer;

// Build radial spiral coordinates from middle-out
void BuildCenterOutSpiralOrder(int width, int height) {
    g_spiralPixelOrder.clear();
    g_spiralPixelOrder.reserve(width * height);

    float centerX = width * 0.5f;
    float centerY = height * 0.5f;

    struct PixelDistance {
        uint16_t x, y;
        float distSq;
    };

    std::vector<PixelDistance> tempCoords;
    tempCoords.reserve(width * height);

    for (int y = 0; y < height; ++y) {
        float dy = (float)y - centerY;
        for (int x = 0; x < width; ++x) {
            float dx = (float)x - centerX;
            tempCoords.push_back({ (uint16_t)x, (uint16_t)y, dx * dx + dy * dy });
        }
    }

    // Sort pixels by Euclidean distance from frame center
    std::sort(tempCoords.begin(), tempCoords.end(), [](const PixelDistance& a, const PixelDistance& b) {
        return a.distSq < b.distSq;
    });

    for (const auto& p : tempCoords) {
        g_spiralPixelOrder.push_back({ p.x, p.y });
    }
}

// Heavy Shader with 6-Bounce Deep Ray Bounces & Jittered Soft Shadows
Vec3 TraceRayHeavy(const Vec3& orig, const Vec3& dir, const std::vector<Sphere>& scene, const Vec3& lightPos, int depth) {
    if (depth > 6) return Vec3(0.02f, 0.03f, 0.06f); // Deep sky background

    float nearestT = 1e9f;
    const Sphere* hitObj = nullptr;
    bool hitPlane = false;

    // Check Spheres
    for (const auto& sphere : scene) {
        float t;
        if (sphere.intersect(orig, dir, t) && t < nearestT) {
            nearestT = t;
            hitObj = &sphere;
            hitPlane = false;
        }
    }

    // Check Reflective Ground Plane (y = -2.0)
    if (std::abs(dir.y) > 0.0001f) {
        float tPlane = (-2.0f - orig.y) / dir.y;
        if (tPlane > 0.001f && tPlane < nearestT) {
            nearestT = tPlane;
            hitObj = nullptr;
            hitPlane = true;
        }
    }

    if (!hitObj && !hitPlane) {
        float t = 0.5f * (dir.y + 1.0f);
        return Vec3(0.01f, 0.02f, 0.05f) * (1.0f - t) + Vec3(0.10f, 0.20f, 0.40f) * t;
    }

    Vec3 hitPoint = orig + dir * nearestT;
    Vec3 normal;
    Vec3 surfaceColor;
    float reflectivity = 0.0f;

    if (hitPlane) {
        normal = Vec3(0, 1, 0);
        int checkX = (int)std::floor(hitPoint.x * 0.5f);
        int checkZ = (int)std::floor(hitPoint.z * 0.5f);
        bool isEven = ((checkX + checkZ) % 2 == 0);
        surfaceColor = isEven ? Vec3(0.85f, 0.85f, 0.90f) : Vec3(0.12f, 0.15f, 0.20f);
        reflectivity = 0.5f;
    } else {
        normal = (hitPoint - hitObj->center).normalize();
        surfaceColor = hitObj->color;
        reflectivity = hitObj->reflectivity;
    }

    // Soft Area Light Ray Calculations
    Vec3 lightColor(0, 0, 0);
    int shadowSamples = 4;
    for (int i = 0; i < shadowSamples; ++i) {
        Vec3 jitteredLight = lightPos + Vec3(((float)rand() / RAND_MAX - 0.5f) * 2.5f,
                                             ((float)rand() / RAND_MAX - 0.5f) * 2.5f,
                                             ((float)rand() / RAND_MAX - 0.5f) * 2.5f);
        Vec3 lightDir = (jitteredLight - hitPoint).normalize();
        bool inShadow = false;

        for (const auto& sphere : scene) {
            float t;
            if (sphere.intersect(hitPoint + normal * 0.002f, lightDir, t)) {
                inShadow = true;
                break;
            }
        }

        float diff = inShadow ? 0.08f : (std::max)(0.08f, normal.dot(lightDir));
        lightColor = lightColor + surfaceColor * diff;
    }
    lightColor = lightColor * (1.0f / shadowSamples);

    // Deep Bounce Reflection Path
    if (reflectivity > 0.0f) {
        Vec3 reflDir = dir - normal * 2.0f * dir.dot(normal);
        Vec3 reflColor = TraceRayHeavy(hitPoint + normal * 0.002f, reflDir.normalize(), scene, lightPos, depth + 1);
        return lightColor * (1.0f - reflectivity) + reflColor * reflectivity;
    }

    return lightColor;
}

// Multi-Threaded Spiral Ray Tracing Worker
void RayTracingWorkerThread(bool stressMode) {
    std::vector<Sphere> scene = {
        { Vec3(0.0f, 0.0f, -12.0f),  3.0f, Vec3(0.95f, 0.15f, 0.15f), 0.85f },
        { Vec3(-5.0f, -0.5f, -10.0f), 2.0f, Vec3(0.15f, 0.85f, 0.25f), 0.50f },
        { Vec3(5.0f, -0.5f, -10.0f),  2.0f, Vec3(0.20f, 0.40f, 0.95f), 0.50f },
        { Vec3(0.0f, 3.8f, -9.0f),   1.5f, Vec3(0.95f, 0.80f, 0.10f), 0.40f },
        { Vec3(-2.8f, -1.2f, -7.0f),  0.8f, Vec3(0.95f, 0.95f, 0.95f), 0.95f },
        { Vec3(2.8f, -1.2f, -7.0f),   0.8f, Vec3(0.85f, 0.15f, 0.85f), 0.50f }
    };

    Vec3 lightPos(12.0f, 20.0f, -2.0f);
    uint32_t totalPixels = (uint32_t)g_spiralPixelOrder.size();
    const uint32_t BLOCK_SIZE = 64;
    int samplesPerPixel = 4;

    thread_local uint32_t rngState = 123456789 + (uint32_t)std::hash<std::thread::id>{}(std::this_thread::get_id());
    auto FastRandom = [&]() {
        rngState ^= rngState << 13;
        rngState ^= rngState >> 17;
        rngState ^= rngState << 5;
        return (float)(rngState & 0xFFFFFF) / 16777216.0f;
    };

    while (!g_benchState.stopRequested.load()) {
        uint32_t blockStart = g_benchState.currentWorkIndex.fetch_add(BLOCK_SIZE);

        if (blockStart >= totalPixels) {
            // Stop ONLY when target passes are reached (in standard mode)
            if (!stressMode && g_benchState.completedPasses.load() >= (uint32_t)g_benchState.maxPasses) {
                break;
            }

            // Safely reset work index for the next pass
            uint32_t oldIndex = g_benchState.currentWorkIndex.exchange(0);
            if (oldIndex >= totalPixels) {
                g_benchState.completedPasses.fetch_add(1);
                std::fill(g_accumulationBuffer.begin(), g_accumulationBuffer.end(), Vec3(0, 0, 0));
            }

            std::this_thread::yield();
            continue;
        }

        uint32_t passNum = g_benchState.completedPasses.load();

        // Moving Camera Orbit per pass
        float camAngle = passNum * 0.08f;
        Vec3 cameraPos(
            std::sin(camAngle) * 3.5f,
            1.5f + std::cos(camAngle * 0.5f) * 0.7f,
            2.0f + std::cos(camAngle) * 1.5f
        );

        uint32_t blockEnd = (std::min)(blockStart + BLOCK_SIZE, totalPixels);

        for (uint32_t i = blockStart; i < blockEnd; ++i) {
            if (g_benchState.stopRequested.load()) break;

            PixelCoord pt = g_spiralPixelOrder[i];
            Vec3 accumulatedColor(0, 0, 0);

            for (int s = 0; s < samplesPerPixel; ++s) {
                float subX = (float)pt.x + (FastRandom() - 0.5f);
                float subY = (float)pt.y + (FastRandom() - 0.5f);

                float u = (subX - (g_benchState.targetWidth * 0.5f)) / g_benchState.targetWidth;
                float v = -(subY - (g_benchState.targetHeight * 0.5f)) / g_benchState.targetHeight;

                Vec3 rayDir = Vec3(u, v, -1.0f).normalize();
                accumulatedColor = accumulatedColor + TraceRayHeavy(cameraPos, rayDir, scene, lightPos, 0);
            }

            accumulatedColor = accumulatedColor * (1.0f / samplesPerPixel);
            int idx = pt.y * g_benchState.targetWidth + pt.x;

            g_accumulationBuffer[idx] = g_accumulationBuffer[idx] + accumulatedColor;

            Vec3 finalColor = g_accumulationBuffer[idx];

            uint8_t r = (uint8_t)(std::clamp(finalColor.x, 0.0f, 1.0f) * 255.0f);
            uint8_t g = (uint8_t)(std::clamp(finalColor.y, 0.0f, 1.0f) * 255.0f);
            uint8_t b = (uint8_t)(std::clamp(finalColor.z, 0.0f, 1.0f) * 255.0f);

            g_pixelBuffer[idx] = (0xFF << 24) | (b << 16) | (g << 8) | r;
            g_benchState.totalRaysCast.fetch_add(samplesPerPixel, std::memory_order_relaxed);
        }
    }

    g_benchState.threadsFinished.fetch_add(1);
}

void StartCpuBenchmark(bool stressMode) {
    if (g_benchState.isRunning) return;

    g_benchState.targetWidth = 1280;
    g_benchState.targetHeight = 720;

    InitBenchmarkTexture(g_pd3dDevice, g_benchState.targetWidth, g_benchState.targetHeight);
    BuildCenterOutSpiralOrder(g_benchState.targetWidth, g_benchState.targetHeight);

    g_accumulationBuffer.assign(g_benchState.targetWidth * g_benchState.targetHeight, Vec3(0, 0, 0));

    g_benchState.isRunning = true;
    g_benchState.stopRequested = false;
    g_benchState.isStressMode = stressMode;
    g_benchState.totalRaysCast = 0;
    g_benchState.currentWorkIndex = 0;
    g_benchState.threadsFinished = 0;
    g_benchState.completedPasses = 0;

    // Fixed pass target: 100 passes guarantees accurate work across all CPUs
    g_benchState.maxPasses = stressMode ? 99999 : 500;
    g_benchState.progress = 0.0f;
    g_benchState.finalScore = 0.0f;

    g_benchState.numThreads = (int)std::thread::hardware_concurrency();
    if (g_benchState.numThreads <= 0) g_benchState.numThreads = 4;

    g_benchState.startTime = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < g_benchState.numThreads; ++i) {
        std::thread(RayTracingWorkerThread, stressMode).detach();
    }
}

void UpdateCpuBenchmarkUIState() {
    if (!g_benchState.isRunning) return;

    auto now = std::chrono::high_resolution_clock::now();
    g_benchState.elapsedTime = std::chrono::duration<float>(now - g_benchState.startTime).count();

    if (g_benchState.elapsedTime > 0.0f) {
        g_benchState.raysPerSecond = (float)g_benchState.totalRaysCast.load() / g_benchState.elapsedTime;
    }

    // Progress percentage based ONLY on passes completed
    g_benchState.progress = std::clamp((float)g_benchState.completedPasses.load() / (float)g_benchState.maxPasses, 0.0f, 1.0f);

    // Stop ONLY when target passes are done or user manually cancels
    if ((!g_benchState.isStressMode && g_benchState.completedPasses.load() >= (uint32_t)g_benchState.maxPasses) ||
        g_benchState.threadsFinished >= (uint32_t)g_benchState.numThreads ||
        g_benchState.stopRequested.load()) 
    {
        g_benchState.isRunning = false;
        g_benchState.progress = 1.0f;

        // Final score normalized by work done vs time taken
        g_benchState.finalScore = (g_benchState.raysPerSecond / 1000000.0f) * 1000.0f;
    }
}

void StopCpuBenchmark() {
    g_benchState.stopRequested = true;
}



// Update CPU, GPU, and VRAM using PDH and DXGI 1.4 Adapter Queries
void UpdateHardwareTelemetry() {
    static ULONGLONG lastUpdate = 0;
    ULONGLONG now = GetTickCount64();

    if (!g_pdhInitialized) {
        InitPDHTelemetry();
    }

    if (now - lastUpdate >= 1000) {
        if (g_pdhInitialized && g_pdhQuery) {
            PdhCollectQueryData(g_pdhQuery);

            // Fetch CPU % Processor Time
            if (g_cpuCounter) {
                PDH_FMT_COUNTERVALUE val;
                if (PdhGetFormattedCounterValue(g_cpuCounter, PDH_FMT_DOUBLE, NULL, &val) == ERROR_SUCCESS) {
                    g_cpuUsage = std::clamp((float)val.doubleValue, 0.0f, 100.0f);
                }
            }

            // Fetch Maximum GPU Engine Usage
            if (g_gpuCounter) {
                DWORD bufferSize = 0;
                DWORD itemCapacity = 0;
                PdhGetFormattedCounterArrayA(g_gpuCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCapacity, NULL);

                if (bufferSize > 0) {
                    std::vector<BYTE> buffer(bufferSize);
                    PDH_FMT_COUNTERVALUE_ITEM_A* pItems = (PDH_FMT_COUNTERVALUE_ITEM_A*)buffer.data();

                    if (PdhGetFormattedCounterArrayA(g_gpuCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCapacity, pItems) == ERROR_SUCCESS) {
                        double maxGpuEngine = 0.0;
                        for (DWORD i = 0; i < itemCapacity; i++) {
                            if (pItems[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) {
                                maxGpuEngine = (std::max)(maxGpuEngine, pItems[i].FmtValue.doubleValue);
                            }
                        }
                        g_gpuUsagePct = std::clamp((float)maxGpuEngine, 0.0f, 100.0f);
                    }
                }
            }
        }

        // DXGI 1.4 Adapter VRAM and GPU Device Info
        IDXGIFactory4* pFactory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&pFactory)))) {
            IDXGIAdapter1* pAdapter = nullptr;
            if (SUCCEEDED(pFactory->EnumAdapters1(0, &pAdapter))) {
                DXGI_ADAPTER_DESC2 desc;
                IDXGIAdapter3* pAdapter3 = nullptr;
                if (SUCCEEDED(pAdapter->QueryInterface(IID_PPV_ARGS(&pAdapter3)))) {
                    if (SUCCEEDED(pAdapter3->GetDesc2(&desc))) {
                        char charName[128];
                        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, charName, sizeof(charName), NULL, NULL);
                        g_gpuName = charName;

                        DXGI_QUERY_VIDEO_MEMORY_INFO memInfoLocal, memInfoNonLocal;
                        float localUsed = 0.0f, nonLocalUsed = 0.0f;

                        if (SUCCEEDED(pAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memInfoLocal))) {
                            localUsed = (float)(memInfoLocal.CurrentUsage / (1024.0 * 1024.0));
                        }
                        if (SUCCEEDED(pAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &memInfoNonLocal))) {
                            nonLocalUsed = (float)(memInfoNonLocal.CurrentUsage / (1024.0 * 1024.0));
                        }

                        g_gpuVRAM_UsedMB = localUsed + nonLocalUsed;

                        float dedicatedMB = (float)(desc.DedicatedVideoMemory / (1024.0 * 1024.0));
                        float sharedMB = (float)(desc.SharedSystemMemory / (1024.0 * 1024.0));
                        g_gpuVRAM_TotalMB = (dedicatedMB > 0.0f) ? (dedicatedMB + sharedMB) : sharedMB;
                    }
                    pAdapter3->Release();
                }
                pAdapter->Release();
            }
            pFactory->Release();
        }

        lastUpdate = now;
    }
}

// Drive Enumerator
void FetchAllDrives() {
    g_driveList.clear();

    char driveBuffer[512] = {0};
    DWORD length = GetLogicalDriveStringsA(sizeof(driveBuffer) - 1, driveBuffer);
    if (length == 0 || length > sizeof(driveBuffer)) return;

    char* drivePtr = driveBuffer;
    while (*drivePtr) {
        std::string driveLetter = drivePtr;
        UINT driveType = GetDriveTypeA(driveLetter.c_str());

        if (driveType == DRIVE_FIXED || driveType == DRIVE_REMOVABLE) {
            ULARGE_INTEGER freeBytes, totalBytes, totalFree;
            if (GetDiskFreeSpaceExA(driveLetter.c_str(), &freeBytes, &totalBytes, &totalFree)) {
                char volumeName[256] = {0};
                GetVolumeInformationA(driveLetter.c_str(), volumeName, sizeof(volumeName), NULL, NULL, NULL, NULL, 0);

                DriveInfo info;
                info.letter = driveLetter;
                info.label = (strlen(volumeName) > 0) ? std::string(volumeName) : "Local Disk";
                info.type = driveType;
                info.totalGB = (float)(totalBytes.QuadPart / (1024.0 * 1024.0 * 1024.0));
                info.freeGB = (float)(totalFree.QuadPart / (1024.0 * 1024.0 * 1024.0));

                g_driveList.push_back(info);
            }
        }
        drivePtr += strlen(drivePtr) + 1;
    }
}

// Media Window Detection
BOOL CALLBACK EnumMediaWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd) && !IsIconic(hwnd)) return TRUE;

    char titleBuf[512] = {0};
    GetWindowTextA(hwnd, titleBuf, sizeof(titleBuf));
    std::string title = titleBuf;

    if (title.empty()) return TRUE;

    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);

    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    char processPath[MAX_PATH] = {0};
    DWORD pathSize = MAX_PATH;

    if (hProcess) {
        QueryFullProcessImageNameA(hProcess, 0, processPath, &pathSize);
        CloseHandle(hProcess);
    }

    std::string procStr = processPath;
    std::transform(procStr.begin(), procStr.end(), procStr.begin(), ::tolower);

    // Spotify
    if (procStr.find("spotify.exe") != std::string::npos) {
        if (title != "Spotify" && title != "Spotify Free" && title != "Spotify Premium" && title.find(" - ") != std::string::npos) {
            size_t dashPos = title.find(" - ");
            g_currentTrackArtist = title.substr(0, dashPos);
            g_currentTrackTitle = title.substr(dashPos + 3);
            return FALSE;
        }
    }

    // Media Players
    if (procStr.find("music.exe") != std::string::npos || procStr.find("foobar2000.exe") != std::string::npos || procStr.find("vlc.exe") != std::string::npos) {
        if (title.find(" - ") != std::string::npos) {
            size_t dashPos = title.find(" - ");
            g_currentTrackArtist = title.substr(0, dashPos);
            g_currentTrackTitle = title.substr(dashPos + 3);
            return FALSE;
        }
    }

    return TRUE;
}

void UpdateNowPlayingMetadata() {
    static ULONGLONG lastMediaCheck = 0;
    if (GetTickCount64() - lastMediaCheck < 1000) return;
    lastMediaCheck = GetTickCount64();

    g_currentTrackTitle = "No Active Song Playing";
    g_currentTrackArtist = "System Audio";

    EnumWindows(EnumMediaWindowsProc, 0);
}

// Audio FFT Implementation
void SimpleFFT(std::vector<float>& real, std::vector<float>& imag) {
    int n = (int)real.size();
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * (float)M_PI / len;
        float wlen_r = std::cos(ang);
        float wlen_i = std::sin(ang);
        for (int i = 0; i < n; i += len) {
            float w_r = 1.0f, w_i = 0.0f;
            for (int j = 0; j < len / 2; j++) {
                float u_r = real[i + j], u_i = imag[i + j];
                float v_r = real[i + j + len / 2] * w_r - imag[i + j + len / 2] * w_i;
                float v_i = real[i + j + len / 2] * w_i + imag[i + j + len / 2] * w_r;
                real[i + j] = u_r + v_r;
                imag[i + j] = u_i + v_i;
                real[i + j + len / 2] = u_r - v_r;
                imag[i + j + len / 2] = u_r - v_r;
                float next_w_r = w_r * wlen_r - w_i * wlen_i;
                float next_w_i = w_r * wlen_i + w_i * wlen_r;
                w_r = next_w_r; w_i = next_w_i;
            }
        }
    }
}

void InitWASAPILoopback() {
    CoInitialize(NULL);

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&g_pEnumerator);
    if (FAILED(hr)) return;

    hr = g_pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_pAudioDevice);
    if (FAILED(hr)) return;

    hr = g_pAudioDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&g_pAudioClient);
    if (FAILED(hr)) return;

    WAVEFORMATEX* pwfx = NULL;
    hr = g_pAudioClient->GetMixFormat(&pwfx);
    if (FAILED(hr)) return;

    hr = g_pAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, pwfx, NULL);
    CoTaskMemFree(pwfx);
    if (FAILED(hr)) return;

    hr = g_pAudioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&g_pCaptureClient);
    if (FAILED(hr)) return;

    g_pAudioClient->Start();
    g_wasapiInitialized = true;
}

void UpdateWASAPIFrequencies() {
    if (!g_wasapiInitialized || !g_pCaptureClient) return;

    UINT32 packetLength = 0;
    HRESULT hr = g_pCaptureClient->GetNextPacketSize(&packetLength);

    while (SUCCEEDED(hr) && packetLength > 0) {
        BYTE* pData;
        UINT32 numFrames;
        DWORD flags;

        hr = g_pCaptureClient->GetBuffer(&pData, &numFrames, &flags, NULL, NULL);
        if (SUCCEEDED(hr) && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            float* samples = (float*)pData;
            for (UINT32 i = 0; i < numFrames; i++) {
                g_audioRawBuffer.erase(g_audioRawBuffer.begin());
                g_audioRawBuffer.push_back(samples[i * 2]);
            }
        } else if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            std::fill(g_audioRawBuffer.begin(), g_audioRawBuffer.end(), 0.0f);
        }
        if (SUCCEEDED(hr)) g_pCaptureClient->ReleaseBuffer(numFrames);
        g_pCaptureClient->GetNextPacketSize(&packetLength);
    }

    float peakAmp = 0.0f;
    for (float s : g_audioRawBuffer) peakAmp = (std::max)(peakAmp, std::abs(s));

    const float NOISE_GATE_THRESHOLD = 0.012f;

    if (peakAmp < NOISE_GATE_THRESHOLD) {
        for (size_t i = 0; i < g_audioFFTBins.size(); i++) {
            g_audioFFTBins[i] *= 0.65f;
            if (g_audioFFTBins[i] < 0.05f) g_audioFFTBins[i] = 0.0f;
        }
    } else {
        std::vector<float> real = g_audioRawBuffer;
        std::vector<float> imag(FFT_SIZE, 0.0f);

        for (int i = 0; i < FFT_SIZE; i++) {
            float window = 0.5f * (1.0f - std::cos(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));
            real[i] *= window;
        }

        SimpleFFT(real, imag);

        int numBins = (int)g_audioFFTBins.size();
        for (int i = 0; i < numBins; i++) {
            int startIdx = (int)std::pow(2, i * 8.0 / numBins);
            int endIdx = (int)std::pow(2, (i + 1) * 8.0 / numBins);
            endIdx = std::clamp(endIdx, startIdx + 1, FFT_SIZE / 2);

            float magnitude = 0.0f;
            for (int k = startIdx; k < endIdx; k++) {
                magnitude += std::sqrt(real[k] * real[k] + imag[k] * imag[k]);
            }
            magnitude /= (endIdx - startIdx);
            float targetVal = std::clamp(magnitude * 160.0f, 0.0f, 100.0f);

            g_audioFFTBins[i] = g_audioFFTBins[i] * 0.5f + targetVal * 0.5f;
        }
    }

    UpdateNowPlayingMetadata();
}

// Display & Hardware Enumeration
LRESULT CALLBACK IdentifyOverlayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rect;
        GetClientRect(hwnd, &rect);
        
        HBRUSH hBrush = CreateSolidBrush(RGB(15, 18, 26));
        FillRect(hdc, &rect, hBrush);
        DeleteObject(hBrush);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 210, 255));
        HFONT hFont = CreateFontA(180, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
        HFONT hOldFont = (HFONT)SelectObject(hdc, hFont);

        int monIndex = (int)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
        char buf[16];
        sprintf(buf, "%d", monIndex);

        DrawTextA(hdc, buf, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        SelectObject(hdc, hOldFont);
        DeleteObject(hFont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_KEYDOWN) {
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

void TriggerMonitorIdentifyOverlay() {
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = IdentifyOverlayWndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "MonitorIdentifyOverlayClass";
    RegisterClassA(&wc);

    for (size_t i = 0; i < g_monitors.size(); i++) {
        const auto& mon = g_monitors[i];
        int w = mon.rect.right - mon.rect.left;
        int h = mon.rect.bottom - mon.rect.top;

        HWND hwndOverlay = CreateWindowExA(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            "MonitorIdentifyOverlayClass",
            "Identify",
            WS_POPUP | WS_VISIBLE,
            mon.rect.left, mon.rect.top, w, h,
            NULL, NULL, GetModuleHandle(NULL), NULL
        );

        SetLayeredWindowAttributes(hwndOverlay, 0, 230, LWA_ALPHA);
        SetWindowLongPtrA(hwndOverlay, GWLP_USERDATA, (LONG_PTR)(i + 1));
        SetTimer(hwndOverlay, 1, 2500, [](HWND hwnd, UINT, UINT_PTR, DWORD) {
            DestroyWindow(hwnd);
        });
    }
}

BOOL CALLBACK MonitorLayoutProc(HMONITOR hMonitor, HDC hdcMonitor, LPRECT lprcMonitor, LPARAM dwData) {
    MONITORINFOEXA mi;
    mi.cbSize = sizeof(MONITORINFOEXA);
    if (GetMonitorInfoA(hMonitor, &mi)) {
        DisplayLayoutInfo disp;
        disp.deviceName = mi.szDevice;
        disp.rect = mi.rcMonitor;
        disp.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        disp.uiOffset = ImVec2(0, 0);

        DEVMODEA dm;
        ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
            disp.rotationDegrees = dm.dmDisplayOrientation * 90;
        } else {
            disp.rotationDegrees = 0;
        }

        g_monitors.push_back(disp);
    }
    return TRUE;
}

void FetchDisplayLayout() {
    g_monitors.clear();
    EnumDisplayMonitors(NULL, NULL, MonitorLayoutProc, 0);
}

static const PROPERTYKEY LOCAL_PKEY_Device_FriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

void FetchRealDevices() {
    g_deviceList.clear();
    std::set<std::string> seenNames;

    // -------------------------------------------------------------
    // 1. AUDIO ENDPOINTS (MMDevice API) - Checks ALL states
    // -------------------------------------------------------------
    if (g_pEnumerator != nullptr) {
        IMMDeviceCollection* pCollection = nullptr;
        // Enum all endpoints (Active, Unplugged, Disabled, NotPresent)
        if (SUCCEEDED(g_pEnumerator->EnumAudioEndpoints(eAll, DEVICE_STATEMASK_ALL, &pCollection))) {
            UINT count = 0;
            pCollection->GetCount(&count);
            for (UINT i = 0; i < count; i++) {
                IMMDevice* pEndpoint = nullptr;
                if (SUCCEEDED(pCollection->Item(i, &pEndpoint))) {
                    
                    // Check active state
                    DWORD dwState = 0;
                    pEndpoint->GetState(&dwState);
                    std::string statusStr = (dwState == DEVICE_STATE_ACTIVE) ? "Connected" : "Not Connected";

                    IPropertyStore* pProps = nullptr;
                    if (SUCCEEDED(pEndpoint->OpenPropertyStore(STGM_READ, &pProps))) {
                        PROPVARIANT varName;
                        PropVariantInit(&varName);
                        
                        if (SUCCEEDED(pProps->GetValue(LOCAL_PKEY_Device_FriendlyName, &varName))) {
                            if (varName.vt == VT_LPWSTR && varName.pwszVal != nullptr) {
                                char nameBuf[256] = {0};
                                WideCharToMultiByte(CP_ACP, 0, varName.pwszVal, -1, nameBuf, sizeof(nameBuf), NULL, NULL);
                                
                                std::string audioName = nameBuf;
                                if (!audioName.empty() && seenNames.find(audioName) == seenNames.end()) {
                                    g_deviceList.push_back({ audioName, "Audio Device", statusStr });
                                    seenNames.insert(audioName);
                                }
                            }
                            PropVariantClear(&varName);
                        }
                        pProps->Release();
                    }
                    pEndpoint->Release();
                }
            }
            pCollection->Release();
        }
    }

    // -------------------------------------------------------------
    // 2. OTHER HARDWARE (Mice, Keyboards, USB, Bluetooth, Displays)
    // -------------------------------------------------------------
    HDEVINFO hDevInfo = SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (hDevInfo == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA deviceInfoData;
    deviceInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hDevInfo, i, &deviceInfoData); i++) {
        char nameBuffer[256] = {0};
        char classBuffer[128] = {0};

        if (!SetupDiGetDeviceRegistryPropertyA(hDevInfo, &deviceInfoData, SPDRP_FRIENDLYNAME, NULL, (PBYTE)nameBuffer, sizeof(nameBuffer), NULL)) {
            SetupDiGetDeviceRegistryPropertyA(hDevInfo, &deviceInfoData, SPDRP_DEVICEDESC, NULL, (PBYTE)nameBuffer, sizeof(nameBuffer), NULL);
        }

        SetupDiGetDeviceRegistryPropertyA(hDevInfo, &deviceInfoData, SPDRP_CLASS, NULL, (PBYTE)classBuffer, sizeof(classBuffer), NULL);

        std::string devName = nameBuffer;
        std::string devClass = classBuffer;

        if (devName.empty()) continue;

        // Ignore internal OS infrastructure nodes
        if (devClass == "System" || devClass == "SoftwareDevice" || devClass == "Computer" || 
            devClass == "Volume" || devClass == "Processor" || devClass == "Net" || devClass == "AudioEndpoint" ||
            devName.find("Root") != std::string::npos || devName.find("Volume") != std::string::npos) {
            continue;
        }

        // Query active PnP driver state via Configuration Manager
        ULONG status = 0;
        ULONG problemNumber = 0;
        CONFIGRET cr = CM_Get_DevNode_Status(&status, &problemNumber, deviceInfoData.DevInst, 0);

        // Active devices are running (DN_STARTED) with no problem flags
        bool isConnected = (cr == CR_SUCCESS) && (status & DN_STARTED) && !(status & DN_HAS_PROBLEM);
        std::string statusStr = isConnected ? "Connected" : "Not Connected";

        if (seenNames.find(devName) != seenNames.end()) continue;
        seenNames.insert(devName);

        // Categorize non-audio peripherals
        std::string category = "Hardware Peripheral";
        if (devClass == "Mouse") {
            category = "Pointing Device (Mouse)";
        } else if (devClass == "Keyboard") {
            category = "Keyboard";
        } else if (devClass == "HIDClass") {
            category = "USB / HID Input Device";
        } else if (devClass == "USB") {
            category = "USB Controller / Hub";
        } else if (devClass == "Bluetooth") {
            category = "Bluetooth Device";
        } else if (devClass == "Display") {
            category = "Display Adapter";
        }

        g_deviceList.push_back({ devName, category, statusStr });

        if (g_deviceList.size() >= 100) break;
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
}

void UpdateMetrics() {
    UpdateHardwareTelemetry();

    static ULONGLONG lastHistoryPush = 0;
    if (GetTickCount64() - lastHistoryPush >= 1000) {
        g_cpuHistory.erase(g_cpuHistory.begin());
        g_cpuHistory.push_back(g_cpuUsage);

        g_gpuHistory.erase(g_gpuHistory.begin());
        g_gpuHistory.push_back(g_gpuUsagePct);
        lastHistoryPush = GetTickCount64();
    }

    MEMORYSTATUSEX memInfo;
    memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        g_ramUsagePct = (float)memInfo.dwMemoryLoad;
        g_totalRAM_MB = (DWORD)(memInfo.ullTotalPhys / (1024 * 1024));
        g_usedRAM_MB = g_totalRAM_MB - (DWORD)(memInfo.ullAvailPhys / (1024 * 1024));
    }

    DWORD aProcesses[2048], cbNeeded;
    if (EnumProcesses(aProcesses, sizeof(aProcesses), &cbNeeded)) {
        g_processCount = cbNeeded / sizeof(DWORD);
    }

    FetchAllDrives();
    UpdateWASAPIFrequencies();
}

void ApplyFluentFlyoutTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 10.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(14, 14);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]           = ImVec4(0.08f, 0.09f, 0.12f, 0.95f);
    colors[ImGuiCol_Border]             = ImVec4(0.22f, 0.25f, 0.32f, 0.60f);
    colors[ImGuiCol_Header]             = ImVec4(0.18f, 0.22f, 0.30f, 1.00f);
    colors[ImGuiCol_HeaderHovered]      = ImVec4(0.24f, 0.30f, 0.42f, 1.00f);
    colors[ImGuiCol_Button]             = ImVec4(0.15f, 0.19f, 0.26f, 1.00f);
    colors[ImGuiCol_ButtonHovered]      = ImVec4(0.00f, 0.52f, 0.88f, 1.00f);
    colors[ImGuiCol_ButtonActive]       = ImVec4(0.00f, 0.40f, 0.70f, 1.00f);
    colors[ImGuiCol_FrameBg]            = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_PlotLines]          = ImVec4(0.00f, 0.82f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotHistogram]      = ImVec4(0.20f, 0.90f, 0.50f, 1.00f);
}

// Applies real OS-level rotation using ChangeDisplaySettingsExA
bool RotateMonitorDevice(const std::string& deviceName, int targetDegrees) {
    DEVMODEA dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);

    if (!EnumDisplaySettingsA(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
        return false;
    }

    DWORD newOrientation = DMDO_DEFAULT;
    switch (targetDegrees) {
        case 90:  newOrientation = DMDO_270; break; // Win32 DMDO_270 is 90° Clockwise
        case 180: newOrientation = DMDO_180; break;
        case 270: newOrientation = DMDO_90;  break;
        case 0:   
        default:  newOrientation = DMDO_DEFAULT; break;
    }

    // Check if orientation changes between Landscape and Portrait
    bool isCurrentlyPortrait = (dm.dmDisplayOrientation == DMDO_90 || dm.dmDisplayOrientation == DMDO_270);
    bool willBePortrait      = (newOrientation == DMDO_90 || newOrientation == DMDO_270);

    // Swap physical pixel dimensions when toggling landscape <-> portrait
    if (isCurrentlyPortrait != willBePortrait) {
        std::swap(dm.dmPelsWidth, dm.dmPelsHeight);
    }

    dm.dmDisplayOrientation = newOrientation;
    dm.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;

    LONG result = ChangeDisplaySettingsExA(deviceName.c_str(), &dm, NULL, CDS_UPDATEREGISTRY, NULL);
    return (result == DISP_CHANGE_SUCCESSFUL);
}

// Sets the target monitor as the primary display in Windows
bool SetPrimaryMonitorDevice(const std::string& deviceName) {
    DEVMODEA dm;
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);

    if (!EnumDisplaySettingsA(deviceName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) {
        return false;
    }

    int offsetX = dm.dmPosition.x;
    int offsetY = dm.dmPosition.y;

    dm.dmPosition.x = 0;
    dm.dmPosition.y = 0;
    dm.dmFields = DM_POSITION;

    LONG res1 = ChangeDisplaySettingsExA(deviceName.c_str(), &dm, NULL, CDS_SET_PRIMARY | CDS_UPDATEREGISTRY, NULL);

    DISPLAY_DEVICEA dd;
    ZeroMemory(&dd, sizeof(dd));
    dd.cb = sizeof(dd);

    for (DWORD i = 0; EnumDisplayDevicesA(NULL, i, &dd, 0); i++) {
        if (std::string(dd.DeviceName) == deviceName) continue;

        DEVMODEA otherDm;
        ZeroMemory(&otherDm, sizeof(otherDm));
        otherDm.dmSize = sizeof(otherDm);

        if (EnumDisplaySettingsA(dd.DeviceName, ENUM_CURRENT_SETTINGS, &otherDm)) {
            otherDm.dmPosition.x -= offsetX;
            otherDm.dmPosition.y -= offsetY;
            otherDm.dmFields = DM_POSITION;
            ChangeDisplaySettingsExA(dd.DeviceName, &otherDm, NULL, CDS_UPDATEREGISTRY, NULL);
        }
    }

    ChangeDisplaySettingsExA(NULL, NULL, NULL, 0, NULL);
    return (res1 == DISP_CHANGE_SUCCESSFUL);
}

// --- Main Entry ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(NULL), NULL, NULL, NULL, NULL, L"FluentDashboard", NULL };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Fluent System Control", WS_OVERLAPPEDWINDOW, 100, 100, 1150, 750, NULL, NULL, wc.hInstance, NULL);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    ApplyFluentFlyoutTheme();
    InitWASAPILoopback();
    FetchDisplayLayout();
    FetchRealDevices();
    FetchAllDrives();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    bool done = false;
    char consoleBuf[256] = "";
    int selectedMonitor = 0;

    while (!done) {
        MSG msg;
        while (::PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        UpdateMetrics();
        UpdateCpuBenchmarkUIState();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);
        ImGui::Begin("FlyoutPanel", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove);

        ImGui::TextColored(ImVec4(0.0f, 0.82f, 1.0f, 1.0f), "FLUENT SYSTEM CONTROL");
        ImGui::SameLine(0.0f, 15.0f);
        ImGui::TextDisabled("| Unified Modern Control Flyout");
        ImGui::Separator();

        if (ImGui::BeginTabBar("MainTabBar")) {

            // TAB 1: TELEMETRY & AUDIO FFT
            if (ImGui::BeginTabItem("Telemetry & Audio FFT")) {
                ImGui::Columns(3, "TopRow", true);

                // CPU & GPU TELEMETRY COLUMN
                ImGui::Text("CPU Load: %.1f%%", g_cpuUsage);
                ImGui::PlotLines("##CPUPlot", g_cpuHistory.data(), (int)g_cpuHistory.size(), 0, nullptr, 0.0f, 100.0f, ImVec2(-1, 55));

                ImGui::Text("GPU: %s", g_gpuName.c_str());
                ImGui::Text("GPU Load: %.1f%% (VRAM: %.0f/%.0f MB)", g_gpuUsagePct, g_gpuVRAM_UsedMB, g_gpuVRAM_TotalMB);
                ImGui::PlotLines("##GPUPlot", g_gpuHistory.data(), (int)g_gpuHistory.size(), 0, nullptr, 0.0f, 100.0f, ImVec2(-1, 55));

                ImGui::Text("RAM Load: %.1f%% (%lu / %lu MB)", g_ramUsagePct, g_usedRAM_MB, g_totalRAM_MB);
                ImGui::ProgressBar(g_ramUsagePct / 100.0f, ImVec2(-1, 14));

                // STORAGE COLUMN
                ImGui::NextColumn();
                ImGui::TextColored(ImVec4(0.0f, 0.82f, 1.0f, 1.0f), "STORAGE & DRIVES");
                
                for (size_t i = 0; i < g_driveList.size(); i++) {
                    const auto& drive = g_driveList[i];
                    float usedGB = drive.totalGB - drive.freeGB;
                    float pct = (drive.totalGB > 0) ? (usedGB / drive.totalGB) * 100.0f : 0.0f;

                    std::string typeTag = (drive.type == DRIVE_REMOVABLE) ? "[USB/External]" : "[Internal]";
                    ImGui::Text("%s %s (%s): %.1f GB Free / %.1f GB", typeTag.c_str(), drive.label.c_str(), drive.letter.c_str(), drive.freeGB, drive.totalGB);
                    
                    std::string barId = "##DriveBar_" + drive.letter;
                    ImGui::ProgressBar(pct / 100.0f, ImVec2(-1, 14), barId.c_str());
                }

                ImGui::Spacing();
                ImGui::Text("Active OS Processes: %lu", g_processCount);

                // NOW PLAYING MEDIA COLUMN
                ImGui::NextColumn();
                ImGui::TextColored(ImVec4(0.0f, 0.82f, 1.0f, 1.0f), "NOW PLAYING");
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", g_currentTrackTitle.c_str());
                ImGui::TextDisabled("Artist / Source: %s", g_currentTrackArtist.c_str());
                ImGui::Spacing();

                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.5f, 1.0f), "Calibrated Audio Spectrum");
                ImGui::PlotHistogram("##AudioFFT", g_audioFFTBins.data(), (int)g_audioFFTBins.size(), 0, nullptr, 0.0f, 100.0f, ImVec2(-1, 70));

                ImGui::Columns(1);
                ImGui::EndTabItem();
            }

            // TAB 2: INTERACTIVE DISPLAYS LAYOUT
            if (ImGui::BeginTabItem("Display Layout & Rearrange")) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Interactive Displays Layout (Drag to rearrange)");
                ImGui::SameLine();
                if (ImGui::Button("Identify Monitors")) {
                    TriggerMonitorIdentifyOverlay();
                }
                ImGui::SameLine();
                if (ImGui::Button("Reset Layout")) {
                    FetchDisplayLayout();
                }

                ImDrawList* drawList = ImGui::GetWindowDrawList();
                ImVec2 canvasPos = ImGui::GetCursorScreenPos();
                ImVec2 canvasSize = ImVec2(ImGui::GetContentRegionAvail().x, 320.0f);

                drawList->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(18, 20, 26, 255), 8.0f);
                drawList->AddRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), IM_COL32(60, 65, 80, 255), 8.0f);

                ImVec2 centerOffset = ImVec2(canvasPos.x + canvasSize.x * 0.4f, canvasPos.y + canvasSize.y * 0.4f);
                float scale = 0.05f;

                for (size_t i = 0; i < g_monitors.size(); i++) {
                    auto& mon = g_monitors[i];

                    float w = (mon.rect.right - mon.rect.left) * scale;
                    float h = (mon.rect.bottom - mon.rect.top) * scale;

                    if (mon.rotationDegrees == 90 || mon.rotationDegrees == 270) {
                        std::swap(w, h);
                    }

                    ImVec2 pMin = ImVec2(centerOffset.x + mon.rect.left * scale + mon.uiOffset.x, centerOffset.y + mon.rect.top * scale + mon.uiOffset.y);
                    ImVec2 pMax = ImVec2(pMin.x + w, pMin.y + h);

                    ImGui::PushID((int)i);
                    ImGui::SetCursorScreenPos(pMin);
                    ImGui::InvisibleButton("##MonBtn", ImVec2(w, h));

                    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                        ImVec2 delta = ImGui::GetIO().MouseDelta;
                        mon.uiOffset.x += delta.x;
                        mon.uiOffset.y += delta.y;
                    }
                    if (ImGui::IsItemClicked()) {
                        selectedMonitor = (int)i;
                    }

                    bool isSelected = (selectedMonitor == (int)i);
                    ImU32 boxBg = isSelected ? IM_COL32(0, 140, 240, 220) : (mon.isPrimary ? IM_COL32(0, 90, 170, 200) : IM_COL32(40, 48, 65, 200));
                    
                    drawList->AddRectFilled(pMin, pMax, boxBg, 6.0f);
                    drawList->AddRect(pMin, pMax, isSelected ? IM_COL32(255, 255, 255, 255) : IM_COL32(180, 200, 230, 255), 6.0f, 0, isSelected ? 3.0f : 1.5f);

                    char label[64];
                    sprintf(label, "[%d] %s\n%dx%d (%d°)", (int)i + 1, mon.isPrimary ? "Primary" : "Secondary",
                            (int)(mon.rect.right - mon.rect.left), (int)(mon.rect.bottom - mon.rect.top), mon.rotationDegrees);

                    drawList->AddText(ImVec2(pMin.x + 8, pMin.y + 8), IM_COL32(255, 255, 255, 255), label);
                    ImGui::PopID();
                }

                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 330.0f);
                ImGui::Separator();

                if (selectedMonitor >= 0 && selectedMonitor < (int)g_monitors.size()) {
                    auto& selMon = g_monitors[selectedMonitor];
                    ImGui::Text("Selected Monitor [%d]: %s", selectedMonitor + 1, selMon.deviceName.c_str());
                    
                    // MAKE PRIMARY DISPLAY
                    if (ImGui::Button("Make Primary Display")) {
                        if (SetPrimaryMonitorDevice(selMon.deviceName)) {
                            FetchDisplayLayout(); // Refresh monitor bounds and positions in ImGui
                        }
                    }
                    
                    ImGui::SameLine();
                    
                    // ROTATE 90°
                    if (ImGui::Button("Rotate 90°")) {
                        int nextDegrees = (selMon.rotationDegrees + 90) % 360;
                        
                        if (RotateMonitorDevice(selMon.deviceName, nextDegrees)) {
                            FetchDisplayLayout(); // Re-query Windows display configuration so UI layout updates instantly
                        }
                    }
                }

                ImGui::EndTabItem();
            }

            // TAB 3: CONNECTED PERIPHERALS
            if (ImGui::BeginTabItem("Peripherals & Devices")) {
                if (ImGui::Button("Refresh Hardware")) FetchRealDevices();
                ImGui::BeginChild("DevList", ImVec2(-1, -1), true);
                if (ImGui::BeginTable("DevTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                    ImGui::TableSetupColumn("Device Description");
                    ImGui::TableSetupColumn("Category");
                    ImGui::TableSetupColumn("Status");
                    ImGui::TableHeadersRow();

                    for (const auto& dev : g_deviceList) {
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0); ImGui::Text("%s", dev.name.c_str());
                        ImGui::TableSetColumnIndex(1); ImGui::TextDisabled("%s", dev.category.c_str());
                        ImGui::TableSetColumnIndex(2); ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.4f, 1.0f), "%s", dev.status.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            // Tab 4: Real-time Silverbench Raytracer Viewport
            if (ImGui::BeginTabItem("Benchmark & Stress")) {
                ImGui::TextColored(ImVec4(0.0f, 0.85f, 1.0f, 1.0f), "SILVERBENCH CPU RAY TRACING BENCHMARK");
                ImGui::Separator();

                // Init Texture Buffer
                if (!g_pBenchSRV && g_pd3dDevice) {
                    InitBenchmarkTexture(g_pd3dDevice, 640, 360);
                }

                // Streams buffer continuously to DX11 texture while burning CPU
                if (g_benchState.isRunning && g_pd3dDeviceContext) {
                    UpdateBenchmarkTextureGPU(g_pd3dDeviceContext, g_benchState.targetWidth);
                }

                if (g_pBenchSRV) {
                    ImGui::Image((ImTextureID)g_pBenchSRV, ImVec2(640, 360));
                }

                if (!g_benchState.isRunning) {
                    if (ImGui::Button("Run Benchmark (Timed)", ImVec2(190, 35))) {
                        StartCpuBenchmark(false);
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Start Burn Stress Test (Infinite)", ImVec2(210, 35))) {
                        StartCpuBenchmark(true);
                    }
                } else {
                    if (ImGui::Button("Stop Test", ImVec2(408, 35))) {
                        StopCpuBenchmark();
                    }
                }

                ImGui::Spacing();
                ImGui::Separator();

                if (g_benchState.isRunning) {
                    ImGui::ProgressBar(g_benchState.progress, ImVec2(-1, 0));
                    ImGui::Text("Active Threads: %d (100%% CPU Utilization)", g_benchState.numThreads);
                    ImGui::Text("Completed Passes: %u / %d", g_benchState.completedPasses.load(), g_benchState.maxPasses);
                    ImGui::Text("Throughput: %.2f MegaRays/sec", g_benchState.raysPerSecond / 1000000.0f);
                    ImGui::Text("Total Rays Cast: %llu", (unsigned long long)g_benchState.totalRaysCast.load());
                    ImGui::Text("Elapsed Time: %.2f sec", g_benchState.elapsedTime);
                } else if (g_benchState.finalScore > 0.0f) {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "Final Score: %.0f PTS", g_benchState.finalScore);
                    ImGui::Text("Average Throughput: %.2f MegaRays/sec", g_benchState.raysPerSecond / 1000000.0f);
                    ImGui::Text("Total Time: %.2f sec", g_benchState.elapsedTime);
                }

                ImGui::EndTabItem();
            }

            // TAB 5: TERMINAL & LAUNCHER
            if (ImGui::BeginTabItem("Terminal & Apps")) {
                ImGui::TextColored(ImVec4(0.0f, 0.82f, 1.0f, 1.0f), "Quick Launcher:");
                for (const auto& app : g_pinnedApps) {
                    if (ImGui::Button(app.name.c_str(), ImVec2(110, 32))) {
                        ShellExecuteA(NULL, "open", app.command.c_str(), NULL, NULL, SW_SHOW);
                    }
                    ImGui::SameLine();
                }
                ImGui::NewLine();
                ImGui::Separator();

                ImGui::Text("Terminal Command Line:");
                ImGui::SetNextItemWidth(-90);
                ImGui::InputText("##ConsoleCmd", consoleBuf, IM_ARRAYSIZE(consoleBuf));
                ImGui::SameLine();
                if (ImGui::Button("Run", ImVec2(80, 0))) {
                    if (strlen(consoleBuf) > 0) {
                        std::string cmd = "/k " + std::string(consoleBuf);
                        ShellExecuteA(NULL, "open", "cmd.exe", cmd.c_str(), NULL, SW_SHOW);
                        consoleBuf[0] = '\0';
                    }
                }
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();

        ImGui::Render();
        const float clear_color[4] = { 0.05f, 0.05f, 0.06f, 1.00f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0);
    }

    // Cleanup Resources
    if (g_pdhQuery) {
        PdhCloseQuery(g_pdhQuery);
    }

    if (g_pAudioClient) g_pAudioClient->Stop();
    if (g_pCaptureClient) g_pCaptureClient->Release();
    if (g_pAudioClient) g_pAudioClient->Release();
    if (g_pAudioDevice) g_pAudioDevice->Release();
    if (g_pEnumerator) g_pEnumerator->Release();
    CoUninitialize();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}

// Direct3D 11 Setup Helpers
bool CreateDeviceD3D(HWND hWnd) {
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
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}