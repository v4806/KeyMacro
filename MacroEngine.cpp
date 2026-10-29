#include "MacroEngine.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <TlHelp32.h>
#include <chrono>
#include <thread>
#include <set>
#include <mutex>
#include <vector>
#include <future>
#include <condition_variable>
#include <climits>
#include <QImage>
#include <QDebug>
#include <QTime>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.System.Threading.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <DispatcherQueue.h>

// 获取带毫秒的时间戳字符串
inline QString getTimestamp()
{
    return QTime::currentTime().toString("hh:mm:ss.zzz");
}

#ifdef _DEBUG
// 支持中文的可变参数日志宏（使用流式输出避免编码问题）
#define LOG_DEBUG(...) qDebug().noquote().nospace() << "[" << getTimestamp() << "][ENGINE] " << __VA_ARGS__
#else
#define LOG_DEBUG(...) ((void)0)
#endif

// （画面截取功能 - 照搬自 OpenCV_Clicker 项目）
using namespace winrt;

// 强制让系统光标可见（解决宏失活后鼠标消失的BUG）
static void forceShowCursor()
{
    for (int i = 0; i < 10; ++i)
        ShowCursor(TRUE);
}

// 内部用 VK_XBUTTON1/2(5/6) 表示鼠标侧键，但 SendInput 的 INPUT_MOUSE.mouseData
// 要求 XBUTTON1=0x01 / XBUTTON2=0x02，二者必须显式映射，否则侧键回放无效。
static inline DWORD vkToXButtonMouseData(DWORD vk)
{
    if (vk == VK_XBUTTON1)
        return XBUTTON1;
    if (vk == VK_XBUTTON2)
        return XBUTTON2;
    return 0;
}

// ===== 事件驱动 WGC 捕获器 =====
class WGCCapture
{
public:
    WGCCapture()
    {
        try
        {
            // 创建原生 D3D11 设备（失败时保持 m_available=false，由调用方回退 GDI，不抛异常）
            winrt::com_ptr<ID3D11Device> nativeDevice;
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                           D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION,
                                           nativeDevice.put(), nullptr, nullptr);
            if (FAILED(hr) || !nativeDevice)
                return;
            m_nativeD3DDevice = nativeDevice;

            winrt::com_ptr<IDXGIDevice> dxgiDevice;
            nativeDevice.as(dxgiDevice);
            if (!dxgiDevice)
                return;

            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(
                dxgiDevice.get(),
                reinterpret_cast<IInspectable **>(winrt::put_abi(m_d3dDevice))));
            m_available = true;
        }
        catch (...)
        {
            m_available = false;
        }
    }

    bool available() const { return m_available; }

    // 尝试捕获一帧（非阻塞，立即返回）
    cv::Mat CaptureNow()
    {
        if (!m_available || !m_framePool)
            return cv::Mat();

        auto frame = m_framePool.TryGetNextFrame();
        if (!frame)
            return cv::Mat();

        return FrameToGray(frame);
    }

    // 等待新帧到达（阻塞，直到有帧或返回 false 表示停止）
    bool WaitForFrame()
    {
        std::unique_lock<std::mutex> lock(m_frameMutex);
        m_frameReady = false;
        return m_frameCv.wait_for(lock, std::chrono::milliseconds(500),
                                  [this]
                                  { return m_frameReady || m_stopWaiting; });
    }

    // 启动/重建捕获会话
    bool StartCapture(HWND hwnd)
    {
        CloseSession();
        m_hwnd = hwnd;
        if (!m_available)
            return false;
        try
        {
            m_item = CreateCaptureItem(hwnd);
            if (!m_item)
                return false;

            m_framePool = winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::Create(
                m_d3dDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                1,
                m_item.Size());

            // 注册帧到达事件
            m_framePool.FrameArrived([this](auto &&, auto &&)
                                     {
                std::lock_guard<std::mutex> lock(m_frameMutex);
                m_frameReady = true;
                m_frameCv.notify_one(); });

            m_session = m_framePool.CreateCaptureSession(m_item);
            m_session.StartCapture();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    void StopCapture()
    {
        CloseSession();
        m_stopWaiting = true;
        m_frameCv.notify_all();
    }

    ~WGCCapture()
    {
        StopCapture();
    }

private:
    // 将捕获帧转为灰度 Mat（通过 STAGING 纹理拷贝后按 RowPitch 逐行读取）
    cv::Mat FrameToGray(winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame const &frame)
    {
        winrt::com_ptr<IDXGISurface> dxgiSurface;
        if (FAILED(GetDxgiInterface(frame.Surface(), dxgiSurface.put())) || !dxgiSurface)
            return cv::Mat();

        DXGI_SURFACE_DESC desc{};
        if (FAILED(dxgiSurface->GetDesc(&desc)) || desc.Width == 0 || desc.Height == 0)
            return cv::Mat();

        winrt::com_ptr<ID3D11Texture2D> srcTexture;
        dxgiSurface.as(srcTexture);
        if (!srcTexture)
            return cv::Mat();

        D3D11_TEXTURE2D_DESC td{};
        srcTexture->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        td.MiscFlags = 0;
        td.MipLevels = 1;
        td.ArraySize = 1;

        winrt::com_ptr<ID3D11Texture2D> staging;
        if (FAILED(m_nativeD3DDevice->CreateTexture2D(&td, nullptr, staging.put())) || !staging)
            return cv::Mat();

        winrt::com_ptr<ID3D11DeviceContext> ctx;
        m_nativeD3DDevice->GetImmediateContext(ctx.put());
        if (!ctx)
            return cv::Mat();
        ctx->CopyResource(staging.get(), srcTexture.get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData)
            return cv::Mat();

        cv::Mat rgba(static_cast<int>(desc.Height), static_cast<int>(desc.Width), CV_8UC4);
        const BYTE *src = static_cast<const BYTE *>(mapped.pData);
        const size_t rowBytes = static_cast<size_t>(desc.Width) * 4;
        for (UINT y = 0; y < desc.Height; ++y)
            memcpy(rgba.ptr(y), src + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
        ctx->Unmap(staging.get(), 0);

        cv::Mat gray;
        cv::cvtColor(rgba, gray, cv::COLOR_RGBA2GRAY);
        return gray;
    }

    static HRESULT GetDxgiInterface(
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface const &surface,
        IDXGISurface **dxgi)
    {
        IInspectable *inspectable = reinterpret_cast<IInspectable *>(winrt::get_abi(surface));
        if (!inspectable)
            return E_POINTER;

        winrt::com_ptr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
        HRESULT hr = inspectable->QueryInterface(
            __uuidof(::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess),
            access.put_void());
        if (FAILED(hr))
            return hr;

        return access->GetInterface(__uuidof(IDXGISurface), reinterpret_cast<void **>(dxgi));
    }

    static winrt::Windows::Graphics::Capture::GraphicsCaptureItem CreateCaptureItem(HWND hwnd)
    {
        auto interop = winrt::get_activation_factory<
            winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
            ::IGraphicsCaptureItemInterop>();
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};
        winrt::check_hresult(interop->CreateForWindow(
            hwnd,
            winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
            reinterpret_cast<void **>(winrt::put_abi(item))));
        return item;
    }

    void CloseSession()
    {
        if (m_session)
        {
            m_session.Close();
            m_session = nullptr;
        }
        if (m_framePool)
        {
            m_framePool.Close();
            m_framePool = nullptr;
        }
        m_item = nullptr;
    }

    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_d3dDevice = nullptr;
    winrt::com_ptr<ID3D11Device> m_nativeD3DDevice;
    bool m_available = false;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool = nullptr;
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item = nullptr;
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session = nullptr;
    HWND m_hwnd = nullptr;

    std::mutex m_frameMutex;
    std::condition_variable m_frameCv;
    bool m_frameReady = false;
    bool m_stopWaiting = false;
};

// ==================== 图像识别（加速版：只截前台窗口 + 灰度匹配） ====================
cv::Mat MacroEngine::captureScreen()
{
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMemory = CreateCompatibleDC(hdcScreen);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcScreen, width, height);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMemory, hBitmap);

    BitBlt(hdcMemory, 0, 0, width, height, hdcScreen, x, y, SRCCOPY);

    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    cv::Mat screen(height, width, CV_8UC4);
    GetDIBits(hdcMemory, hBitmap, 0, height, screen.data,
              (BITMAPINFO *)&bi, DIB_RGB_COLORS);

    cv::Mat screenBGR;
    cv::cvtColor(screen, screenBGR, cv::COLOR_BGRA2BGR);

    SelectObject(hdcMemory, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMemory);
    ReleaseDC(NULL, hdcScreen);

    return screenBGR;
}

cv::Mat MacroEngine::captureForegroundGray()
{
    static WGCCapture wgc;
    static HWND lastHwnd = nullptr;
    static int wgcFailStreak = 0; // 连续无帧/失败次数
    static int wgcRetryTick = 0;  // 连续失败后周期性重试 WGC 的计数

    HWND hwnd = GetForegroundWindow();
    if (!hwnd)
    {
        cv::Mat screenBGR = captureScreen();
        cv::Mat screenGray;
        cv::cvtColor(screenBGR, screenGray, cv::COLOR_BGR2GRAY);
        return screenGray;
    }

    // 连续失败时回退 GDI，但每隔约 60 个周期重试一次 WGC，避免一次空帧就永久禁用加速路径
    bool tryWgc = wgc.available() && (wgcFailStreak < 3 || (++wgcRetryTick % 60) == 0);
    if (tryWgc)
    {
        try
        {
            if (hwnd != lastHwnd)
            {
                wgc.StopCapture();
                if (wgc.StartCapture(hwnd))
                {
                    lastHwnd = hwnd;
                    wgcFailStreak = 0;
                }
                else
                {
                    lastHwnd = nullptr;
                    ++wgcFailStreak;
                }
            }

            if (hwnd == lastHwnd)
            {
                // 尝试获取当前帧（非阻塞）；WGC 首帧异步到达，取不到时本周期回退
                cv::Mat gray = wgc.CaptureNow();
                if (!gray.empty())
                {
                    wgcFailStreak = 0;
                    return gray;
                }
                ++wgcFailStreak;
            }
        }
        catch (...)
        {
            ++wgcFailStreak;
        }
    }

    // ===== WGC 不可用/本周期无帧，回退到 PrintWindow =====

    HDC hdcWindow = GetWindowDC(hwnd);
    HDC hdcMemory = CreateCompatibleDC(hdcWindow);
    HBITMAP hBitmap = CreateCompatibleBitmap(hdcWindow, 1, 1);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hdcMemory, hBitmap);

    RECT rect;
    if (!GetWindowRect(hwnd, &rect))
    {
        SelectObject(hdcMemory, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hdcMemory);
        ReleaseDC(hwnd, hdcWindow);

        cv::Mat screenBGR = captureScreen();
        cv::Mat screenGray;
        cv::cvtColor(screenBGR, screenGray, cv::COLOR_BGR2GRAY);
        return screenGray;
    }

    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0)
    {
        SelectObject(hdcMemory, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hdcMemory);
        ReleaseDC(hwnd, hdcWindow);

        cv::Mat screenBGR = captureScreen();
        cv::Mat screenGray;
        cv::cvtColor(screenBGR, screenGray, cv::COLOR_BGR2GRAY);
        return screenGray;
    }

    // 释放 1x1 探测位图（必须先恢复旧位图再删除，否则 DeleteObject 失败导致句柄泄漏）
    SelectObject(hdcMemory, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMemory);

    hBitmap = CreateCompatibleBitmap(hdcWindow, width, height);
    if (!hBitmap)
    {
        ReleaseDC(hwnd, hdcWindow);
        return cv::Mat();
    }
    hdcMemory = CreateCompatibleDC(hdcWindow);
    hOldBitmap = (HBITMAP)SelectObject(hdcMemory, hBitmap);

    if (!PrintWindow(hwnd, hdcMemory, 0x00000002))
    {
        SelectObject(hdcMemory, hOldBitmap);
        DeleteObject(hBitmap);
        DeleteDC(hdcMemory);
        ReleaseDC(hwnd, hdcWindow);
        return cv::Mat();
    }

    // GetDIBits 要求位图不处于被 DC 选中的状态
    SelectObject(hdcMemory, hOldBitmap);

    BITMAPINFOHEADER bi = {0};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = width;
    bi.biHeight = -height;
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;

    cv::Mat screenBGRA(height, width, CV_8UC4);
    if (GetDIBits(hdcMemory, hBitmap, 0, height, screenBGRA.data,
                  (BITMAPINFO *)&bi, DIB_RGB_COLORS) == 0)
    {
        DeleteObject(hBitmap);
        DeleteDC(hdcMemory);
        ReleaseDC(hwnd, hdcWindow);
        return cv::Mat();
    }

    cv::Mat screenGray;
    cv::cvtColor(screenBGRA, screenGray, cv::COLOR_BGRA2GRAY);

    DeleteObject(hBitmap);
    DeleteDC(hdcMemory);
    ReleaseDC(hwnd, hdcWindow);

    return screenGray;
}

// 模板图像缓存：路径 + 修改时间/大小 -> 灰度模板。
// 识别线程每 60ms 一轮，每轮每图都重新读盘解码会持续占用 CPU/磁盘（长时间运行卡顿的主要来源之一）。
struct TemplateCacheEntry
{
    qint64 mtime = -1;
    qint64 size = -1;
    cv::Mat gray;
};
static std::mutex g_templateCacheMutex;
static QHash<QString, TemplateCacheEntry> g_templateCache;

static bool loadTemplateGray(const QString &imgPath, cv::Mat &outGray)
{
    QFileInfo fi(imgPath);
    if (!fi.exists() || !fi.isFile())
        return false;
    const qint64 mtime = fi.lastModified().toMSecsSinceEpoch();
    const qint64 size = fi.size();
    {
        std::lock_guard<std::mutex> lock(g_templateCacheMutex);
        auto it = g_templateCache.find(imgPath);
        if (it != g_templateCache.end() && it->mtime == mtime && it->size == size && !it->gray.empty())
        {
            outGray = it->gray;
            return true;
        }
    }

    // 使用 QImage 加载，避开 OpenCV Debug 库缺少 PNG 解码器的问题
    QImage qimg(imgPath);
    if (qimg.isNull())
        return false;
    QImage grayImg = qimg.convertToFormat(QImage::Format_Grayscale8);
    cv::Mat tmpl(grayImg.height(), grayImg.width(), CV_8UC1,
                 grayImg.bits(), grayImg.bytesPerLine());
    cv::Mat gray = tmpl.clone();

    {
        std::lock_guard<std::mutex> lock(g_templateCacheMutex);
        // 简单的容量上限，避免长期运行/大量换图时缓存无限增长
        if (g_templateCache.size() > 256)
            g_templateCache.clear();
        TemplateCacheEntry entry;
        entry.mtime = mtime;
        entry.size = size;
        entry.gray = gray;
        g_templateCache.insert(imgPath, entry);
    }
    outGray = gray;
    return true;
}

bool MacroEngine::matchGroupImages(const QStringList &imagePaths, double threshold,
                                   const cv::Mat &screenGray,
                                   QVector<ImageLearnData> &learnData,
                                   int tolerancePx)
{
    LOG_DEBUG("[IMG-MATCH] 开始并行匹配，图像数=" << imagePaths.size()
                                                  << " 窗口=" << screenGray.cols << "x" << screenGray.rows
                                                  << " 阈值=" << threshold << " 容错=" << tolerancePx);

    if (imagePaths.isEmpty())
    {
        LOG_DEBUG("[IMG-MATCH] 无目标图像，直接返回匹配成功");
        return true;
    }

    if (threshold <= 0.0)
        threshold = 0.8;
    if (tolerancePx <= 0)
        tolerancePx = 50;

    // 确保 learnData 与 imagePaths 大小一致
    while (learnData.size() < imagePaths.size())
        learnData.append(ImageLearnData());

    // 并行启动所有图像的匹配任务
    std::vector<std::future<bool>> futures;
    futures.reserve(imagePaths.size());

    for (int i = 0; i < imagePaths.size(); ++i)
    {
        // 预取指针避免多线程并发调用 QVector::operator[] 导致 COW 竞态
        ImageLearnData *ldPtr = &learnData[i];
        futures.push_back(std::async(std::launch::async,
                                     [i, &imagePaths, threshold, &screenGray, ldPtr, tolerancePx]() -> bool
                                     {
                                         const QString &imgPath = imagePaths[i];
                                         // 以 mtime/size 为键读取模板缓存，避免每 60ms 每图重新读盘解码
                                         cv::Mat tmpl;
                                         if (!loadTemplateGray(imgPath, tmpl))
                                         {
                                             LOG_DEBUG("[IMG-MATCH] 模板加载失败: " << imgPath);
                                             return false;
                                         }
                                         auto &ld = *ldPtr;
                                         double bestScore = 0.0;

                                         // 策略1：已学习位置 → ROI 区域匹配
                                         if (ld.centerX >= 0 && ld.centerY >= 0 && ld.tw > 0 && ld.th > 0)
                                         {
                                             int roiX = std::max(0, ld.centerX - ld.tw / 2 - tolerancePx);
                                             int roiY = std::max(0, ld.centerY - ld.th / 2 - tolerancePx);
                                             int roiW = ld.tw + 2 * tolerancePx;
                                             int roiH = ld.th + 2 * tolerancePx;
                                             if (roiX + roiW > screenGray.cols)
                                                 roiW = screenGray.cols - roiX;
                                             if (roiY + roiH > screenGray.rows)
                                                 roiH = screenGray.rows - roiY;

                                             if (roiW >= tmpl.cols && roiH >= tmpl.rows)
                                             {
                                                 cv::Mat roi = screenGray(cv::Rect(roiX, roiY, roiW, roiH));
                                                 cv::Mat result;
                                                 cv::matchTemplate(roi, tmpl, result, cv::TM_CCOEFF_NORMED);
                                                 double maxVal;
                                                 cv::minMaxLoc(result, nullptr, &maxVal, nullptr, nullptr);
                                                 bestScore = maxVal;
                                                 if (bestScore >= threshold)
                                                     return true;
                                             }
                                         }
                                         else
                                         {
                                             // 无学习数据 → 全窗口匹配（仅首次）
                                             LOG_DEBUG("[IMG-MATCH] 组内图像[" << i << "] 全窗口匹配, 模板:"
                                                                               << tmpl.cols << "x" << tmpl.rows
                                                                               << " 屏幕:" << screenGray.cols << "x" << screenGray.rows);
                                             if (tmpl.cols <= screenGray.cols && tmpl.rows <= screenGray.rows)
                                             {
                                                 cv::Mat result;
                                                 cv::matchTemplate(screenGray, tmpl, result, cv::TM_CCOEFF_NORMED);
                                                 double maxVal;
                                                 cv::minMaxLoc(result, nullptr, &maxVal, nullptr, nullptr);
                                                 bestScore = maxVal;
                                                 LOG_DEBUG("[IMG-MATCH] 全窗口匹配得分: " << bestScore);
                                                 if (bestScore >= threshold)
                                                 {
                                                     // 记录学习位置
                                                     cv::Point maxLoc;
                                                     cv::minMaxLoc(result, nullptr, nullptr, nullptr, &maxLoc);
                                                     ld.centerX = maxLoc.x + tmpl.cols / 2;
                                                     ld.centerY = maxLoc.y + tmpl.rows / 2;
                                                     ld.tw = tmpl.cols;
                                                     ld.th = tmpl.rows;
                                                     return true;
                                                 }
                                             }
                                             else
                                             {
                                                 LOG_DEBUG("[IMG-MATCH] 模板尺寸大于屏幕，无法匹配");
                                             }
                                         }
                                         return false;
                                     }));
    }

    // 等待任意一个任务成功，或全部失败
    bool anyMatched = false;
    for (auto &f : futures)
    {
        if (f.get())
        {
            anyMatched = true;
            // 不要 break，保持等待所有任务结束，避免 learnData 竞争
        }
    }

    LOG_DEBUG("[IMG-MATCH] 并行匹配结果: " << (anyMatched ? "✅ 成功" : "❌ 全部未匹配"));
    return anyMatched;
}

// ==================== 后台图像匹配线程 ====================

void MacroEngine::rebuildImageCheckItems()
{
    std::lock_guard<std::mutex> lock(m_imageCheckMutex);
    m_imageCheckItems.clear();

    LOG_DEBUG("[IMG-CHECK] rebuildImageCheckItems: 共 " << m_groups.size() << " 个宏组");

    int groupsWithImages = 0;
    for (int i = 0; i < m_groups.size(); ++i)
    {
        const auto &grp = m_groups[i];
        LOG_DEBUG("[IMG-CHECK]   组[" << i << "] \"" << grp.name
                                      << "\" targetExe=\"" << grp.targetExe
                                      << "\" 图像数=" << grp.imagePaths.size()
                                      << " 阈值=" << grp.matchThreshold);

        if (grp.imagePaths.isEmpty())
        {
            LOG_DEBUG("[IMG-CHECK]   组[" << i << "] 无图像，跳过");
            continue;
        }

        for (int j = 0; j < grp.imagePaths.size(); ++j)
        {
            LOG_DEBUG("[IMG-CHECK]   组[" << i << "] 图像[" << j << "]: " << grp.imagePaths[j]);
        }

        ImageCheckItem item;
        item.groupIndex = i;
        item.targetExe = grp.targetExe;
        item.imagePaths = grp.imagePaths;
        item.matchThreshold = grp.matchThreshold;
        item.imageTolerancePx = grp.imageTolerancePx;
        item.imageLearnData = grp.imageLearnData;
        m_imageCheckItems.push_back(item);
        ++groupsWithImages;
    }

    LOG_DEBUG("[IMG-CHECK] rebuildImageCheckItems 完成: " << groupsWithImages
                                                          << " 个组有待匹配图像, 共 " << m_imageCheckItems.size() << " 条检查项");

    // 唤醒可能正在等待的图像匹配线程，避免死等
    m_imageCv.notify_one();
}

bool MacroEngine::getImageMatchResult(int groupIndex) const
{
    // 快速检查：如果该组没有图像配置，直接返回 true
    {
        std::lock_guard<std::mutex> lock(m_imageCheckMutex);
        bool hasImages = false;
        for (const auto &item : m_imageCheckItems)
        {
            if (item.groupIndex == groupIndex)
            {
                hasImages = true;
                break;
            }
        }
        if (!hasImages)
        {
            LOG_DEBUG("[IMG-RESULT] getImageMatchResult(组[" << groupIndex << "]): 该组无图像配置 → 返回 true");
            return true;
        }
    }

    std::lock_guard<std::mutex> lock(m_imageResultMutex);
    auto it = m_imageMatchResults.find(groupIndex);
    if (it != m_imageMatchResults.end())
    {
        LOG_DEBUG("[IMG-RESULT] getImageMatchResult(组[" << groupIndex << "]): 缓存命中 → "
                                                         << (it->second ? "true(匹配通过)" : "false(匹配失败)"));
        return it->second;
    }

    LOG_DEBUG("[IMG-RESULT] getImageMatchResult(组[" << groupIndex << "]): 缓存未命中(后台线程尚无结果) → 返回 false");
    return false;
}

void MacroEngine::imageMatchWorkerFunc()
{
    LOG_DEBUG("Image match worker started (fixed 60ms interval fully parallel)");

    std::map<int, bool> previousMatchState;

    while (!m_imageMatchStop.load())
    {
        // 后台线程不允许异常逃逸：任何未捕获异常都会 std::terminate 整个进程
        try
        {
            std::vector<ImageCheckItem> itemsCopy;
            {
                std::lock_guard<std::mutex> lock(m_imageCheckMutex);
                itemsCopy = m_imageCheckItems;
            }

        if (!itemsCopy.empty())
        {
            // 校验前台窗口是否属于目标程序，避免在非目标窗口上启动 WGC 捕获（产生黄色提示边框）
            HWND fgHwnd = GetForegroundWindow();
            QString fgProc = getProcessNameFromWindow(fgHwnd);
            bool fgMatchesAnyTarget = false;
            for (const auto &item : itemsCopy)
            {
                if (!item.targetExe.isEmpty() && fgProc.compare(item.targetExe, Qt::CaseInsensitive) == 0)
                {
                    fgMatchesAnyTarget = true;
                    break;
                }
            }
            if (!fgMatchesAnyTarget)
            {
                // 前台窗口不是任何目标程序，跳过本轮捕获
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                continue;
            }

            // 重置新加入组的 previousMatchState，防止快速切换窗口导致的竞争条件
            {
                std::lock_guard<std::mutex> lock(m_imageCheckMutex);
                for (int idx : m_pendingMatchStateReset)
                    previousMatchState.erase(idx);
                m_pendingMatchStateReset.clear();
            }

            // 直接尝试获取当前帧（非阻塞）
            cv::Mat screenGray = captureForegroundGray();
            if (!screenGray.empty())
            {
                // 记录匹配前的学习状态，用于判断本轮是否首次学习到位置
                std::vector<QVector<ImageLearnData>> prevLearnData;
                prevLearnData.reserve(itemsCopy.size());
                for (const auto &item : itemsCopy)
                    prevLearnData.push_back(item.imageLearnData);

                // ========== 完全并行匹配所有组 ==========
                std::vector<std::future<std::pair<int, bool>>> futures;
                futures.reserve(itemsCopy.size());

                for (auto &item : itemsCopy)
                {
                    futures.push_back(std::async(std::launch::async,
                                                 [&item, &screenGray]() -> std::pair<int, bool>
                                                 {
                                                     bool matched = matchGroupImages(item.imagePaths,
                                                                                     item.matchThreshold,
                                                                                     screenGray,
                                                                                     item.imageLearnData,
                                                                                     item.imageTolerancePx);
                                                     return {item.groupIndex, matched};
                                                 }));
                }

                std::map<int, bool> currentResults;
                for (auto &f : futures)
                {
                    auto res = f.get();
                    currentResults[res.first] = res.second;
                }

                // 同步学习数据到后台快照；持久化由主线程 onImageLearned 校验后完成
                {
                    std::lock_guard<std::mutex> lock(m_imageCheckMutex);
                    for (size_t k = 0; k < itemsCopy.size(); ++k)
                    {
                        auto &item = itemsCopy[k];
                        for (auto &orig : m_imageCheckItems)
                        {
                            if (orig.groupIndex == item.groupIndex)
                            {
                                orig.imageLearnData = item.imageLearnData;
                                break;
                            }
                        }
                        int gIdx = item.groupIndex;
                        int imageCount = qMin(item.imageLearnData.size(), item.imagePaths.size());
                        for (int j = 0; j < imageCount; ++j)
                        {
                            const auto &ld = item.imageLearnData[j];
                            if (ld.centerX < 0)
                                continue;
                            bool alreadyKnown = (j < prevLearnData[k].size()) &&
                                                (prevLearnData[k][j].centerX >= 0);
                            if (!alreadyKnown)
                            {
                                QMetaObject::invokeMethod(this, "onImageLearned", Qt::QueuedConnection,
                                                          Q_ARG(int, gIdx), Q_ARG(int, j),
                                                          Q_ARG(int, ld.centerX), Q_ARG(int, ld.centerY),
                                                          Q_ARG(int, ld.tw), Q_ARG(int, ld.th));
                            }
                        }
                    }
                }

                // 更新结果缓存
                {
                    std::lock_guard<std::mutex> lock(m_imageResultMutex);
                    for (auto &kv : currentResults)
                        m_imageMatchResults[kv.first] = kv.second;
                }

                // 状态变化通知
                for (const auto &kv : currentResults)
                {
                    int gIdx = kv.first;
                    bool matched = kv.second;
                    auto it = previousMatchState.find(gIdx);
                    bool prev = (it != previousMatchState.end()) ? it->second : false;
                    if (matched && !prev)
                    {
                        QMetaObject::invokeMethod(this, "onImageMatched", Qt::QueuedConnection,
                                                  Q_ARG(int, gIdx));
                    }
                    else if (!matched && prev)
                    {
                        QMetaObject::invokeMethod(this, "onImageLost", Qt::QueuedConnection,
                                                  Q_ARG(int, gIdx));
                    }
                }

                // 清除已不在检查列表中的组的状态
                for (auto it = previousMatchState.begin(); it != previousMatchState.end();)
                {
                    bool found = false;
                    for (const auto &item : itemsCopy)
                    {
                        if (item.groupIndex == it->first)
                        {
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                        it = previousMatchState.erase(it);
                    else
                        ++it;
                }
                previousMatchState = currentResults;
            }
            }
        }
        catch (const std::exception &e)
        {
            LOG_DEBUG("[IMG-MATCH] worker exception: " << e.what());
        }
        catch (...)
        {
            LOG_DEBUG("[IMG-MATCH] worker unknown exception");
        }

        // 固定间隔 60ms
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }

    LOG_DEBUG("Image match worker stopped");
}

void MacroEngine::startImageMatchForGroup(int groupIndex)
{
    std::lock_guard<std::mutex> lock(m_imageCheckMutex);
    if (groupIndex < 0 || groupIndex >= m_groups.size())
        return;

    const auto &grp = m_groups[groupIndex];
    if (grp.imagePaths.isEmpty())
        return;

    // 检查是否已存在
    for (const auto &item : m_imageCheckItems)
    {
        if (item.groupIndex == groupIndex)
            return; // 已在列表中
    }

    ImageCheckItem item;
    item.groupIndex = groupIndex;
    item.targetExe = grp.targetExe;
    item.imagePaths = grp.imagePaths;
    item.matchThreshold = grp.matchThreshold;
    item.imageTolerancePx = grp.imageTolerancePx;
    item.imageLearnData = grp.imageLearnData;
    m_imageCheckItems.push_back(item);
    m_pendingMatchStateReset.insert(groupIndex);

    m_imageCv.notify_one();
}

void MacroEngine::stopImageMatchForGroup(int groupIndex)
{
    std::lock_guard<std::mutex> lock(m_imageCheckMutex);
    m_pendingMatchStateReset.erase(groupIndex);
    for (auto it = m_imageCheckItems.begin(); it != m_imageCheckItems.end();)
    {
        if (it->groupIndex == groupIndex)
        {
            it = m_imageCheckItems.erase(it);
        }
        else
        {
            ++it;
        }
    }
    // 同时清除结果缓存，避免残留脏数据
    {
        std::lock_guard<std::mutex> rlock(m_imageResultMutex);
        m_imageMatchResults.erase(groupIndex);
    }
}

void MacroEngine::stopAllImageMatch()
{
    {
        std::lock_guard<std::mutex> lock(m_imageCheckMutex);
        m_imageCheckItems.clear();
    }
    m_imageCv.notify_one();
}

void MacroEngine::startImageMatchWorker()
{
    m_imageMatchStop.store(false);
    m_imageMatchWorker = std::thread(&MacroEngine::imageMatchWorkerFunc, this);
}

void MacroEngine::stopImageMatchWorker()
{
    m_imageMatchStop.store(true);
    m_imageCv.notify_one(); // 唤醒可能阻塞的识别线程
    if (m_imageMatchWorker.joinable())
    {
        m_imageMatchWorker.join();
    }
}

void MacroEngine::onImageMatched(int groupIndex)
{
    if (m_activationMode != ActivationMode::Group)
        return;

    if (!m_activeGroupIndices.contains(groupIndex) && m_pendingImageGroupIndices.contains(groupIndex))
    {
        LOG_DEBUG("Fully activating group " << groupIndex << " (event-driven)");
        m_pendingImageGroupIndices.remove(groupIndex);
        m_activeGroupIndices.insert(groupIndex);
        emit activeGroupActivated(groupIndex);
        emit dataChanged();
        rebuildHotkeyVkCodes();
        updateHooksState();

        // ========== 图像自动执行：移动鼠标到目标图像中央并自动执行组内宏 ==========
        if (groupIndex >= 0 && groupIndex < m_groups.size())
        {
            const auto &grp = m_groups[groupIndex];
            if (grp.imageAutoExecute && !grp.macroNames.isEmpty())
            {
                // 获取匹配图像的中心坐标（从 learnData 中提取）
                int imgCenterX = -1, imgCenterY = -1;
                for (const auto &ld : grp.imageLearnData)
                {
                    if (ld.centerX >= 0 && ld.centerY >= 0)
                    {
                        imgCenterX = ld.centerX;
                        imgCenterY = ld.centerY;
                        break;
                    }
                }

                if (imgCenterX >= 0 && imgCenterY >= 0)
                {
                    // 获取前台窗口屏幕坐标，转换为绝对屏幕坐标
                    HWND hwnd = GetForegroundWindow();
                    RECT wndRect;
                    int screenCenterX = imgCenterX;
                    int screenCenterY = imgCenterY;
                    if (hwnd && GetWindowRect(hwnd, &wndRect))
                    {
                        screenCenterX = wndRect.left + imgCenterX;
                        screenCenterY = wndRect.top + imgCenterY;
                    }
                    LOG_DEBUG("[AUTO-EXEC] 图像匹配成功，启动自动执行: 屏幕坐标("
                              << screenCenterX << "," << screenCenterY
                              << ") 组[" << groupIndex << "] \"" << grp.name << "\"");
                    startAutoExecute(groupIndex, screenCenterX, screenCenterY);
                }
                else
                {
                    LOG_DEBUG("[AUTO-EXEC] 图像匹配成功但无有效中心坐标，跳过自动执行");
                }
            }
        }

        // 检查是否有在激活前就按下的热键，并按优先级选择最佳匹配宏
        for (DWORD vk : m_pressedHotkeyVks)
        {
            QString key;
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
                key = keyToString(vk, true);
            else
                key = keyToString(vk);
            if (key.isEmpty())
                continue;
            const Macro *chosenMacro = nullptr;
            const Macro *globalCandidate = nullptr;
            int bestPrio = INT_MAX;
            for (const auto &m : m_macroInventory)
            {
                if (!m.enabled || m.steps.isEmpty() || m.triggerKey != key)
                    continue;
                bool inGroup = false;
                int groupPrio = INT_MAX;
                for (int gi = 0; gi < m_groups.size(); ++gi)
                    if (m_groups[gi].macroNames.contains(m.name) && isMacroActive(gi, 0))
                    {
                        inGroup = true;
                        if (m_groups[gi].priority < groupPrio)
                            groupPrio = m_groups[gi].priority;
                    }
                if (inGroup)
                {
                    if (groupPrio < bestPrio)
                    {
                        bestPrio = groupPrio;
                        chosenMacro = &m;
                    }
                }
                else if (m.globalActivation && !globalCandidate)
                {
                    globalCandidate = &m;
                }
            }
            if (!chosenMacro)
                chosenMacro = globalCandidate;
            if (!chosenMacro)
                continue;
            // 所有重复模式均支持继承触发（用户已在激活前按住热键）
            LOG_DEBUG("Inherited hotkey press: " << key << " -> " << chosenMacro->name
                                                 << " (mode=" << chosenMacro->repeatMode << ")");
            m_activeTriggeredByKey[key] = chosenMacro->name;
            triggerMacro(*chosenMacro);
            break;
        }
    }
}

void MacroEngine::onImageLost(int groupIndex)
{
    if (m_activationMode != ActivationMode::Group)
        return;
    // 组可能已在配置编辑中被删除/移动，后台线程的过期结果必须丢弃
    if (groupIndex < 0 || groupIndex >= m_groups.size())
        return;

    // 图像自动执行：只有失活的正是当前自动执行的来源组时才停止
    if (m_autoExecuting.load() && m_autoExecuteGroupIndex.load() == groupIndex)
    {
        LOG_DEBUG("[AUTO-EXEC] 目标图像消失，停止自动执行");
        stopAutoExecute();
    }

    if (m_activeGroupIndices.contains(groupIndex))
    {
        LOG_DEBUG("Group " << groupIndex << " lost image match, deactivating (event-driven)");
        m_activeGroupIndices.remove(groupIndex);
        // 不盲目清空 m_pressedHotkeyVks——物理按键状态不应随宏组失活而丢失
        const auto &grp = m_groups[groupIndex];
        // 检查失活组是否有宏正在运行（在 m_activeTriggeredByKey 中有条目）
        bool needStop = false;
        for (const auto &macroName : grp.macroNames)
        {
            for (auto it = m_activeTriggeredByKey.begin(); it != m_activeTriggeredByKey.end(); ++it)
            {
                if (it.value() == macroName)
                {
                    needStop = true;
                    break;
                }
            }
            if (needStop)
                break;
        }
        // 清理属于本组已触发宏的 m_activeTriggeredByKey 条目，释放热键供其他活跃组重新评估
        for (const auto &macroName : grp.macroNames)
        {
            for (auto it = m_activeTriggeredByKey.begin(); it != m_activeTriggeredByKey.end();)
            {
                if (it.value() == macroName)
                    it = m_activeTriggeredByKey.erase(it);
                else
                    ++it;
            }
        }
        m_suppressedMouseVkButtons.clear();
        m_suppressedKeyboardVkCodes.clear();
        // 仅当失活组确实有宏在运行时才全局停止，避免误杀其他组刚刚继承触发的宏
        if (needStop)
            requestStopMacroOnMainThread();
        forceShowCursor();
        emit activeGroupDeactivated(groupIndex);
        emit dataChanged();
        rebuildHotkeyVkCodes();
        updateHooksState();

        // 失活后尝试继承触发：检查仍在按下的热键是否能匹配到其他活跃组中的宏
        for (DWORD vk : m_pressedHotkeyVks)
        {
            QString key;
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
                key = keyToString(vk, true);
            else
                key = keyToString(vk);
            if (key.isEmpty())
                continue;
            // 跳过已被触发的热键（仍在 m_activeTriggeredByKey 中说明已有活跃组宏在运行）
            if (m_activeTriggeredByKey.contains(key))
                continue;
            const Macro *chosenMacro = nullptr;
            const Macro *globalCandidate = nullptr;
            int bestPrio = INT_MAX;
            for (const auto &m : m_macroInventory)
            {
                if (!m.enabled || m.steps.isEmpty() || m.triggerKey != key)
                    continue;
                bool inGroup = false;
                int groupPrio = INT_MAX;
                for (int gi = 0; gi < m_groups.size(); ++gi)
                {
                    if (m_groups[gi].macroNames.contains(m.name) && isMacroActive(gi, 0))
                    {
                        inGroup = true;
                        if (m_groups[gi].priority < groupPrio)
                            groupPrio = m_groups[gi].priority;
                    }
                }
                if (inGroup)
                {
                    if (groupPrio < bestPrio)
                    {
                        bestPrio = groupPrio;
                        chosenMacro = &m;
                    }
                }
                else if (m.globalActivation && !globalCandidate)
                {
                    globalCandidate = &m;
                }
            }
            if (!chosenMacro)
                chosenMacro = globalCandidate;
            if (!chosenMacro)
                continue;
            LOG_DEBUG("Inherited hotkey press after group deactivation: " << key << " -> " << chosenMacro->name);
            m_activeTriggeredByKey[key] = chosenMacro->name;
            triggerMacro(*chosenMacro);
            break;
        }
    }
}

// 自学习回调：后台线程匹配成功后通知主线程持久化
void MacroEngine::onImageLearned(int groupIndex, int imageIndex, int cx, int cy, int tw, int th)
{
    if (groupIndex < 0 || groupIndex >= m_groups.size())
        return;
    auto &grp = m_groups[groupIndex];
    // 后台线程持有的图像索引可能因配置编辑而过期，越界则丢弃
    if (imageIndex < 0 || imageIndex >= grp.imagePaths.size())
        return;
    while (grp.imageLearnData.size() <= imageIndex)
        grp.imageLearnData.append(ImageLearnData());
    auto &ld = grp.imageLearnData[imageIndex];
    ld.centerX = cx;
    ld.centerY = cy;
    ld.tw = tw;
    ld.th = th;
    LOG_DEBUG("[IMG-LEARN] 持久化学习数据: 组[" << groupIndex << "] 图像[" << imageIndex
                                                << "] 位置=(" << cx << "," << cy << ") 尺寸=" << tw << "x" << th);
    // 自动保存
    QString filePath = QCoreApplication::applicationDirPath() + "/macros.ini";
    saveToFile(filePath);
}

// ==================== 世代机制静态成员 ====================
std::atomic<uint64_t> MacroEngine::s_macroGeneration{0};
std::atomic<bool> MacroEngine::s_isHotkeyPressed{false};
std::atomic<bool> MacroEngine::s_stopRequested{false};
std::atomic<int> MacroEngine::s_activeMacroThreads{0};
std::atomic<bool> MacroEngine::s_disableInput{false};

// ==================== 单例与构造 ====================
MacroEngine *MacroEngine::s_instance = nullptr;

MacroEngine *MacroEngine::instance()
{
    if (!s_instance)
        s_instance = new MacroEngine(qApp);
    return s_instance;
}

MacroEngine::MacroEngine(QObject *parent)
    : QObject(parent), m_mainThreadId(GetCurrentThreadId())
{
    LOG_DEBUG("Constructor - MainThread: " << m_mainThreadId);
    timeBeginPeriod(1);
    QCoreApplication::instance()->installNativeEventFilter(this);

    // 启动前台窗口轮询（每 50ms 检查一次，与图像匹配线程同步）
    m_foregroundTimer = new QTimer(this);
    m_foregroundTimer->setInterval(50);
    connect(m_foregroundTimer, &QTimer::timeout, this, &MacroEngine::checkForegroundWindow);
    m_foregroundTimer->start();

    QString filePath = QCoreApplication::applicationDirPath() + "/macros.ini";
    LOG_DEBUG("Config path: " << filePath);
    if (loadFromFile(filePath))
    {
        LOG_DEBUG("Load succeeded");
        rebuildHotkeyVkCodes();
    }
    else
    {
        // 配置文件存在但加载失败（损坏/版本不支持）：记录状态，UI 不应再用演示数据覆盖
        m_configLoadFailed = QFile::exists(filePath);
        // 兼容旧版：尝试加载 macros.dat 并自动迁移
        QString oldPath = QCoreApplication::applicationDirPath() + "/macros.dat";
        if (QFile::exists(oldPath))
        {
            LOG_DEBUG("Old macros.dat found, attempting migration");
            if (loadFromFile(oldPath))
            {
                LOG_DEBUG("Migration load succeeded, saving to new path");
                saveToFile(filePath);
                rebuildHotkeyVkCodes();
                m_configLoadFailed = false;
            }
            else
                LOG_DEBUG("Migration load failed");
        }
        else
            LOG_DEBUG("No existing config file found");
    }

    // 启动后台图像匹配线程
    LOG_DEBUG("Starting image match worker...");
    startImageMatchWorker();
    stopAllImageMatch(); // 初始时全局模式，不允许识别
    updateHooksState();
}

MacroEngine::~MacroEngine()
{
    LOG_DEBUG("Destructor");
    // 先停止所有宏播放线程并等待退出，避免对象析构后线程仍对已销毁对象投递事件（UAF）
    s_stopRequested.store(true);
    s_isHotkeyPressed.store(false);
    s_disableInput.store(true);
    for (int i = 0; i < 300 && s_activeMacroThreads.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stopAutoExecute();
    stopImageMatchWorker();
    uninstallHooks();
    s_instance = nullptr;
    timeEndPeriod(1);
}

// ==================== 钩子 ====================
void MacroEngine::installHooks()
{
    LOG_DEBUG("Installing hooks");
    if (!m_keyboardHook)
        m_keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, keyboardHookProc, GetModuleHandle(nullptr), 0);
    if (!m_keyboardHook)
        LOG_DEBUG("Keyboard hook install FAILED, GetLastError=" << GetLastError());
    if (!m_mouseHook)
        m_mouseHook = SetWindowsHookEx(WH_MOUSE_LL, mouseHookProc, GetModuleHandle(nullptr), 0);
    if (!m_mouseHook)
        LOG_DEBUG("Mouse hook install FAILED, GetLastError=" << GetLastError());
    LOG_DEBUG("Keyboard: " << (void *)m_keyboardHook << ", Mouse: " << (void *)m_mouseHook);
}

void MacroEngine::updateHooksState()
{
    bool hasGlobalMacro = false;
    for (const auto &m : m_macroInventory)
    {
        if (m.globalActivation && m.enabled)
        {
            hasGlobalMacro = true;
            break;
        }
    }
    bool needHook = !m_activeGroupIndices.isEmpty() ||
                    !m_pendingImageGroupIndices.isEmpty() ||
                    m_bindRecording ||
                    m_recording ||
                    hasGlobalMacro;
    if (needHook && !m_hookInstalled)
    {
        LOG_DEBUG("Installing hooks (on demand)");
        installHooks();
        // 只有键盘与鼠标钩子都成功才认为已安装；失败的钩子会在下次 updateHooksState 重试
        m_hookInstalled = (m_keyboardHook != nullptr && m_mouseHook != nullptr);
        if (!m_hookInstalled)
            LOG_DEBUG("Hook install incomplete, will retry on next state update");
    }
    else if (!needHook && m_hookInstalled)
    {
        LOG_DEBUG("Uninstalling hooks (no longer needed)");
        uninstallHooks();
        m_hookInstalled = false;
    }
}

void MacroEngine::uninstallHooks()
{
    if (m_keyboardHook)
        UnhookWindowsHookEx(m_keyboardHook);
    if (m_mouseHook)
        UnhookWindowsHookEx(m_mouseHook);
    m_keyboardHook = nullptr;
    m_mouseHook = nullptr;
    m_hookInstalled = false;
    LOG_DEBUG("Hooks uninstalled");
}

// 钩子回调：拷贝结构体数值，通过 invokeMethod 传递值类型参数
LRESULT CALLBACK MacroEngine::keyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && s_instance)
    {
        KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT *)lParam;
        bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        if (isDown || isUp)
        {
            // 注入事件（本程序宏/自动执行 SendInput 产生）不参与热键触发、录制与拦截，
            // 否则宏会触发自己、把模拟输入录进新宏，或吞掉自己发出的按键。
            if (kb->flags & LLKHF_INJECTED)
                return CallNextHookEx(nullptr, nCode, wParam, lParam);

            QMetaObject::invokeMethod(s_instance, "processKeyEvent", Qt::QueuedConnection,
                                      Q_ARG(WPARAM, wParam),
                                      Q_ARG(DWORD, kb->vkCode),
                                      Q_ARG(DWORD, kb->scanCode),
                                      Q_ARG(DWORD, kb->flags),
                                      Q_ARG(DWORD, kb->time));

            bool shouldSuppress = false;
            {
                std::lock_guard<std::mutex> lock(s_instance->m_hotkeyVkMutex);
                if (s_instance->m_hotkeyVkCodes.count(kb->vkCode))
                {
                    if (isDown)
                    {
                        s_instance->m_suppressedKeyboardVkCodes.insert(kb->vkCode);
                        shouldSuppress = true;
                    }
                    else if (isUp)
                    {
                        if (s_instance->m_suppressedKeyboardVkCodes.erase(kb->vkCode) > 0)
                            shouldSuppress = true;
                    }
                }
            }
            if (!shouldSuppress && s_instance->m_blockActionKeys.load())
            {
                std::lock_guard<std::mutex> lock(s_instance->m_blockedActionKeyMutex);
                if (!(kb->flags & LLKHF_INJECTED) && s_instance->m_blockedActionKeyVkCodes.count(kb->vkCode))
                    shouldSuppress = true;
            }
            if (shouldSuppress)
                return 1;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

LRESULT CALLBACK MacroEngine::mouseHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && s_instance)
    {
        MSLLHOOKSTRUCT *ms = (MSLLHOOKSTRUCT *)lParam;

        // 注入事件（本程序宏/自动执行 SendInput 产生）直接放行，不触发热键、不录制、不拦截
        if (ms->flags & LLMHF_INJECTED)
            return CallNextHookEx(nullptr, nCode, wParam, lParam);

        // 统一判断是否为鼠标按键事件，并提取对应的 VK 码
        bool isDown = false;
        bool isUp = false;
        DWORD vkButton = 0;

        switch (wParam)
        {
        case WM_LBUTTONDOWN:
            isDown = true;
            vkButton = VK_LBUTTON;
            break;
        case WM_LBUTTONUP:
            isUp = true;
            vkButton = VK_LBUTTON;
            break;
        case WM_RBUTTONDOWN:
            isDown = true;
            vkButton = VK_RBUTTON;
            break;
        case WM_RBUTTONUP:
            isUp = true;
            vkButton = VK_RBUTTON;
            break;
        case WM_MBUTTONDOWN:
            isDown = true;
            vkButton = VK_MBUTTON;
            break;
        case WM_MBUTTONUP:
            isUp = true;
            vkButton = VK_MBUTTON;
            break;
        case WM_XBUTTONDOWN:
            isDown = true;
            vkButton = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            break;
        case WM_XBUTTONUP:
            isUp = true;
            vkButton = (HIWORD(ms->mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
            break;
        default:
            break;
        }

        bool shouldSuppress = false;
        if (isDown || isUp)
        {
            std::lock_guard<std::mutex> lock(s_instance->m_hotkeyVkMutex);
            if (s_instance->m_activeMouseVkCodes.count(vkButton))
            {
                if (isDown)
                {
                    s_instance->m_suppressedMouseVkButtons.insert(vkButton);
                    shouldSuppress = true;
                }
                else if (isUp)
                {
                    if (s_instance->m_suppressedMouseVkButtons.erase(vkButton) > 0)
                        shouldSuppress = true;
                }
            }
        }
        if (!shouldSuppress && (isDown || isUp) && s_instance->m_blockActionKeys.load())
        {
            std::lock_guard<std::mutex> lock(s_instance->m_blockedActionKeyMutex);
            if (!(ms->flags & LLMHF_INJECTED) && s_instance->m_blockedActionKeyVkCodes.count(vkButton))
                shouldSuppress = true;
        }

        // 对所有鼠标按键事件（含滚轮）分发到 processMouseEvent
        if (wParam == WM_LBUTTONDOWN || wParam == WM_LBUTTONUP ||
            wParam == WM_RBUTTONDOWN || wParam == WM_RBUTTONUP ||
            wParam == WM_MBUTTONDOWN || wParam == WM_MBUTTONUP ||
            wParam == WM_XBUTTONDOWN || wParam == WM_XBUTTONUP ||
            wParam == WM_MOUSEWHEEL)
        {
            QMetaObject::invokeMethod(s_instance, "processMouseEvent", Qt::QueuedConnection,
                                      Q_ARG(WPARAM, wParam),
                                      Q_ARG(LONG, ms->pt.x),
                                      Q_ARG(LONG, ms->pt.y),
                                      Q_ARG(DWORD, ms->mouseData),
                                      Q_ARG(DWORD, ms->flags),
                                      Q_ARG(DWORD, ms->time));
            if (shouldSuppress)
                return 1;
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool MacroEngine::nativeEventFilter(const QByteArray &eventType, void *message, qintptr * /*result*/)
{
    return false;
}

// ==================== 热键管理 ====================

DWORD mouseKeyNameToVk(const QString &key)
{
    if (key == "左键")
        return VK_LBUTTON;
    if (key == "右键")
        return VK_RBUTTON;
    if (key == "中键")
        return VK_MBUTTON;
    if (key == "X1键")
        return VK_XBUTTON1;
    if (key == "X2键")
        return VK_XBUTTON2;
    return 0;
}

bool MacroEngine::isMacroActiveByName(const QString &macroName) const
{
    if (m_activationMode == ActivationMode::Global)
    {
        for (int gi = 0; gi < m_groups.size(); ++gi)
        {
            if (m_groups[gi].macroNames.contains(macroName))
                return true;
        }
        return false;
    }
    for (int idx : m_activeGroupIndices)
    {
        if (idx >= 0 && idx < m_groups.size() && m_groups[idx].macroNames.contains(macroName))
            return true;
    }
    return false;
}

void MacroEngine::rebuildHotkeyVkCodes()
{
    std::lock_guard<std::mutex> lock(m_hotkeyVkMutex);
    m_hotkeyVkCodes.clear();
    m_activeMouseVkCodes.clear();

    QStringList activeGroupKeys;
    auto addVk = [&](const QString &key)
    {
        DWORD mouseVk = mouseKeyNameToVk(key);
        if (mouseVk != 0)
        {
            m_activeMouseVkCodes.insert(mouseVk);
            return;
        }
        DWORD vk = keyNameToVkCode(key);
        if (vk == 0)
        {
            // 尝试从 "VK_0xXX" 格式解析 VK 码（支持多媒体及所有冷门键码）
            if (key.startsWith("VK_0x", Qt::CaseInsensitive))
            {
                bool ok = false;
                vk = key.mid(5).toUInt(&ok, 16);
                if (!ok)
                    vk = 0;
            }
        }
        if (vk != 0)
        {
            m_hotkeyVkCodes.insert(vk);
            // 低层键盘钩子的 vkCode 是左右修饰键变体，而热键文本无法区分左右，
            // 因此把左右变体一并加入抑制集合
            switch (vk)
            {
            case VK_CONTROL:
            case VK_LCONTROL:
                m_hotkeyVkCodes.insert(VK_LCONTROL);
                m_hotkeyVkCodes.insert(VK_RCONTROL);
                break;
            case VK_SHIFT:
            case VK_LSHIFT:
                m_hotkeyVkCodes.insert(VK_LSHIFT);
                m_hotkeyVkCodes.insert(VK_RSHIFT);
                break;
            case VK_MENU:
            case VK_LMENU:
                m_hotkeyVkCodes.insert(VK_LMENU);
                m_hotkeyVkCodes.insert(VK_RMENU);
                break;
            case VK_LWIN:
                m_hotkeyVkCodes.insert(VK_RWIN);
                break;
            case VK_RWIN:
                m_hotkeyVkCodes.insert(VK_LWIN);
                break;
            default:
                break;
            }
        }
        else
            LOG_DEBUG("Warning: Could not find VK code for hotkey: " << key);
    };

    // 收集所有活跃组和挂起组中的宏热键（挂起组的热键也需要加入 m_hotkeyVkCodes，
    // 以便钩子能拦截这些按键并在图像匹配成功后继承触发）
    for (const auto &macro : m_macroInventory)
    {
        if (!macro.enabled || macro.triggerKey.isEmpty() || macro.steps.isEmpty())
            continue;
        bool inActiveOrPendingGroup = false;
        for (int gi = 0; gi < m_groups.size(); ++gi)
        {
            if (m_groups[gi].macroNames.contains(macro.name) &&
                (m_activeGroupIndices.contains(gi) || m_pendingImageGroupIndices.contains(gi)))
            {
                inActiveOrPendingGroup = true;
                break;
            }
        }
        if (inActiveOrPendingGroup)
        {
            activeGroupKeys.append(macro.triggerKey);
            addVk(macro.triggerKey);
        }
    }

    for (const auto &macro : m_macroInventory)
    {
        if (!macro.enabled || macro.triggerKey.isEmpty() || !macro.globalActivation || macro.steps.isEmpty())
            continue;
        if (activeGroupKeys.contains(macro.triggerKey))
            continue;
        addVk(macro.triggerKey);
    }

    LOG_DEBUG("Rebuilt hotkey VK codes: keyboard=" << m_hotkeyVkCodes.size()
                                                   << ", mouse=" << m_activeMouseVkCodes.size() << " entries");
}

// ==================== 事件处理 ====================
void MacroEngine::processKeyEvent(WPARAM wParam, DWORD vkCode, DWORD scanCode, DWORD flags, DWORD time)
{
    Q_UNUSED(scanCode);
    Q_UNUSED(flags);
    Q_UNUSED(time);
    bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    bool isUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
    if (!isDown && !isUp)
        return;

    // 维护物理按下集合，供组合键（triggerKey 形如 "Ctrl+A"）匹配使用
    if (isDown)
        m_physicalPressedVks.insert(vkCode);
    else
        m_physicalPressedVks.remove(vkCode);

    // // 空格键日志（用于测试图像出现到宏组激活的延迟）
    // if (isDown && vkCode == VK_SPACE)
    // {
    //     LOG_DEBUG("SPACE key pressed ────────────────────────────");
    // }

    if (m_bindRecording || m_recording)
    {
        LOG_DEBUG("Key event - vk: 0x" << QString::number(vkCode, 16).toUpper().rightJustified(2, '0')
                                       << ", down: " << isDown << ", bind_rec: " << m_bindRecording << ", rec: " << m_recording);
    }

    if (m_bindRecording && isDown)
    {
        // Esc 取消录制（仅在尚未记录任何键时）
        if (vkCode == VK_ESCAPE && m_bindPressedVKs.isEmpty())
        {
            cancelBindRecording();
            return;
        }
        // 过滤重复按下（系统自动重复消息）：同一 VK 已记录则忽略
        if (m_bindPressedVKs.contains(vkCode))
            return;
        // 记录
        QString key = keyToString(vkCode);
        m_bindPressedKeyNames.append(key);
        m_bindPressedVKs.insert(vkCode);
        return;
    }
    if (m_bindRecording && isUp)
    {
        m_bindPressedVKs.remove(vkCode);
        if (m_bindPressedVKs.isEmpty() && !m_bindPressedKeyNames.isEmpty())
        {
            finishBindRecording();
        }
        return;
    }

    if (isDown && !m_bindRecording && !m_recording)
    {
        QString key = keyToString(vkCode);
        // 按住热键时系统会持续发送重复的 KeyDown 消息，
        // 若该热键已触发过且尚未释放，则忽略后续重复消息，防止宏被反复重启
        if (m_activeTriggeredByKey.contains(key))
        {
            return;
        }
        struct Candidate
        {
            const Macro *macro;
            int priority;
        };
        QVector<Candidate> candidates;
        const Macro *globalCandidate = nullptr;
        for (const auto &m : m_macroInventory)
        {
            if (!m.enabled || m.steps.isEmpty() || m.triggerKey != key)
                continue;
            bool inActiveGroup = false;
            int bestPriority = INT_MAX;
            for (int gi = 0; gi < m_groups.size(); ++gi)
            {
                if (m_groups[gi].macroNames.contains(m.name) && isMacroActive(gi, 0))
                {
                    inActiveGroup = true;
                    if (m_groups[gi].priority < bestPriority)
                        bestPriority = m_groups[gi].priority;
                }
            }
            if (inActiveGroup)
            {
                candidates.push_back({&m, bestPriority});
            }
            else if (m.globalActivation && !globalCandidate)
            {
                globalCandidate = &m;
            }
        }
        // 按优先级排序（数字越小优先级越高）
        std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b)
                  { return a.priority < b.priority; });
        const Macro *chosenMacro = nullptr;
        bool chosenIsGlobal = false;
        if (!candidates.isEmpty())
        {
            chosenMacro = candidates.first().macro;
            chosenIsGlobal = false;
        }
        else if (globalCandidate)
        {
            chosenMacro = globalCandidate;
            chosenIsGlobal = true;
        }
        if (chosenMacro)
        {
            m_pressedHotkeyVks.insert(vkCode);
            m_activeTriggeredByKey[key] = chosenMacro->name;
            LOG_DEBUG("Keyboard hotkey triggered: " << key << " -> " << chosenMacro->name << (chosenIsGlobal ? " (global)" : " (group)"));
            LOG_DEBUG("Macro '" << chosenMacro->name << "' has " << chosenMacro->steps.size() << " steps");
            for (int i = 0; i < qMin(5, chosenMacro->steps.size()); ++i)
            {
                if (chosenMacro->steps[i].type == StepType::Delay)
                    LOG_DEBUG("  step[" << i << "]: Delay = " << chosenMacro->steps[i].delaySec << " sec");
            }
            triggerMacro(*chosenMacro);
            return;
        }

        // ===== 组合键热键（triggerKey 含 '+'，如 "Ctrl+A"）=====
        // 单键匹配失败时，检查所有成员键当前是否都已按下；全部按下才触发。
        const Macro *comboCandidate = nullptr;
        int comboPriority = INT_MAX;
        for (const auto &m : m_macroInventory)
        {
            if (!m.enabled || m.steps.isEmpty() || !m.triggerKey.contains('+'))
                continue;
            if (m_activeTriggeredByKey.contains(m.triggerKey))
                continue;
            const QStringList parts = m.triggerKey.split('+', Qt::SkipEmptyParts);
            if (parts.isEmpty())
                continue;
            bool allPressed = true;
            for (const QString &part : parts)
            {
                DWORD partVk = keyNameToVkCode(part.trimmed());
                if (partVk == 0 || !m_physicalPressedVks.contains(partVk))
                {
                    allPressed = false;
                    break;
                }
            }
            if (!allPressed)
                continue;
            bool inActiveGroup = false;
            int bestPriority = INT_MAX;
            for (int gi = 0; gi < m_groups.size(); ++gi)
            {
                if (m_groups[gi].macroNames.contains(m.name) && isMacroActive(gi, 0))
                {
                    inActiveGroup = true;
                    if (m_groups[gi].priority < bestPriority)
                        bestPriority = m_groups[gi].priority;
                }
            }
            if (inActiveGroup)
            {
                if (bestPriority < comboPriority)
                {
                    comboPriority = bestPriority;
                    comboCandidate = &m;
                }
            }
            else if (m.globalActivation && !comboCandidate)
            {
                comboCandidate = &m;
            }
        }
        if (comboCandidate)
        {
            m_activeTriggeredByKey[comboCandidate->triggerKey] = comboCandidate->name;
            LOG_DEBUG("Combo hotkey triggered: " << comboCandidate->triggerKey << " -> " << comboCandidate->name);
            triggerMacro(*comboCandidate);
            return;
        }

        // 当前无活跃组可触发，但该键可能是挂起组中的热键（等待图像匹配激活）。
        // 记录到 m_pressedHotkeyVks，待 onImageMatched 激活组时继承触发。
        {
            std::lock_guard<std::mutex> lk(m_hotkeyVkMutex);
            if (m_hotkeyVkCodes.count(vkCode))
                m_pressedHotkeyVks.insert(vkCode);
        }
    }

    if (isUp && !m_recording)
    {
        QString key = keyToString(vkCode);
        m_pressedHotkeyVks.erase(vkCode);
        QString triggeredMacroName = m_activeTriggeredByKey.value(key);
        if (!triggeredMacroName.isEmpty())
        {
            m_activeTriggeredByKey.remove(key);
            const Macro *macroPtr = findMacro(triggeredMacroName);
            if (macroPtr && (macroPtr->repeatMode == 1 || macroPtr->repeatMode == 2))
            {
                LOG_DEBUG("Stopping repeat mode due to key release: " << key);
                s_isHotkeyPressed.store(false);
                return;
            }
        }

        // 组合键：任一成员键松开即清除触发状态，并在需要时停止按住重复
        for (auto it = m_activeTriggeredByKey.begin(); it != m_activeTriggeredByKey.end();)
        {
            if (!it.key().contains('+'))
            {
                ++it;
                continue;
            }
            const QStringList parts = it.key().split('+', Qt::SkipEmptyParts);
            bool containsReleased = false;
            for (const QString &part : parts)
            {
                if (keyNameToVkCode(part.trimmed()) == vkCode)
                {
                    containsReleased = true;
                    break;
                }
            }
            if (!containsReleased)
            {
                ++it;
                continue;
            }
            const Macro *macroPtr = findMacro(it.value());
            it = m_activeTriggeredByKey.erase(it);
            if (macroPtr && (macroPtr->repeatMode == 1 || macroPtr->repeatMode == 2))
            {
                LOG_DEBUG("Stopping combo repeat mode due to key release");
                s_isHotkeyPressed.store(false);
            }
        }
    }

    if (m_recording)
    {
        MacroStep step;
        step.keyOrButton = vkCode;
        step.type = isDown ? StepType::KeyPress : StepType::KeyRelease;
        m_recordedSteps.append(step);
        m_stepTimestamps.append(m_recordTimer.elapsed());
        emit stepRecorded(step);
    }
}

void MacroEngine::processMouseEvent(WPARAM wParam, LONG ptX, LONG ptY, DWORD mouseData, DWORD flags, DWORD time)
{
    Q_UNUSED(flags);
    Q_UNUSED(time);
    bool isDown = false;
    WORD btn = 0;
    bool isUp = false;
    bool isWheel = false;
    short wheelDelta = 0;

    switch (wParam)
    {
    case WM_LBUTTONDOWN:
        isDown = true;
        btn = VK_LBUTTON;
        break;
    case WM_LBUTTONUP:
        isUp = true;
        btn = VK_LBUTTON;
        break;
    case WM_RBUTTONDOWN:
        isDown = true;
        btn = VK_RBUTTON;
        break;
    case WM_RBUTTONUP:
        isUp = true;
        btn = VK_RBUTTON;
        break;
    case WM_MBUTTONDOWN:
        isDown = true;
        btn = VK_MBUTTON;
        break;
    case WM_MBUTTONUP:
        isUp = true;
        btn = VK_MBUTTON;
        break;
    case WM_XBUTTONDOWN:
        isDown = true;
        btn = (HIWORD(mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    case WM_XBUTTONUP:
        isUp = true;
        btn = (HIWORD(mouseData) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2;
        break;
    case WM_MOUSEWHEEL:
        isWheel = true;
        wheelDelta = GET_WHEEL_DELTA_WPARAM(mouseData);
        break;
    default:
        return;
    }

    if (m_bindRecording || m_recording)
    {
        LOG_DEBUG("Mouse event - msg:0x" << QString::number(wParam, 16).toUpper()
                                         << ", btn:0x" << QString::number(btn, 16).toUpper()
                                         << ", down:" << isDown << ", up:" << isUp);
    }

    if (m_bindRecording)
    {
        if (isWheel)
        {
            // 滚轮直接绑定
            QString key = (wheelDelta > 0) ? "WheelUp" : "WheelDown";
            m_bindPressedKeyNames.clear();
            m_bindPressedVKs.clear();
            m_bindPressedKeyNames.append(key);
            finishBindRecording();
            return;
        }
        if (isDown)
        {
            QString key = mouseButtonToString(btn);
            // 避免重复记录（已经按着的鼠标按键不再添加）
            if (!m_bindPressedVKs.contains(btn))
            {
                m_bindPressedKeyNames.append(key);
                m_bindPressedVKs.insert(btn);
            }
            return;
        }
        if (isUp)
        {
            m_bindPressedVKs.remove(btn);
            if (m_bindPressedVKs.isEmpty() && !m_bindPressedKeyNames.isEmpty())
            {
                finishBindRecording();
            }
            return;
        }
    }

    if (isDown && !m_bindRecording && !m_recording)
    {
        QString key = keyToString(btn, true);
        // 按住鼠标热键时系统可能持续发送重复的按下消息，
        // 若该热键已触发过且尚未释放，则忽略后续重复消息，防止宏被反复重启
        if (m_activeTriggeredByKey.contains(key))
        {
            return;
        }
        struct Candidate
        {
            const Macro *macro;
            int priority;
        };
        QVector<Candidate> candidates;
        const Macro *globalCandidate = nullptr;
        for (const auto &m : m_macroInventory)
        {
            if (!m.enabled || m.steps.isEmpty() || m.triggerKey != key)
                continue;
            bool inActiveGroup = false;
            int bestPriority = INT_MAX;
            for (int gi = 0; gi < m_groups.size(); ++gi)
            {
                if (m_groups[gi].macroNames.contains(m.name) && isMacroActive(gi, 0))
                {
                    inActiveGroup = true;
                    if (m_groups[gi].priority < bestPriority)
                        bestPriority = m_groups[gi].priority;
                }
            }
            if (inActiveGroup)
            {
                candidates.push_back({&m, bestPriority});
            }
            else if (m.globalActivation && !globalCandidate)
            {
                globalCandidate = &m;
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b)
                  { return a.priority < b.priority; });
        const Macro *chosenMacro = nullptr;
        bool chosenIsGlobal = false;
        if (!candidates.isEmpty())
        {
            chosenMacro = candidates.first().macro;
            chosenIsGlobal = false;
        }
        else if (globalCandidate)
        {
            chosenMacro = globalCandidate;
            chosenIsGlobal = true;
        }
        if (chosenMacro)
        {
            m_pressedHotkeyVks.insert(static_cast<DWORD>(btn));
            m_activeTriggeredByKey[key] = chosenMacro->name;
            LOG_DEBUG("Mouse hotkey triggered: " << key << " -> " << chosenMacro->name << (chosenIsGlobal ? " (global)" : " (group)"));
            triggerMacro(*chosenMacro);
            return;
        }
        // 当前无活跃组可触发，但该键可能是挂起组中的热键（等待图像匹配激活）。
        {
            std::lock_guard<std::mutex> lk(m_hotkeyVkMutex);
            if (m_activeMouseVkCodes.count(static_cast<DWORD>(btn)))
                m_pressedHotkeyVks.insert(static_cast<DWORD>(btn));
        }
    }

    if (isUp && !m_bindRecording && !m_recording)
    {
        QString key = keyToString(btn, true);
        m_pressedHotkeyVks.erase(static_cast<DWORD>(btn));
        QString triggeredMacroName = m_activeTriggeredByKey.value(key);
        if (!triggeredMacroName.isEmpty())
        {
            m_activeTriggeredByKey.remove(key);
            const Macro *macroPtr = findMacro(triggeredMacroName);
            if (macroPtr && (macroPtr->repeatMode == 1 || macroPtr->repeatMode == 2))
            {
                LOG_DEBUG("Stopping repeat mode due to mouse release: " << key);
                s_isHotkeyPressed.store(false);
                return;
            }
        }
    }

    if (m_recording)
    {
        if (!m_recordExcludeRegion.isNull() && m_recordExcludeRegion.contains(ptX, ptY))
        {
            LOG_DEBUG("Mouse event in exclude region, skipping record");
            return;
        }

        MacroStep step;
        if (isWheel)
        {
            step.type = StepType::MouseScroll;
            step.scrollDelta = wheelDelta;
        }
        else
        {
            step.type = isDown ? StepType::MousePress : StepType::MouseRelease;
            step.keyOrButton = btn;
        }
        m_recordedSteps.append(step);
        m_stepTimestamps.append(m_recordTimer.elapsed());
        emit stepRecorded(step);
    }
}

// ==================== 录制控制 ====================
void MacroEngine::startRecording()
{
    LOG_DEBUG("Recording started");
    m_recording = true;
    m_recordedSteps.clear();  // 必须清除临时步骤，便于本次录制
    m_stepTimestamps.clear(); // 必须清除时间戳，重新计时
    m_recordTimer.start();
    emit logMessage("录制开始");
    emit recordingStarted();
    updateHooksState();
}

void MacroEngine::stopRecording()
{
    if (!m_recording)
        return;
    LOG_DEBUG("Recording stopped, raw steps: " << m_recordedSteps.size());
    m_recording = false;

    // 从当前宏已有的步骤开始（不清空）
    QVector<MacroStep> finalSteps = m_currentMacro.steps;

    // 计算已有步骤的总延迟（毫秒）
    qint64 baseTimestamp = 0;
    for (const auto &step : finalSteps)
    {
        if (step.type == StepType::Delay)
            baseTimestamp += static_cast<qint64>(step.delaySec * 1000.0);
    }

    // 追加新录制步骤
    if (!m_recordedSteps.isEmpty())
    {
        finalSteps.append(m_recordedSteps[0]);
        for (int i = 1; i < m_recordedSteps.size(); ++i)
        {
            qint64 deltaMs = m_stepTimestamps[i] - m_stepTimestamps[i - 1];
            if (deltaMs > 0)
            {
                MacroStep delay;
                delay.type = StepType::Delay;
                delay.delaySec = deltaMs / 1000.0;
                finalSteps.append(delay);
            }
            finalSteps.append(m_recordedSteps[i]);
        }
        qint64 finalDeltaMs = m_recordTimer.elapsed() - m_stepTimestamps.last();
        if (finalDeltaMs > 0)
        {
            MacroStep delay;
            delay.type = StepType::Delay;
            delay.delaySec = finalDeltaMs / 1000.0;
            finalSteps.append(delay);
        }
    }

    m_currentMacro.steps = finalSteps;
    for (auto &m : m_macroInventory)
        if (m.name == m_currentMacro.name)
        {
            m = m_currentMacro;
            break;
        }
    emit logMessage(QString("录制结束，共 %1 步").arg(finalSteps.size()));
    emit recordingStopped();
    emit dataChanged();
    updateHooksState();
}

bool MacroEngine::isRecording() const { return m_recording; }

void MacroEngine::startBindRecording()
{
    m_bindRecording = true;
    m_bindPressedKeyNames.clear();
    m_bindPressedVKs.clear();
    emit logMessage("请按下组合键，松开所有键后完成绑定...");
    emit bindRecordingStarted();
    updateHooksState();
}

void MacroEngine::stopBindRecording()
{
    m_bindRecording = false;
    m_bindPressedKeyNames.clear();
    m_bindPressedVKs.clear();
    updateHooksState();
}

bool MacroEngine::isBindRecording() const { return m_bindRecording; }

void MacroEngine::finishBindRecording()
{
    if (m_bindPressedKeyNames.isEmpty())
        return;

    QString key = m_bindPressedKeyNames.join("+");

    // 清除该宏的旧绑定
    QStringList keysToRemove;
    for (auto it = m_triggerBindings.begin(); it != m_triggerBindings.end(); ++it)
    {
        if (it.value() == m_currentMacro.name)
            keysToRemove.append(it.key());
    }
    for (const QString &k : keysToRemove)
        m_triggerBindings.remove(k);

    m_currentMacro.triggerKey = key;
    m_triggerBindings[key] = m_currentMacro.name;

    for (auto &m : m_macroInventory)
        if (m.name == m_currentMacro.name)
        {
            m.triggerKey = key;
            break;
        }

    m_bindRecording = false;
    m_bindPressedKeyNames.clear();
    m_bindPressedVKs.clear();
    updateHooksState();
    rebuildHotkeyVkCodes();
    emit triggerKeySet(key);
    emit dataChanged();
}

void MacroEngine::cancelBindRecording()
{
    m_bindRecording = false;
    m_bindPressedKeyNames.clear();
    m_bindPressedVKs.clear();
    updateHooksState();
    emit logMessage("绑定已取消");
    emit dataChanged();
}

void MacroEngine::setRecordExcludeRegion(const QRect &globalRect)
{
    m_recordExcludeRegion = globalRect;
    LOG_DEBUG("Record exclude region set: " << globalRect);
}

void MacroEngine::clearRecordExcludeRegion()
{
    m_recordExcludeRegion = QRect();
    LOG_DEBUG("Record exclude region cleared");
}

// ==================== 回放 ====================
void MacroEngine::macroThreadFunc(const QVector<MacroStep> &steps, uint64_t myGeneration, bool isRepeatMode, bool breakAfterOneCycle)
{
    s_activeMacroThreads.fetch_add(1, std::memory_order_relaxed);
    LOG_DEBUG("macro thread start (gen " << myGeneration << ")");

    using namespace std::chrono;
    using clock = steady_clock;

    double totalDelaySec = 0.0;
    for (const auto &step : steps)
    {
        if (step.type == StepType::Delay)
            totalDelaySec += step.delaySec;
    }

    int loopIndex = 1;
    while (!s_stopRequested.load() && (!isRepeatMode || s_isHotkeyPressed.load()))
    {
        if (s_macroGeneration.load() != myGeneration)
        {
            LOG_DEBUG("macro thread (gen " << myGeneration << ") generation expired, exiting");
            break;
        }

        auto cycleStart = clock::now();
        LOG_DEBUG(">> start loop " << loopIndex << " (total delay: " << totalDelaySec << "s)");

        double accumulatedDelay = 0.0;
        bool interrupted = false;

        for (int i = 0; i < steps.size(); ++i)
        {
            const auto &step = steps[i];

            if (s_stopRequested.load() || s_macroGeneration.load() != myGeneration)
            {
                LOG_DEBUG("macro thread interrupted between steps");
                interrupted = true;
                break;
            }

            switch (step.type)
            {
            case StepType::Delay:
            {
                accumulatedDelay += step.delaySec;
                LOG_DEBUG("    delay " << step.delaySec << "s (accumulated " << accumulatedDelay << "s)");
                auto target = cycleStart + duration<double>(accumulatedDelay);
                // 分片睡眠以支持中断
                const auto kMaxSleepMs = std::chrono::milliseconds(50);
                while (clock::now() < target)
                {
                    auto now = clock::now();
                    auto sleepTime = (target - now) < kMaxSleepMs ? (target - now) : kMaxSleepMs;
                    std::this_thread::sleep_for(sleepTime);
                    if (s_stopRequested.load() || (isRepeatMode && !s_isHotkeyPressed.load()))
                    {
                        LOG_DEBUG("loop interrupted during delay (hotkey released or stop requested)");
                        interrupted = true;
                        break;
                    }
                }
                break;
            }
            case StepType::KeyPress:
            {
                LOG_DEBUG("    key down 0x" << QString::number(step.keyOrButton, 16).toUpper().rightJustified(2, '0'));
                INPUT ip = {};
                ip.type = INPUT_KEYBOARD;
                ip.ki.wVk = static_cast<WORD>(step.keyOrButton);
                ip.ki.wScan = MapVirtualKey(step.keyOrButton, MAPVK_VK_TO_VSC);
                ip.ki.dwFlags = KEYEVENTF_SCANCODE;
                if ((step.keyOrButton >= 33 && step.keyOrButton <= 46) ||
                    (step.keyOrButton >= 91 && step.keyOrButton <= 93) ||
                    step.keyOrButton == VK_DIVIDE || step.keyOrButton == VK_RCONTROL ||
                    step.keyOrButton == VK_RMENU || step.keyOrButton == VK_RETURN ||
                    step.keyOrButton == VK_INSERT || step.keyOrButton == VK_DELETE ||
                    step.keyOrButton == VK_HOME || step.keyOrButton == VK_END ||
                    step.keyOrButton == VK_PRIOR || step.keyOrButton == VK_NEXT ||
                    step.keyOrButton == VK_LEFT || step.keyOrButton == VK_RIGHT ||
                    step.keyOrButton == VK_UP || step.keyOrButton == VK_DOWN ||
                    step.keyOrButton == VK_NUMLOCK || step.keyOrButton == VK_CANCEL ||
                    step.keyOrButton == VK_SNAPSHOT || step.keyOrButton == VK_DIVIDE)
                {
                    ip.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                }
                sendTrackedInput(1, &ip);
                break;
            }
            case StepType::KeyRelease:
            {
                LOG_DEBUG("    key up 0x" << QString::number(step.keyOrButton, 16).toUpper().rightJustified(2, '0'));
                INPUT ip = {};
                ip.type = INPUT_KEYBOARD;
                ip.ki.wVk = static_cast<WORD>(step.keyOrButton);
                ip.ki.wScan = MapVirtualKey(step.keyOrButton, MAPVK_VK_TO_VSC);
                ip.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_SCANCODE;
                if ((step.keyOrButton >= 33 && step.keyOrButton <= 46) ||
                    (step.keyOrButton >= 91 && step.keyOrButton <= 93) ||
                    step.keyOrButton == VK_DIVIDE || step.keyOrButton == VK_RCONTROL ||
                    step.keyOrButton == VK_RMENU || step.keyOrButton == VK_RETURN ||
                    step.keyOrButton == VK_INSERT || step.keyOrButton == VK_DELETE ||
                    step.keyOrButton == VK_HOME || step.keyOrButton == VK_END ||
                    step.keyOrButton == VK_PRIOR || step.keyOrButton == VK_NEXT ||
                    step.keyOrButton == VK_LEFT || step.keyOrButton == VK_RIGHT ||
                    step.keyOrButton == VK_UP || step.keyOrButton == VK_DOWN ||
                    step.keyOrButton == VK_NUMLOCK || step.keyOrButton == VK_CANCEL ||
                    step.keyOrButton == VK_SNAPSHOT || step.keyOrButton == VK_DIVIDE)
                {
                    ip.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                }
                sendTrackedInput(1, &ip);
                break;
            }
            case StepType::MousePress:
            {
                DWORD btn = step.keyOrButton;
                DWORD flags = 0;
                if (btn == VK_LBUTTON)
                    flags = MOUSEEVENTF_LEFTDOWN;
                else if (btn == VK_RBUTTON)
                    flags = MOUSEEVENTF_RIGHTDOWN;
                else if (btn == VK_MBUTTON)
                    flags = MOUSEEVENTF_MIDDLEDOWN;
                else if (btn == VK_XBUTTON1 || btn == VK_XBUTTON2)
                    flags = MOUSEEVENTF_XDOWN;
                LOG_DEBUG("    mouse down " << btn);
                INPUT ip = {};
                ip.type = INPUT_MOUSE;
                ip.mi.dwFlags = flags;
                if (btn == VK_XBUTTON1 || btn == VK_XBUTTON2)
                    ip.mi.mouseData = vkToXButtonMouseData(btn);
                sendTrackedInput(1, &ip);
                break;
            }
            case StepType::MouseRelease:
            {
                DWORD btn = step.keyOrButton;
                DWORD flags = 0;
                if (btn == VK_LBUTTON)
                    flags = MOUSEEVENTF_LEFTUP;
                else if (btn == VK_RBUTTON)
                    flags = MOUSEEVENTF_RIGHTUP;
                else if (btn == VK_MBUTTON)
                    flags = MOUSEEVENTF_MIDDLEUP;
                else if (btn == VK_XBUTTON1 || btn == VK_XBUTTON2)
                    flags = MOUSEEVENTF_XUP;
                LOG_DEBUG("    mouse up " << btn);
                INPUT ip = {};
                ip.type = INPUT_MOUSE;
                ip.mi.dwFlags = flags;
                if (btn == VK_XBUTTON1 || btn == VK_XBUTTON2)
                    ip.mi.mouseData = vkToXButtonMouseData(btn);
                sendTrackedInput(1, &ip);
                break;
            }
            case StepType::MouseScroll:
            {
                LOG_DEBUG("    scroll " << (step.scrollDelta > 0 ? "up" : "down"));
                INPUT ip = {};
                ip.type = INPUT_MOUSE;
                ip.mi.dwFlags = MOUSEEVENTF_WHEEL;
                ip.mi.mouseData = step.scrollDelta;
                sendTrackedInput(1, &ip);
                break;
            }
            }

            if (interrupted)
                break;
        }

        if (interrupted)
        {
            LOG_DEBUG("<< loop interrupted");
            break;
        }

        LOG_DEBUG("<< loop completed");

        if (breakAfterOneCycle)
        {
            LOG_DEBUG("break after one cycle (hold-execute mode)");
            break;
        }

        if (!isRepeatMode)
        {
            LOG_DEBUG("single execution mode, exiting");
            break;
        }

        if (totalDelaySec > 0)
        {
            auto nextCycleTime = cycleStart + duration<double>(totalDelaySec);
            std::this_thread::sleep_until(nextCycleTime);
        }
        else
        {
            // 无延迟的重复宏若全速循环会洪泛输入并占满 CPU，至少让出 1ms
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ++loopIndex;
    }

    if (s_macroGeneration.load() == myGeneration)
    {
        LOG_DEBUG("macro thread (gen " << myGeneration << ") normal exit, releasing pressed keys");
        releaseOnlyPressedKeys();
    }
    else
    {
        LOG_DEBUG("macro thread (gen " << myGeneration << ") expired, skip release");
    }

    s_activeMacroThreads.fetch_sub(1, std::memory_order_relaxed);
}

void MacroEngine::requestNewMacroOnMainThread(const Macro &macro)
{
    LOG_DEBUG("Main thread preempt: stopping old macro and releasing keys");

    // 立即复制步骤序列，防止后续 SendInput 触发嵌套消息循环导致 macro 引用失效
    const QVector<MacroStep> stepsCopy = macro.steps;
    const bool blockActionKeys = macro.blockActionKeys;

    s_stopRequested.store(true);
    // releaseOnlyPressedKeys() 已移至工作线程，避免主线程 SendInput 触发嵌套
    // 消息循环导致 Qt 容器 COW 断言崩溃 (qarraydataops.h:285 isMutable)

    // 根据宏的屏蔽动作键位设置，收集步骤中的 VK 码
    {
        std::lock_guard<std::mutex> lock(m_blockedActionKeyMutex);
        m_blockedActionKeyVkCodes.clear();
        m_blockActionKeys.store(blockActionKeys);
        if (blockActionKeys)
        {
            for (const auto &step : stepsCopy)
            {
                if (step.type == StepType::KeyPress || step.type == StepType::KeyRelease ||
                    step.type == StepType::MousePress || step.type == StepType::MouseRelease)
                {
                    m_blockedActionKeyVkCodes.insert(step.keyOrButton);
                }
            }
            LOG_DEBUG("Blocked action keys enabled, " << m_blockedActionKeyVkCodes.size() << " VK codes collected");
        }
    }

    bool shouldRepeat = (macro.repeatMode == 1 || macro.repeatMode == 2 || macro.repeatMode == 3);
    bool breakAfterOneCycle = (macro.repeatMode == 2);
    if (breakAfterOneCycle)
        shouldRepeat = true;
    s_isHotkeyPressed.store(shouldRepeat);
    s_stopRequested.store(false);
    s_disableInput.store(false); // 新宏启动，允许注入输入
    // 先递增 generation 让上一代线程退出，并短暂等待其真正结束，
    // 避免新旧线程并发 SendInput、旧线程退出时跳过释放导致按键卡住
    uint64_t newGen = ++s_macroGeneration;
    for (int i = 0; i < 20 && s_activeMacroThreads.load() > 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    LOG_DEBUG("Starting new macro (gen " << newGen << ", mode: " << macro.repeatMode << ", repeat: " << shouldRepeat << ")");

    // 所有 SendInput 调用均放入工作线程，避免主线程触发嵌套消息循环导致 Qt 容器重入崩溃
    QThread *worker = QThread::create([this, steps = stepsCopy, newGen, shouldRepeat, breakAfterOneCycle, blockActionKeys]()
                                      {
        // 先释放上一轮宏可能遗留的按键（原在主线程调用，现移至工作线程）
        releaseOnlyPressedKeys();

        if (blockActionKeys)
        {
            for (const auto &step : steps)
            {
                if (step.type == StepType::KeyPress || step.type == StepType::KeyRelease)
                {
                    if (GetAsyncKeyState(step.keyOrButton) & 0x8000)
                    {
                        INPUT ip = {};
                        ip.type = INPUT_KEYBOARD;
                        ip.ki.wVk = static_cast<WORD>(step.keyOrButton);
                        ip.ki.wScan = MapVirtualKey(step.keyOrButton, MAPVK_VK_TO_VSC);
                        ip.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_SCANCODE;
                        if ((step.keyOrButton >= 33 && step.keyOrButton <= 46) ||
                            (step.keyOrButton >= 91 && step.keyOrButton <= 93) ||
                            step.keyOrButton == VK_DIVIDE || step.keyOrButton == VK_RCONTROL ||
                            step.keyOrButton == VK_RMENU || step.keyOrButton == VK_RETURN ||
                            step.keyOrButton == VK_INSERT || step.keyOrButton == VK_DELETE ||
                            step.keyOrButton == VK_HOME || step.keyOrButton == VK_END ||
                            step.keyOrButton == VK_PRIOR || step.keyOrButton == VK_NEXT ||
                            step.keyOrButton == VK_LEFT || step.keyOrButton == VK_RIGHT ||
                            step.keyOrButton == VK_UP || step.keyOrButton == VK_DOWN ||
                            step.keyOrButton == VK_NUMLOCK || step.keyOrButton == VK_CANCEL ||
                            step.keyOrButton == VK_SNAPSHOT)
                        {
                            ip.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                        }
                        SendInput(1, &ip, sizeof(INPUT));
                    }
                }
                else if (step.type == StepType::MousePress || step.type == StepType::MouseRelease)
                {
                    if (GetAsyncKeyState(step.keyOrButton) & 0x8000)
                    {
                        DWORD flags = 0;
                        if (step.keyOrButton == VK_LBUTTON)
                            flags = MOUSEEVENTF_LEFTUP;
                        else if (step.keyOrButton == VK_RBUTTON)
                            flags = MOUSEEVENTF_RIGHTUP;
                        else if (step.keyOrButton == VK_MBUTTON)
                            flags = MOUSEEVENTF_MIDDLEUP;
                        else if (step.keyOrButton == VK_XBUTTON1 || step.keyOrButton == VK_XBUTTON2)
                            flags = MOUSEEVENTF_XUP;
                        if (flags)
                        {
                            INPUT ip = {};
                            ip.type = INPUT_MOUSE;
                            ip.mi.dwFlags = flags;
                            if (step.keyOrButton == VK_XBUTTON1 || step.keyOrButton == VK_XBUTTON2)
                                ip.mi.mouseData = vkToXButtonMouseData(step.keyOrButton);
                            SendInput(1, &ip, sizeof(INPUT));
                        }
                    }
                }
            }
        }
        macroThreadFunc(steps, newGen, shouldRepeat, breakAfterOneCycle);
        // 仅当本代宏仍是当前代时才清理屏蔽动作键，避免抢占后清除新宏刚设置的集合
        if (s_macroGeneration.load() == newGen)
        {
            std::lock_guard<std::mutex> lock(m_blockedActionKeyMutex);
            m_blockedActionKeyVkCodes.clear();
            m_blockActionKeys.store(false);
        }
        QMetaObject::invokeMethod(this, "playbackFinished", Qt::QueuedConnection, Q_ARG(QString, QString())); });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();

    emit playbackStarted(macro.name);
}

void MacroEngine::requestStopMacroOnMainThread()
{
    s_disableInput.store(true);
    LOG_DEBUG("Main thread: requesting stop + input disabled");
    s_stopRequested.store(true);
    s_isHotkeyPressed.store(false);
    m_suppressedMouseVkButtons.clear();
    m_suppressedKeyboardVkCodes.clear();
    {
        std::lock_guard<std::mutex> lock(m_blockedActionKeyMutex);
        m_blockedActionKeyVkCodes.clear();
        m_blockActionKeys.store(false);
    }
    // 将 SendInput 调用推迟到工作线程，避免主线程触发嵌套消息循环导致 Qt 容器 COW 断言
    QThread *worker = QThread::create([]()
                                      {
        releaseOnlyPressedKeys();
        forceShowCursor(); });
    connect(worker, &QThread::finished, worker, &QObject::deleteLater);
    worker->start();
}

void MacroEngine::clearBlockedActionKeys()
{
    std::lock_guard<std::mutex> lock(m_blockedActionKeyMutex);
    m_blockedActionKeyVkCodes.clear();
    m_blockActionKeys.store(false);
    LOG_DEBUG("Blocked action keys cleared");
}

// ==================== 图像自动执行 ====================

void MacroEngine::autoExecuteThreadFunc(int groupIndex, uint64_t generation)
{
    LOG_DEBUG("[AUTO-EXEC] 自动执行线程启动: 组[" << groupIndex << "] gen=" << generation);

    // 线程退出前清理主线程的鼠标锁定（QTimer 与 ClipCursor 必须在主线程释放）
    auto cleanupOnMainThread = [this]()
    {
        QMetaObject::invokeMethod(this, [this]()
                                  {
                                      if (m_mouseLockTimer)
                                          m_mouseLockTimer->stop();
                                      ClipCursor(nullptr);
                                      m_autoExecuteGroupIndex.store(-1);
                                  },
                                  Qt::QueuedConnection);
    };

    if (groupIndex < 0 || groupIndex >= static_cast<int>(m_groups.size()))
    {
        LOG_DEBUG("[AUTO-EXEC] 无效组索引，退出");
        m_autoExecuting.store(false);
        cleanupOnMainThread();
        return;
    }

    // 快照所需数据，避免工作线程无锁读取主线程容器 m_groups
    const QString targetExe = m_groups[groupIndex].targetExe;
    const QStringList macroNames = m_groups[groupIndex].macroNames;

    // 构建合并步骤序列：将所有组内宏的步骤按顺序合并
    QVector<MacroStep> combinedSteps;
    for (const auto &macroName : macroNames)
    {
        const Macro *macro = findMacro(macroName);
        if (!macro || !macro->enabled || macro->steps.isEmpty())
        {
            LOG_DEBUG("[AUTO-EXEC] 跳过无效宏: \"" << macroName << "\"");
            continue;
        }
        for (const auto &step : macro->steps)
            combinedSteps.append(step);
    }

    if (combinedSteps.isEmpty())
    {
        LOG_DEBUG("[AUTO-EXEC] 无有效步骤，退出");
        m_autoExecuting.store(false);
        cleanupOnMainThread();
        return;
    }

    LOG_DEBUG("[AUTO-EXEC] 合并步骤数: " << combinedSteps.size());

    int cx = m_mouseLockCenterX.load();
    int cy = m_mouseLockCenterY.load();
    RECT lockRect;
    lockRect.left = cx - 5;
    lockRect.top = cy - 5;
    lockRect.right = cx + 5;
    lockRect.bottom = cy + 5;

    // 跟踪当前线程按下的键/按钮，中断时自动释放
    struct PressedItem
    {
        bool isMouse;
        DWORD vk;
    };
    std::vector<PressedItem> pressedStack;

    auto releasePressedStack = [&]()
    {
        while (!pressedStack.empty())
        {
            PressedItem item = pressedStack.back();
            pressedStack.pop_back();
            INPUT ip = {};
            if (item.isMouse)
            {
                ip.type = INPUT_MOUSE;
                if (item.vk == VK_LBUTTON)
                    ip.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                else if (item.vk == VK_RBUTTON)
                    ip.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
                else if (item.vk == VK_MBUTTON)
                    ip.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
                else
                {
                    ip.mi.dwFlags = MOUSEEVENTF_XUP;
                    ip.mi.mouseData = item.vk;
                }
            }
            else
            {
                ip.type = INPUT_KEYBOARD;
                ip.ki.wVk = static_cast<WORD>(item.vk);
                ip.ki.wScan = MapVirtualKey(item.vk, MAPVK_VK_TO_VSC);
                ip.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_SCANCODE;
                if ((item.vk >= 33 && item.vk <= 46) ||
                    (item.vk >= 91 && item.vk <= 93) ||
                    item.vk == VK_DIVIDE || item.vk == VK_RCONTROL ||
                    item.vk == VK_RMENU || item.vk == VK_RETURN ||
                    item.vk == VK_INSERT || item.vk == VK_DELETE ||
                    item.vk == VK_HOME || item.vk == VK_END ||
                    item.vk == VK_PRIOR || item.vk == VK_NEXT ||
                    item.vk == VK_LEFT || item.vk == VK_RIGHT ||
                    item.vk == VK_UP || item.vk == VK_DOWN ||
                    item.vk == VK_NUMLOCK || item.vk == VK_CANCEL ||
                    item.vk == VK_SNAPSHOT || item.vk == VK_DIVIDE)
                    ip.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            }
            SendInput(1, &ip, sizeof(INPUT));
        }
    };

    // 同步锁定辅助（SendInput 可能干扰 ClipCursor，每次操作后立即重锁）
    auto relockMouse = [&]()
    {
        SetCursorPos(cx, cy);
        ClipCursor(&lockRect);
    };

    // 循环执行，直到被停止
    int cycleIndex = 1;
    while (!m_autoExecuteStop.load() && generation == m_autoExecuteGeneration.load())
    {
        // 前台切换到其他程序时立即停止，避免继续向任意窗口注入键鼠并锁死鼠标
        if (!targetExe.isEmpty())
        {
            QString fgProc = getProcessNameFromWindow(GetForegroundWindow());
            if (fgProc.compare(targetExe, Qt::CaseInsensitive) != 0)
            {
                LOG_DEBUG("[AUTO-EXEC] 前台已不是目标程序，停止自动执行");
                break;
            }
        }
        LOG_DEBUG("[AUTO-EXEC] 开始第 " << cycleIndex << " 轮执行");

        using namespace std::chrono;
        using clock = steady_clock;

        auto cycleStart = clock::now();
        double accumulatedDelay = 0.0;
        bool interrupted = false;

        for (int i = 0; i < combinedSteps.size(); ++i)
        {
            if (m_autoExecuteStop.load() || generation != m_autoExecuteGeneration.load())
            {
                interrupted = true;
                break;
            }

            const auto &step = combinedSteps[i];

            switch (step.type)
            {
            case StepType::Delay:
            {
                accumulatedDelay += step.delaySec;
                auto target = cycleStart + duration<double>(accumulatedDelay);
                const auto kMaxSleepMs = std::chrono::milliseconds(50);
                while (clock::now() < target)
                {
                    auto now = clock::now();
                    auto sleepTime = (target - now) < kMaxSleepMs ? (target - now) : kMaxSleepMs;
                    std::this_thread::sleep_for(sleepTime);
                    // 延迟期间持续锁定
                    relockMouse();
                    if (m_autoExecuteStop.load() || generation != m_autoExecuteGeneration.load())
                    {
                        interrupted = true;
                        break;
                    }
                }
                break;
            }
            case StepType::KeyPress:
            {
                pressedStack.push_back({false, step.keyOrButton});
                INPUT ip = {};
                ip.type = INPUT_KEYBOARD;
                ip.ki.wVk = static_cast<WORD>(step.keyOrButton);
                ip.ki.wScan = MapVirtualKey(step.keyOrButton, MAPVK_VK_TO_VSC);
                ip.ki.dwFlags = KEYEVENTF_SCANCODE;
                if ((step.keyOrButton >= 33 && step.keyOrButton <= 46) ||
                    (step.keyOrButton >= 91 && step.keyOrButton <= 93) ||
                    step.keyOrButton == VK_DIVIDE || step.keyOrButton == VK_RCONTROL ||
                    step.keyOrButton == VK_RMENU || step.keyOrButton == VK_RETURN ||
                    step.keyOrButton == VK_INSERT || step.keyOrButton == VK_DELETE ||
                    step.keyOrButton == VK_HOME || step.keyOrButton == VK_END ||
                    step.keyOrButton == VK_PRIOR || step.keyOrButton == VK_NEXT ||
                    step.keyOrButton == VK_LEFT || step.keyOrButton == VK_RIGHT ||
                    step.keyOrButton == VK_UP || step.keyOrButton == VK_DOWN ||
                    step.keyOrButton == VK_NUMLOCK || step.keyOrButton == VK_CANCEL ||
                    step.keyOrButton == VK_SNAPSHOT || step.keyOrButton == VK_DIVIDE)
                {
                    ip.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                }
                SendInput(1, &ip, sizeof(INPUT));
                relockMouse();
                break;
            }
            case StepType::KeyRelease:
            {
                // 从栈中移除匹配的按下记录
                for (auto it = pressedStack.rbegin(); it != pressedStack.rend(); ++it)
                {
                    if (!it->isMouse && it->vk == step.keyOrButton)
                    {
                        pressedStack.erase(std::next(it).base());
                        break;
                    }
                }
                INPUT ip = {};
                ip.type = INPUT_KEYBOARD;
                ip.ki.wVk = static_cast<WORD>(step.keyOrButton);
                ip.ki.wScan = MapVirtualKey(step.keyOrButton, MAPVK_VK_TO_VSC);
                ip.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_SCANCODE;
                if ((step.keyOrButton >= 33 && step.keyOrButton <= 46) ||
                    (step.keyOrButton >= 91 && step.keyOrButton <= 93) ||
                    step.keyOrButton == VK_DIVIDE || step.keyOrButton == VK_RCONTROL ||
                    step.keyOrButton == VK_RMENU || step.keyOrButton == VK_RETURN ||
                    step.keyOrButton == VK_INSERT || step.keyOrButton == VK_DELETE ||
                    step.keyOrButton == VK_HOME || step.keyOrButton == VK_END ||
                    step.keyOrButton == VK_PRIOR || step.keyOrButton == VK_NEXT ||
                    step.keyOrButton == VK_LEFT || step.keyOrButton == VK_RIGHT ||
                    step.keyOrButton == VK_UP || step.keyOrButton == VK_DOWN ||
                    step.keyOrButton == VK_NUMLOCK || step.keyOrButton == VK_CANCEL ||
                    step.keyOrButton == VK_SNAPSHOT || step.keyOrButton == VK_DIVIDE)
                {
                    ip.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
                }
                SendInput(1, &ip, sizeof(INPUT));
                relockMouse();
                break;
            }
            case StepType::MousePress:
            {
                DWORD btn = step.keyOrButton;
                pressedStack.push_back({true, btn});
                DWORD flags = 0;
                if (btn == VK_LBUTTON)
                    flags = MOUSEEVENTF_LEFTDOWN;
                else if (btn == VK_RBUTTON)
                    flags = MOUSEEVENTF_RIGHTDOWN;
                else if (btn == VK_MBUTTON)
                    flags = MOUSEEVENTF_MIDDLEDOWN;
                else if (btn == VK_XBUTTON1 || btn == VK_XBUTTON2)
                    flags = MOUSEEVENTF_XDOWN;
                INPUT ip = {};
                ip.type = INPUT_MOUSE;
                ip.mi.dwFlags = flags;
                if (btn == VK_XBUTTON1 || btn == VK_XBUTTON2)
                    ip.mi.mouseData = vkToXButtonMouseData(btn);
                SendInput(1, &ip, sizeof(INPUT));
                relockMouse();
                break;
            }
            case StepType::MouseRelease:
            {
                DWORD btn = step.keyOrButton;
                for (auto it = pressedStack.rbegin(); it != pressedStack.rend(); ++it)
                {
                    if (it->isMouse && it->vk == btn)
                    {
                        pressedStack.erase(std::next(it).base());
                        break;
                    }
                }
                DWORD flags = 0;
                if (btn == VK_LBUTTON)
                    flags = MOUSEEVENTF_LEFTUP;
                else if (btn == VK_RBUTTON)
                    flags = MOUSEEVENTF_RIGHTUP;
                else if (btn == VK_MBUTTON)
                    flags = MOUSEEVENTF_MIDDLEUP;
                else if (btn == VK_XBUTTON1 || btn == VK_XBUTTON2)
                    flags = MOUSEEVENTF_XUP;
                INPUT ip = {};
                ip.type = INPUT_MOUSE;
                ip.mi.dwFlags = flags;
                if (btn == VK_XBUTTON1 || btn == VK_XBUTTON2)
                    ip.mi.mouseData = vkToXButtonMouseData(btn);
                SendInput(1, &ip, sizeof(INPUT));
                relockMouse();
                break;
            }
            case StepType::MouseScroll:
            {
                INPUT ip = {};
                ip.type = INPUT_MOUSE;
                ip.mi.dwFlags = MOUSEEVENTF_WHEEL;
                ip.mi.mouseData = step.scrollDelta;
                SendInput(1, &ip, sizeof(INPUT));
                relockMouse();
                break;
            }
            }

            if (interrupted)
                break;
        }

        if (interrupted)
        {
            LOG_DEBUG("[AUTO-EXEC] 第 " << cycleIndex << " 轮被中断");
            break;
        }

        LOG_DEBUG("[AUTO-EXEC] 第 " << cycleIndex << " 轮执行完毕");
        ++cycleIndex;
    }

    // 释放所有未弹起的键/按钮
    releasePressedStack();
    releaseOnlyPressedKeys();
    LOG_DEBUG("[AUTO-EXEC] 自动执行线程退出");
    m_autoExecuting.store(false);
    cleanupOnMainThread();
}

void MacroEngine::startAutoExecute(int groupIndex, int screenCenterX, int screenCenterY)
{
    // 如果已经在自动执行，先停止旧的
    if (m_autoExecuting.load())
    {
        LOG_DEBUG("[AUTO-EXEC] 已有自动执行在进行中，先停止旧的");
        stopAutoExecute();
    }
    // 旧线程可能已自然退出（m_autoExecuting=false）但成员仍 joinable；
    // 直接移动赋值 joinable 的 std::thread 会触发 std::terminate，必须先 join。
    if (m_autoExecuteThread.joinable())
        m_autoExecuteThread.join();
    m_autoExecuting.store(false);

    m_autoExecuteStop.store(false);
    m_autoExecuting.store(true);
    m_autoExecuteGroupIndex.store(groupIndex);
    m_mouseLockCenterX.store(screenCenterX);
    m_mouseLockCenterY.store(screenCenterY);
    ++m_autoExecuteGeneration;

    // 初始移动鼠标
    SetCursorPos(screenCenterX, screenCenterY);

    // 启动主线程定时器驱动鼠标锁定（ClipCursor 需要消息泵才能可靠工作）
    if (!m_mouseLockTimer)
    {
        m_mouseLockTimer = new QTimer(this);
        m_mouseLockTimer->setInterval(5);
        connect(m_mouseLockTimer, &QTimer::timeout, this, [this]()
                {
            if (!m_autoExecuting.load()) return;
            int cx = m_mouseLockCenterX.load();
            int cy = m_mouseLockCenterY.load();
            SetCursorPos(cx, cy);
            RECT r;
            r.left   = cx - 5;
            r.top    = cy - 5;
            r.right  = cx + 5;
            r.bottom = cy + 5;
            ClipCursor(&r); });
    }
    m_mouseLockTimer->start();

    // 启动自动执行线程
    uint64_t gen = m_autoExecuteGeneration.load();
    m_autoExecuteThread = std::thread(&MacroEngine::autoExecuteThreadFunc, this, groupIndex, gen);
}

void MacroEngine::stopAutoExecute()
{
    // 线程可能已自然退出（m_autoExecuting 已为 false）但仍是 joinable，
    // 不能仅凭 m_autoExecuting 提前返回，否则析构/重启时 std::terminate。
    if (!m_autoExecuting.load() && !m_autoExecuteThread.joinable())
        return;

    LOG_DEBUG("[AUTO-EXEC] 请求停止自动执行");
    m_autoExecuteStop.store(true);
    ++m_autoExecuteGeneration; // 使旧线程的 generation 失效

    // 停止主线程鼠标锁定定时器
    if (m_mouseLockTimer)
        m_mouseLockTimer->stop();
    ClipCursor(nullptr);
    forceShowCursor();

    // 等待线程退出
    if (m_autoExecuteThread.joinable())
    {
        m_autoExecuteThread.join();
    }

    m_autoExecuteGroupIndex.store(-1);
    m_autoExecuting.store(false);
    LOG_DEBUG("[AUTO-EXEC] 自动执行已完全停止");
}

void MacroEngine::triggerMacro(const Macro &macro)
{
    if (!macro.enabled || macro.steps.isEmpty())
        return;

    LOG_DEBUG("Trigger macro: " << macro.name << ", repeatMode: " << macro.repeatMode);

    switch (macro.repeatMode)
    {
    case 0:
        requestNewMacroOnMainThread(macro);
        break;
    case 1:
        requestNewMacroOnMainThread(macro);
        break;
    case 2:
        requestNewMacroOnMainThread(macro);
        break;
    case 3:
        // 按一下开始重复、再按一下停止：用"是否有宏线程在运行"判断，
        // 不能依赖 s_stopRequested（宏自然结束后它仍为 false，会把首次按下误当停止）
        if (s_activeMacroThreads.load() > 0 && !s_stopRequested.load())
            requestStopMacroOnMainThread();
        else
            requestNewMacroOnMainThread(macro);
        break;
    default:
        break;
    }
}

void MacroEngine::playMacro(const Macro &macro)
{
    triggerMacro(macro);
}

void MacroEngine::stopPlayback()
{
    requestStopMacroOnMainThread();
}

// ==================== 数据访问 ====================
const QVector<MacroGroup> &MacroEngine::groups() const { return m_groups; }

void MacroEngine::addGroup(const MacroGroup &grp)
{
    LOG_DEBUG("Adding group: " << grp.name);
    m_groups.append(grp);
    emit dataChanged();
}

void MacroEngine::removeGroup(int index)
{
    if (index < 0 || index >= m_groups.size())
        return;
    LOG_DEBUG("Removing group: " << m_groups[index].name);

    // 删除前停止该组的图像识别，避免后台线程继续使用旧索引
    stopImageMatchForGroup(index);
    if (m_autoExecuting.load())
        stopAutoExecute();

    m_groups.removeAt(index);

    // 所有以组索引为键的状态在删除后必须重映射，否则会指向错误的组
    auto remapSet = [index](QSet<int> &s)
    {
        QSet<int> out;
        for (int v : s)
        {
            if (v == index)
                continue;
            out.insert(v > index ? v - 1 : v);
        }
        s = out;
    };
    remapSet(m_activeGroupIndices);
    remapSet(m_pendingImageGroupIndices);
    remapSet(m_previousActiveIndices);
    remapSet(m_previousPendingIndices);
    {
        std::lock_guard<std::mutex> lock(m_imageCheckMutex);
        for (auto it = m_imageCheckItems.begin(); it != m_imageCheckItems.end();)
        {
            if (it->groupIndex == index)
            {
                it = m_imageCheckItems.erase(it);
            }
            else
            {
                if (it->groupIndex > index)
                    --it->groupIndex;
                ++it;
            }
        }
        std::set<int> reset;
        for (int v : m_pendingMatchStateReset)
        {
            if (v == index)
                continue;
            reset.insert(v > index ? v - 1 : v);
        }
        m_pendingMatchStateReset = reset;
    }
    {
        std::lock_guard<std::mutex> rlock(m_imageResultMutex);
        std::map<int, bool> results;
        for (const auto &kv : m_imageMatchResults)
        {
            if (kv.first == index)
                continue;
            results[kv.first > index ? kv.first - 1 : kv.first] = kv.second;
        }
        m_imageMatchResults = results;
    }

    rebuildHotkeyVkCodes();
    updateHooksState();
    emit dataChanged();
}

MacroGroup *MacroEngine::mutableGroup(int index)
{
    if (index >= 0 && index < m_groups.size())
        return &m_groups[index];
    return nullptr;
}

Macro MacroEngine::currentMacro() const { return m_currentMacro; }

void MacroEngine::setCurrentMacro(const Macro &macro)
{
    LOG_DEBUG("Setting current macro: " << macro.name);
    m_currentMacro = macro;
    for (int i = 0; i < m_macroInventory.size(); ++i)
    {
        if (m_macroInventory[i].name == macro.name)
        {
            m_macroInventory[i] = macro;
            LOG_DEBUG("Updated inventory[" << i << "]");
            break;
        }
    }
    emit dataChanged();
}

// ==================== 全局宏库存 ====================
const QVector<Macro> &MacroEngine::macroInventory() const { return m_macroInventory; }

int MacroEngine::addToInventory(const Macro &macro)
{
    m_macroInventory.append(macro);
    LOG_DEBUG("Added macro to inventory: " << macro.name << " (index " << (m_macroInventory.size() - 1) << ")");
    emit dataChanged();
    return m_macroInventory.size() - 1;
}

bool MacroEngine::removeFromInventory(const QString &name)
{
    // 调用方常传入 m_macroInventory 内元素的 QString 引用（如 const Macro &m = inv[idx]），
    // removeAt 后该引用会悬垂或指向下一个宏，必须先拷贝成独立值再使用。
    const QString safeName = name;
    int idx = findMacroIndex(safeName);
    if (idx < 0)
    {
        LOG_DEBUG("removeFromInventory: macro not found: " << safeName);
        return false;
    }
    m_macroInventory.removeAt(idx);
    for (auto &grp : m_groups)
    {
        int ri = grp.macroNames.indexOf(safeName);
        if (ri >= 0)
            grp.macroNames.removeAt(ri);
    }
    if (m_currentMacro.name == safeName)
        m_currentMacro = Macro();
    // 删除已触发宏的状态，否则按住重复/切换模式可能被残留条目误触发
    for (auto it = m_activeTriggeredByKey.begin(); it != m_activeTriggeredByKey.end();)
    {
        if (it.value() == safeName)
            it = m_activeTriggeredByKey.erase(it);
        else
            ++it;
    }
    QStringList keysToRemove;
    for (auto it = m_triggerBindings.begin(); it != m_triggerBindings.end(); ++it)
        if (it.value() == safeName)
            keysToRemove.append(it.key());
    for (const auto &k : keysToRemove)
        m_triggerBindings.remove(k);
    rebuildHotkeyVkCodes();
    LOG_DEBUG("Removed macro from inventory: " << safeName);
    emit dataChanged();
    return true;
}

Macro *MacroEngine::findMutableMacro(const QString &name)
{
    for (auto &m : m_macroInventory)
        if (m.name == name)
            return &m;
    return nullptr;
}

const Macro *MacroEngine::findMacro(const QString &name) const
{
    for (const auto &m : m_macroInventory)
        if (m.name == name)
            return &m;
    return nullptr;
}

int MacroEngine::findMacroIndex(const QString &name) const
{
    for (int i = 0; i < m_macroInventory.size(); ++i)
        if (m_macroInventory[i].name == name)
            return i;
    return -1;
}

void MacroEngine::updateInInventory(const QString &oldName, const Macro &newMacro)
{
    int idx = findMacroIndex(oldName);
    if (idx < 0)
    {
        LOG_DEBUG("updateInInventory: macro not found: " << oldName);
        return;
    }
    m_macroInventory[idx] = newMacro;
    if (oldName != newMacro.name)
    {
        for (auto &grp : m_groups)
        {
            int ri = grp.macroNames.indexOf(oldName);
            if (ri >= 0)
                grp.macroNames[ri] = newMacro.name;
        }
        for (auto it = m_triggerBindings.begin(); it != m_triggerBindings.end(); ++it)
        {
            if (it.value() == oldName)
                it.value() = newMacro.name;
        }
        // 已触发状态里记录的宏名也要同步，否则改名后按住重复/切换模式会失去跟踪
        for (auto it = m_activeTriggeredByKey.begin(); it != m_activeTriggeredByKey.end(); ++it)
        {
            if (it.value() == oldName)
                it.value() = newMacro.name;
        }
    }
    if (m_currentMacro.name == oldName || m_currentMacro.name == newMacro.name)
        m_currentMacro = newMacro;
    rebuildHotkeyVkCodes();
    emit dataChanged();
    LOG_DEBUG("Updated inventory macro: " << oldName << " -> " << newMacro.name);
}

// ==================== 宏组激活与前台窗口检测 ====================

ActivationMode MacroEngine::activationMode() const { return m_activationMode; }

void MacroEngine::setActivationMode(ActivationMode mode)
{
    if (m_activationMode != mode)
    {
        m_activationMode = mode;
        LOG_DEBUG("Activation mode changed to: " << (mode == ActivationMode::Global ? "Global" : "Group"));

        if (mode == ActivationMode::Global)
        {
            // 模式切换后图像自动执行失去来源组，必须停止，避免继续注入输入
            if (m_autoExecuting.load())
                stopAutoExecute();
            QSet<int> oldActive = m_activeGroupIndices;
            m_activeGroupIndices.clear();
            m_pendingImageGroupIndices.clear();
            m_pressedHotkeyVks.clear();
            stopAllImageMatch();
            requestStopMacroOnMainThread();
            for (int idx : oldActive)
                emit activeGroupDeactivated(idx);
            updateHooksState();
        }
        else
        {
            checkForegroundWindow();
        }

        emit activationModeChanged(mode);
        emit dataChanged();
        rebuildHotkeyVkCodes();
    }
}

int MacroEngine::activeGroupIndex() const
{
    if (m_activationMode == ActivationMode::Global)
        return -1;
    return m_activeGroupIndices.isEmpty() ? -1 : *m_activeGroupIndices.begin();
}

QString MacroEngine::currentForegroundProcess() const { return m_foregroundProcess; }

int MacroEngine::foregroundActiveGroupIndex() const
{
    if (m_activationMode == ActivationMode::Global)
        return -1;
    return m_activeGroupIndices.isEmpty() ? -1 : *m_activeGroupIndices.begin();
}

QSet<int> MacroEngine::activeGroupIndices() const
{
    return m_activeGroupIndices;
}

bool MacroEngine::isMacroActive(int groupIndex, int macroIndex) const
{
    Q_UNUSED(macroIndex);
    if (m_activationMode == ActivationMode::Global)
        return true;
    return m_activeGroupIndices.contains(groupIndex);
}

void MacroEngine::moveMacroToGroup(int fromGroup, int macroIdx, int toGroup)
{
    if (fromGroup < 0 || fromGroup >= m_groups.size())
        return;
    if (toGroup < 0 || toGroup >= m_groups.size())
        return;
    if (macroIdx < 0 || macroIdx >= m_groups[fromGroup].macroNames.size())
        return;
    QString name = m_groups[fromGroup].macroNames[macroIdx];
    m_groups[fromGroup].macroNames.removeAt(macroIdx);
    m_groups[toGroup].macroNames.append(name);
    rebuildHotkeyVkCodes();
    emit dataChanged();
}

void MacroEngine::copyMacroToGroup(int fromGroup, int macroIdx, int toGroup)
{
    if (fromGroup < 0 || fromGroup >= m_groups.size())
        return;
    if (toGroup < 0 || toGroup >= m_groups.size())
        return;
    if (macroIdx < 0 || macroIdx >= m_groups[fromGroup].macroNames.size())
        return;
    QString name = m_groups[fromGroup].macroNames[macroIdx];
    m_groups[toGroup].macroNames.append(name);
    rebuildHotkeyVkCodes();
    emit dataChanged();
}

QString MacroEngine::getProcessNameFromWindow(HWND hwnd) const
{
    if (!hwnd)
        return QString();
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid)
        return QString();
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess)
        return QString();
    wchar_t exePath[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    QString result;
    if (QueryFullProcessImageNameW(hProcess, 0, exePath, &size))
    {
        QString fullPath = QString::fromWCharArray(exePath);
        int lastSlash = fullPath.lastIndexOf('\\');
        result = (lastSlash >= 0) ? fullPath.mid(lastSlash + 1) : fullPath;
    }
    CloseHandle(hProcess);
    return result;
}

void MacroEngine::checkForegroundWindow()
{
    try
    {
        if (m_activationMode != ActivationMode::Group)
        {
            // 切回全局模式时，图像自动执行失去来源组，必须停止，避免继续向任意前台窗口注入输入
            if (m_autoExecuting.load())
                stopAutoExecute();
            stopAllImageMatch();
            m_pendingImageGroupIndices.clear();
            if (!m_activeGroupIndices.isEmpty())
            {
                QSet<int> oldActive = m_activeGroupIndices;
                m_activeGroupIndices.clear();
                m_pressedHotkeyVks.clear();
                requestStopMacroOnMainThread();
                forceShowCursor();
                for (int idx : oldActive)
                    emit activeGroupDeactivated(idx);
                emit dataChanged();
                rebuildHotkeyVkCodes();
                updateHooksState();
            }
            return;
        }

        HWND hwnd = GetForegroundWindow();
        QString procName = getProcessNameFromWindow(hwnd);
        if (procName != m_foregroundProcess)
        {
            m_foregroundProcess = procName;
            emit foregroundProcessChanged(procName);
        }

        // 找出所有目标程序匹配的组索引
        QSet<int> matchedGroups;
        for (int i = 0; i < m_groups.size(); ++i)
        {
            const auto &grp = m_groups[i];
            if (!grp.targetExe.isEmpty() && procName.compare(grp.targetExe, Qt::CaseInsensitive) == 0)
                matchedGroups.insert(i);
        }

        // 移除不再匹配的组
        QSet<int> toRemoveActive;
        for (int idx : m_activeGroupIndices)
        {
            if (!matchedGroups.contains(idx))
                toRemoveActive.insert(idx);
        }
        for (int idx : toRemoveActive)
        {
            m_activeGroupIndices.remove(idx);
            // 若自动执行来源于该组，组失活时必须停止（否则会继续向任意前台窗口注入键鼠并锁定鼠标）
            if (m_autoExecuting.load() && m_autoExecuteGroupIndex.load() == idx)
                stopAutoExecute();
            // 清理属于本组已触发宏的 m_activeTriggeredByKey 条目
            const auto &grp = m_groups[idx];
            for (const auto &macroName : grp.macroNames)
            {
                for (auto it = m_activeTriggeredByKey.begin(); it != m_activeTriggeredByKey.end();)
                {
                    if (it.value() == macroName)
                        it = m_activeTriggeredByKey.erase(it);
                    else
                        ++it;
                }
            }
            requestStopMacroOnMainThread();
            forceShowCursor();
            stopImageMatchForGroup(idx);
            emit activeGroupDeactivated(idx);
        }
        QSet<int> toRemovePending;
        for (int idx : m_pendingImageGroupIndices)
        {
            if (!matchedGroups.contains(idx))
                toRemovePending.insert(idx);
        }
        for (int idx : toRemovePending)
        {
            m_pendingImageGroupIndices.remove(idx);
            stopImageMatchForGroup(idx);
        }

        if (!toRemoveActive.isEmpty() || !toRemovePending.isEmpty())
        {
            emit dataChanged();
            rebuildHotkeyVkCodes();
            updateHooksState();
        }

        // 处理新匹配的组
        for (int idx : matchedGroups)
        {
            const auto &grp = m_groups[idx];
            if (m_activeGroupIndices.contains(idx) || m_pendingImageGroupIndices.contains(idx))
                continue;

            if (grp.imagePaths.isEmpty())
            {
                // 无图像直接激活
                m_activeGroupIndices.insert(idx);
                emit activeGroupActivated(idx);
            }
            else
            {
                // 需要图像识别
                m_pendingImageGroupIndices.insert(idx);
                // 确保图像匹配线程包含此组
                startImageMatchForGroup(idx);
            }
        }

        // 仅当激活组或挂起组集合发生变化时才触发信号与热键重建
        if (m_activeGroupIndices != m_previousActiveIndices ||
            m_pendingImageGroupIndices != m_previousPendingIndices)
        {
            m_previousActiveIndices = m_activeGroupIndices;
            m_previousPendingIndices = m_pendingImageGroupIndices;

            if (!m_activeGroupIndices.isEmpty() || !m_pendingImageGroupIndices.isEmpty())
            {
                emit dataChanged();
                rebuildHotkeyVkCodes();
                updateHooksState();
            }
        }
    }
    catch (const std::exception &e)
    {
        LOG_DEBUG("checkForegroundWindow exception: " << e.what());
    }
    catch (...)
    {
        LOG_DEBUG("checkForegroundWindow unknown exception");
    }
}

// ==================== 键码转换 ====================
QString MacroEngine::mouseButtonToString(WORD button) const
{
    switch (button)
    {
    case VK_LBUTTON:
        return "左键";
    case VK_RBUTTON:
        return "右键";
    case VK_MBUTTON:
        return "中键";
    case VK_XBUTTON1:
        return "X1键";
    case VK_XBUTTON2:
        return "X2键";
    default:
        return QString("鼠标%1").arg(button);
    }
}

QString MacroEngine::keyToString(DWORD vk, bool isMouse, int mouseData) const
{
    if (isMouse)
        return mouseButtonToString(static_cast<WORD>(vk));

    static QMap<DWORD, QString> keyMap;
    if (keyMap.isEmpty())
    {
        for (int i = 'A'; i <= 'Z'; ++i)
            keyMap[i] = QChar(i);
        for (int i = '0'; i <= '9'; ++i)
            keyMap[i] = QChar(i);
        keyMap[VK_SPACE] = "Space";
        keyMap[VK_RETURN] = "Enter";
        keyMap[VK_TAB] = "Tab";
        keyMap[VK_ESCAPE] = "Esc";
        keyMap[VK_BACK] = "Backspace";
        keyMap[VK_UP] = "↑";
        keyMap[VK_DOWN] = "↓";
        keyMap[VK_LEFT] = "←";
        keyMap[VK_RIGHT] = "→";
        keyMap[VK_HOME] = "Home";
        keyMap[VK_END] = "End";
        keyMap[VK_PRIOR] = "PgUp";
        keyMap[VK_NEXT] = "PgDn";
        keyMap[VK_INSERT] = "Ins";
        keyMap[VK_DELETE] = "Del";
        for (int i = VK_F1; i <= VK_F24; ++i)
            keyMap[i] = QString("F%1").arg(i - VK_F1 + 1);
        keyMap[VK_CONTROL] = "Ctrl";
        keyMap[VK_LCONTROL] = "Ctrl";
        keyMap[VK_RCONTROL] = "Ctrl";
        keyMap[VK_MENU] = "Alt";
        keyMap[VK_LMENU] = "Alt";
        keyMap[VK_RMENU] = "Alt";
        keyMap[VK_SHIFT] = "Shift";
        keyMap[VK_LSHIFT] = "Shift";
        keyMap[VK_RSHIFT] = "Shift";
        keyMap[VK_LWIN] = "Win";
        keyMap[VK_RWIN] = "Win";
        keyMap[VK_NUMLOCK] = "NumLock";
        keyMap[VK_SCROLL] = "ScrollLock";
        keyMap[VK_PRINT] = "PrtSc";
        keyMap[VK_SNAPSHOT] = "PrtSc";
        keyMap[VK_PAUSE] = "Pause";
        keyMap[VK_CAPITAL] = "CapsLock";
        keyMap[VK_OEM_1] = ";";
        keyMap[VK_OEM_2] = "/";
        keyMap[VK_OEM_3] = "`";
        keyMap[VK_OEM_4] = "[";
        keyMap[VK_OEM_5] = "\\";
        keyMap[VK_OEM_6] = "]";
        keyMap[VK_OEM_7] = "'";
        keyMap[VK_OEM_COMMA] = ",";
        keyMap[VK_OEM_PERIOD] = ".";
        keyMap[VK_OEM_MINUS] = "-";
        keyMap[VK_OEM_PLUS] = "=";
        keyMap[VK_MULTIPLY] = "Num*";
        keyMap[VK_ADD] = "Num+";
        keyMap[VK_SEPARATOR] = "Num,";
        keyMap[VK_SUBTRACT] = "Num-";
        keyMap[VK_DECIMAL] = "Num.";
        keyMap[VK_DIVIDE] = "Num/";
        keyMap[VK_NUMPAD0] = "Num0";
        keyMap[VK_NUMPAD1] = "Num1";
        keyMap[VK_NUMPAD2] = "Num2";
        keyMap[VK_NUMPAD3] = "Num3";
        keyMap[VK_NUMPAD4] = "Num4";
        keyMap[VK_NUMPAD5] = "Num5";
        keyMap[VK_NUMPAD6] = "Num6";
        keyMap[VK_NUMPAD7] = "Num7";
        keyMap[VK_NUMPAD8] = "Num8";
        keyMap[VK_NUMPAD9] = "Num9";
    }
    if (keyMap.contains(vk))
        return keyMap[vk];
    return QString("VK_0x%1").arg(vk, 2, 16, QLatin1Char('0'));
}

DWORD MacroEngine::keyNameToVkCode(const QString &keyName) const
{
    static QMap<QString, DWORD> revMap;
    if (revMap.isEmpty())
    {
        for (int i = 'A'; i <= 'Z'; ++i)
            revMap[QChar(i).toLower()] = i;
        for (int i = '0'; i <= '9'; ++i)
            revMap[QString(QChar(i))] = i;
        for (int i = VK_F1; i <= VK_F12; ++i)
            revMap[QString("f%1").arg(i - VK_F1 + 1)] = i;
        revMap["esc"] = VK_ESCAPE;
        revMap["space"] = VK_SPACE;
        revMap["enter"] = VK_RETURN;
        revMap["tab"] = VK_TAB;
        revMap["backspace"] = VK_BACK;
        revMap["caps lock"] = VK_CAPITAL;
        revMap["shift"] = VK_SHIFT;
        revMap["ctrl"] = VK_CONTROL;
        revMap["right ctrl"] = VK_RCONTROL;
        revMap["alt"] = VK_MENU;
        revMap["alt gr"] = VK_MENU;
        revMap["left windows"] = VK_LWIN;
        revMap["right windows"] = VK_RWIN;
        revMap["menu"] = VK_APPS;
        revMap["up"] = VK_UP;
        revMap["down"] = VK_DOWN;
        revMap["left"] = VK_LEFT;
        revMap["right"] = VK_RIGHT;
        revMap["insert"] = VK_INSERT;
        revMap["home"] = VK_HOME;
        revMap["page up"] = VK_PRIOR;
        revMap["delete"] = VK_DELETE;
        revMap["end"] = VK_END;
        revMap["page down"] = VK_NEXT;
        revMap["`"] = VK_OEM_3;
        revMap["-"] = VK_OEM_MINUS;
        revMap["="] = VK_OEM_PLUS;
        revMap["["] = VK_OEM_4;
        revMap["]"] = VK_OEM_6;
        revMap["\\"] = VK_OEM_5;
        revMap[";"] = VK_OEM_1;
        revMap["'"] = VK_OEM_7;
        revMap[","] = VK_OEM_COMMA;
        revMap["."] = VK_OEM_PERIOD;
        revMap["/"] = VK_OEM_2;
        revMap["print screen"] = VK_PRINT;
        revMap["scroll lock"] = VK_SCROLL;
        revMap["pause"] = VK_PAUSE;
        revMap["num lock"] = VK_NUMLOCK;
        revMap["num /"] = VK_DIVIDE;
        revMap["num *"] = VK_MULTIPLY;
        revMap["num -"] = VK_SUBTRACT;
        revMap["num +"] = VK_ADD;
        revMap["num ."] = VK_DECIMAL;
        revMap["num enter"] = VK_RETURN;
        // ===== 与 keyToString 输出保持一致（热键文本由 keyToString 生成，抑制集合需能反查）=====
        revMap["↑"] = VK_UP;
        revMap["↓"] = VK_DOWN;
        revMap["←"] = VK_LEFT;
        revMap["→"] = VK_RIGHT;
        revMap["ins"] = VK_INSERT;
        revMap["del"] = VK_DELETE;
        revMap["pgup"] = VK_PRIOR;
        revMap["pgdn"] = VK_NEXT;
        revMap["prtsc"] = VK_SNAPSHOT;
        revMap["capslock"] = VK_CAPITAL;
        revMap["scrolllock"] = VK_SCROLL;
        revMap["numlock"] = VK_NUMLOCK;
        revMap["win"] = VK_LWIN;
        // 低层钩子给的是左右修饰键变体，文本无法区分左右，统一按左侧变体反查，
        // rebuildHotkeyVkCodes 会把左右变体都加入抑制集合
        revMap["ctrl"] = VK_LCONTROL;
        revMap["shift"] = VK_LSHIFT;
        revMap["alt"] = VK_LMENU;
        revMap["num*"] = VK_MULTIPLY;
        revMap["num+"] = VK_ADD;
        revMap["num,"] = VK_SEPARATOR;
        revMap["num-"] = VK_SUBTRACT;
        revMap["num."] = VK_DECIMAL;
        revMap["num/"] = VK_DIVIDE;
        for (int i = 0; i <= 9; ++i)
            revMap[QString("num%1").arg(i)] = static_cast<DWORD>(VK_NUMPAD0 + i);
        for (int i = 13; i <= 24; ++i)
            revMap[QString("f%1").arg(i)] = static_cast<DWORD>(VK_F1 + i - 1);
    }
    QString lower = keyName.trimmed().toLower();
    return revMap.value(lower, 0);
}

// ==================== 输入追踪 ====================
static std::mutex g_pressedKeysMutex;
static std::set<WORD> g_currentPressedKeys;

void MacroEngine::extractKeyFromInput(const INPUT &input, bool &isDown, WORD &vk)
{
    vk = 0;
    isDown = false;
    if (input.type == INPUT_MOUSE)
    {
        if (input.mi.dwFlags & MOUSEEVENTF_LEFTDOWN)
        {
            vk = VK_LBUTTON;
            isDown = true;
        }
        else if (input.mi.dwFlags & MOUSEEVENTF_LEFTUP)
        {
            vk = VK_LBUTTON;
            isDown = false;
        }
        else if (input.mi.dwFlags & MOUSEEVENTF_RIGHTDOWN)
        {
            vk = VK_RBUTTON;
            isDown = true;
        }
        else if (input.mi.dwFlags & MOUSEEVENTF_RIGHTUP)
        {
            vk = VK_RBUTTON;
            isDown = false;
        }
        else if (input.mi.dwFlags & MOUSEEVENTF_MIDDLEDOWN)
        {
            vk = VK_MBUTTON;
            isDown = true;
        }
        else if (input.mi.dwFlags & MOUSEEVENTF_MIDDLEUP)
        {
            vk = VK_MBUTTON;
            isDown = false;
        }
        else if (input.mi.dwFlags & MOUSEEVENTF_XDOWN)
        {
            // mouseData 低字是 XBUTTON1/2(1/2)，内部记账统一转成 VK_XBUTTON1/2
            vk = (LOWORD(input.mi.mouseData) == XBUTTON2) ? VK_XBUTTON2 : VK_XBUTTON1;
            isDown = true;
        }
        else if (input.mi.dwFlags & MOUSEEVENTF_XUP)
        {
            vk = (LOWORD(input.mi.mouseData) == XBUTTON2) ? VK_XBUTTON2 : VK_XBUTTON1;
            isDown = false;
        }
    }
    else if (input.type == INPUT_KEYBOARD)
    {
        vk = input.ki.wVk;
        isDown = !(input.ki.dwFlags & KEYEVENTF_KEYUP);
    }
}

void MacroEngine::sendTrackedInput(UINT cInputs, LPINPUT pInputs)
{
    // 紧急停止期间不再注入任何输入（s_disableInput 在停止请求时置位，新宏启动时复位）
    if (s_disableInput.load())
        return;

    UINT result = SendInput(cInputs, pInputs, sizeof(INPUT));
    std::lock_guard<std::mutex> lock(g_pressedKeysMutex);
    for (UINT i = 0; i < cInputs; ++i)
    {
        // 只有 SendInput 实际接受的事件才记账，注入失败（UIPI/安全桌面等）不虚报按下状态
        if (i >= result)
            continue;
        bool isDown;
        WORD vk;
        extractKeyFromInput(pInputs[i], isDown, vk);
        if (vk != 0)
        {
            if (isDown)
                g_currentPressedKeys.insert(vk);
            else
                g_currentPressedKeys.erase(vk);
        }
    }
}

void MacroEngine::releaseOnlyPressedKeys()
{
    std::vector<WORD> keysToRelease;
    {
        std::lock_guard<std::mutex> lock(g_pressedKeysMutex);
        if (g_currentPressedKeys.empty())
            return;
        keysToRelease.assign(g_currentPressedKeys.begin(), g_currentPressedKeys.end());
        g_currentPressedKeys.clear();
    }
    std::vector<INPUT> inputs;
    for (WORD vk : keysToRelease)
    {
        if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2)
        {
            INPUT ip = {};
            ip.type = INPUT_MOUSE;
            switch (vk)
            {
            case VK_LBUTTON:
                ip.mi.dwFlags = MOUSEEVENTF_LEFTUP;
                break;
            case VK_RBUTTON:
                ip.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
                break;
            case VK_MBUTTON:
                ip.mi.dwFlags = MOUSEEVENTF_MIDDLEUP;
                break;
            default:
                ip.mi.dwFlags = MOUSEEVENTF_XUP;
                ip.mi.mouseData = vkToXButtonMouseData(vk);
                break;
            }
            inputs.push_back(ip);
        }
        else
        {
            INPUT ip = {};
            ip.type = INPUT_KEYBOARD;
            ip.ki.wVk = vk;
            ip.ki.wScan = MapVirtualKey(vk, MAPVK_VK_TO_VSC);
            ip.ki.dwFlags = KEYEVENTF_KEYUP | KEYEVENTF_SCANCODE;
            if ((vk >= 33 && vk <= 46) ||
                (vk >= 91 && vk <= 93) ||
                vk == VK_DIVIDE || vk == VK_RCONTROL ||
                vk == VK_RMENU || vk == VK_RETURN ||
                vk == VK_INSERT || vk == VK_DELETE ||
                vk == VK_HOME || vk == VK_END ||
                vk == VK_PRIOR || vk == VK_NEXT ||
                vk == VK_LEFT || vk == VK_RIGHT ||
                vk == VK_UP || vk == VK_DOWN ||
                vk == VK_NUMLOCK || vk == VK_CANCEL ||
                vk == VK_SNAPSHOT || vk == VK_DIVIDE)
            {
                ip.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            }
            inputs.push_back(ip);
        }
    }
    if (!inputs.empty())
        SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

// ==================== 持久化 ====================
bool MacroEngine::saveToFile(const QString &filename)
{
    LOG_DEBUG("Saving to: " << filename);
    QJsonObject rootObj;
    rootObj["version"] = 2;
    rootObj["activationMode"] = static_cast<int>(m_activationMode);

    QJsonArray inventoryArray;
    for (const auto &macro : m_macroInventory)
    {
        QJsonObject mObj;
        mObj["name"] = macro.name;
        mObj["triggerKey"] = macro.triggerKey;
        mObj["repeatMode"] = macro.repeatMode;
        mObj["enabled"] = macro.enabled;
        mObj["globalActivation"] = macro.globalActivation;
        mObj["blockActionKeys"] = macro.blockActionKeys;
        QJsonArray stepArr;
        for (const auto &step : macro.steps)
        {
            QJsonObject sObj;
            sObj["type"] = static_cast<int>(step.type);
            sObj["keyOrButton"] = static_cast<int>(step.keyOrButton);
            sObj["scrollDelta"] = step.scrollDelta;
            sObj["delaySec"] = step.delaySec;
            stepArr.append(sObj);
        }
        mObj["steps"] = stepArr;
        inventoryArray.append(mObj);
    }
    rootObj["inventory"] = inventoryArray;
    LOG_DEBUG("  Wrote inventory with " << m_macroInventory.size() << " macros");

    QJsonArray groupArray;
    for (const auto &grp : m_groups)
    {
        QJsonObject grpObj;
        grpObj["name"] = grp.name;
        grpObj["targetExe"] = grp.targetExe;
        grpObj["matchThreshold"] = grp.matchThreshold;
        grpObj["imageTolerancePx"] = grp.imageTolerancePx;
        grpObj["priority"] = grp.priority;
        QJsonArray imgArr;
        for (const auto &img : grp.imagePaths)
            imgArr.append(img);
        grpObj["images"] = imgArr;

        QJsonArray learnArr;
        for (const auto &ld : grp.imageLearnData)
        {
            QJsonObject ldObj;
            ldObj["cx"] = ld.centerX;
            ldObj["cy"] = ld.centerY;
            ldObj["tw"] = ld.tw;
            ldObj["th"] = ld.th;
            learnArr.append(ldObj);
        }
        grpObj["learnData"] = learnArr;

        QJsonArray nameArr;
        for (const auto &n : grp.macroNames)
            nameArr.append(n);
        grpObj["macroNames"] = nameArr;

        grpObj["imageAutoExecute"] = grp.imageAutoExecute;

        groupArray.append(grpObj);
        LOG_DEBUG("  Wrote group '" << grp.name << "' with " << grp.macroNames.size() << " macro refs");
    }

    rootObj["groups"] = groupArray;
    QJsonDocument doc;
    doc.setObject(rootObj);

    // 覆盖前保留上一份配置备份，防止写坏/误覆盖后无法恢复
    if (QFile::exists(filename))
    {
        const QString bakPath = filename + ".bak";
        QFile::remove(bakPath);
        QFile::copy(filename, bakPath);
    }

    // 使用 QSaveFile 原子写入：写入失败时不会破坏已有配置文件
    QSaveFile file(filename);
    if (file.open(QIODevice::WriteOnly))
    {
        const QByteArray payload = doc.toJson();
        if (file.write(payload) == payload.size() && file.commit())
        {
            LOG_DEBUG("Save succeeded, size: " << payload.size());
            emit logMessage("数据已保存");
            return true;
        }
        LOG_DEBUG("Save failed: " << file.errorString());
        emit logMessage("保存失败: " + file.errorString());
        return false;
    }
    LOG_DEBUG("Save failed: " << file.errorString());
    emit logMessage("保存失败: " + file.errorString());
    return false;
}

bool MacroEngine::loadFromFile(const QString &filename)
{
    LOG_DEBUG("Loading from: " << filename);
    QFile file(filename);
    if (!file.exists())
    {
        LOG_DEBUG("File not found");
        return false;
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        LOG_DEBUG("Cannot open file: " << file.errorString());
        return false;
    }

    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);

    int fileVersion = 0;
    if (doc.isObject() && doc.object().contains("version"))
        fileVersion = doc.object()["version"].toInt();

    // 备份内存中的现有数据：解析/版本异常时恢复，避免把已有配置清空后又被演示数据覆盖
    const QVector<Macro> oldInventory = m_macroInventory;
    const QVector<MacroGroup> oldGroups = m_groups;
    const ActivationMode oldActivationMode = m_activationMode;

    if (fileVersion > 2)
    {
        LOG_DEBUG("Unsupported config version " << fileVersion << ", keep existing data");
        return false;
    }

    m_macroInventory.clear();
    m_groups.clear();

    if (fileVersion == 2)
    {
        QJsonObject rootObj = doc.object();
        int actMode = rootObj["activationMode"].toInt(0);
        m_activationMode = (actMode == 1) ? ActivationMode::Group : ActivationMode::Global;

        for (const auto &mVal : rootObj["inventory"].toArray())
        {
            QJsonObject mObj = mVal.toObject();
            Macro macro;
            macro.name = mObj["name"].toString();
            macro.triggerKey = mObj["triggerKey"].toString();
            macro.repeatMode = qBound(0, mObj["repeatMode"].toInt(0), 3);
            macro.enabled = mObj["enabled"].toBool(true);
            macro.globalActivation = mObj["globalActivation"].toBool(false);
            macro.blockActionKeys = mObj["blockActionKeys"].toBool(false);
            for (const auto &sVal : mObj["steps"].toArray())
            {
                QJsonObject sObj = sVal.toObject();
                int stepType = sObj["type"].toInt();
                if (stepType < 0 || stepType > static_cast<int>(StepType::Delay))
                    continue; // 非法步骤类型直接跳过，避免后续 switch 落空
                MacroStep step;
                step.type = static_cast<StepType>(stepType);
                step.keyOrButton = static_cast<DWORD>(sObj["keyOrButton"].toInt());
                step.scrollDelta = sObj["scrollDelta"].toInt();
                step.delaySec = sObj["delaySec"].toDouble();
                macro.steps.append(step);
            }
            m_macroInventory.append(macro);
        }

        for (const auto &gVal : rootObj["groups"].toArray())
        {
            QJsonObject grpObj = gVal.toObject();
            MacroGroup grp;
            grp.name = grpObj["name"].toString();
            grp.targetExe = grpObj["targetExe"].toString();
            grp.matchThreshold = qBound(0.01, grpObj["matchThreshold"].toDouble(0.85), 1.0);
            grp.imageTolerancePx = qBound(0, grpObj["imageTolerancePx"].toInt(50), 500);
            grp.priority = grpObj["priority"].toInt(m_groups.size());
            for (const auto &img : grpObj["images"].toArray())
                grp.imagePaths.append(img.toString());
            for (const auto &ldVal : grpObj["learnData"].toArray())
            {
                QJsonObject ldObj = ldVal.toObject();
                ImageLearnData ld;
                ld.centerX = qBound(-1, ldObj["cx"].toInt(-1), 32767);
                ld.centerY = qBound(-1, ldObj["cy"].toInt(-1), 32767);
                ld.tw = qBound(0, ldObj["tw"].toInt(0), 8192);
                ld.th = qBound(0, ldObj["th"].toInt(0), 8192);
                grp.imageLearnData.append(ld);
            }
            for (const auto &nVal : grpObj["macroNames"].toArray())
                grp.macroNames.append(nVal.toString());
            grp.imageAutoExecute = grpObj["imageAutoExecute"].toBool(false);
            m_groups.append(grp);
        }
        LOG_DEBUG("Loaded v2 format: " << m_macroInventory.size() << " inventory macros, "
                                       << m_groups.size() << " groups");
    }
    else
    {
        // 旧格式迁移
        QJsonArray groupArray;
        if (doc.isObject())
        {
            QJsonObject rootObj = doc.object();
            m_activationMode = static_cast<ActivationMode>(rootObj["activationMode"].toInt(0));
            groupArray = rootObj["groups"].toArray();
        }
        else if (doc.isArray())
        {
            groupArray = doc.array();
            m_activationMode = ActivationMode::Global;
        }
        else
        {
            LOG_DEBUG("JSON format error, keep existing in-memory data");
            m_macroInventory = oldInventory;
            m_groups = oldGroups;
            m_activationMode = oldActivationMode;
            return false;
        }

        LOG_DEBUG("Migrating old format: " << groupArray.size() << " groups");
        int oldGroupIdx = 0;
        for (const auto &gVal : groupArray)
        {
            QJsonObject grpObj = gVal.toObject();
            MacroGroup grp;
            grp.name = grpObj["name"].toString();
            grp.targetExe = grpObj["targetExe"].toString();
            grp.matchThreshold = qBound(0.01, grpObj["matchThreshold"].toDouble(0.85), 1.0);
            grp.imageTolerancePx = qBound(0, grpObj["imageTolerancePx"].toInt(50), 500);
            grp.priority = grpObj["priority"].toInt(oldGroupIdx);
            for (const auto &img : grpObj["images"].toArray())
                grp.imagePaths.append(img.toString());
            for (const auto &ldVal : grpObj["learnData"].toArray())
            {
                QJsonObject ldObj = ldVal.toObject();
                ImageLearnData ld;
                ld.centerX = qBound(-1, ldObj["cx"].toInt(-1), 32767);
                ld.centerY = qBound(-1, ldObj["cy"].toInt(-1), 32767);
                ld.tw = qBound(0, ldObj["tw"].toInt(0), 8192);
                ld.th = qBound(0, ldObj["th"].toInt(0), 8192);
                grp.imageLearnData.append(ld);
            }

            for (const auto &mVal : grpObj["macros"].toArray())
            {
                QJsonObject mObj = mVal.toObject();
                Macro macro;
                macro.name = mObj["name"].toString();
                macro.triggerKey = mObj["triggerKey"].toString();
                macro.repeatMode = qBound(0, mObj["repeatMode"].toInt(0), 3);
                macro.enabled = mObj["enabled"].toBool(true);
                macro.blockActionKeys = mObj["blockActionKeys"].toBool(false);
                for (const auto &sVal : mObj["steps"].toArray())
                {
                    QJsonObject sObj = sVal.toObject();
                    MacroStep step;
                    step.type = static_cast<StepType>(sObj["type"].toInt());
                    step.keyOrButton = static_cast<DWORD>(sObj["keyOrButton"].toInt());
                    step.scrollDelta = sObj["scrollDelta"].toInt();
                    step.delaySec = sObj["delaySec"].toDouble();
                    macro.steps.append(step);
                }
                if (findMacroIndex(macro.name) < 0)
                    m_macroInventory.append(macro);
                grp.macroNames.append(macro.name);
            }
            m_groups.append(grp);
            ++oldGroupIdx;
        }
        LOG_DEBUG("Migration complete: " << m_macroInventory.size() << " inventory macros, "
                                         << m_groups.size() << " groups");
    }

    m_triggerBindings.clear();
    for (const auto &m : m_macroInventory)
        if (!m.triggerKey.isEmpty())
        {
            m_triggerBindings[m.triggerKey] = m.name;
            LOG_DEBUG("  Bind: '" << m.triggerKey << "' -> '" << m.name << "'");
        }

    LOG_DEBUG("Load complete, " << m_groups.size() << " groups, "
                                << m_macroInventory.size() << " inventory, "
                                << m_triggerBindings.size() << " bindings");
    emit dataChanged();
    return true;
}