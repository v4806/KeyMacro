#ifndef MACROENGINE_H
#define MACROENGINE_H

#include <QObject>
#include <QAbstractNativeEventFilter>
#include <QVector>
#include <QMap>
#include <QString>
#include <QSet>
#include <QRect>
#include <QThread>
#include <QElapsedTimer>
#include <QTimer>
#include <Windows.h>
#include <Psapi.h>
#include <opencv2/opencv.hpp>
#include <set>
#include <mutex>
#include <atomic>
#include <memory>
#include <thread>
#include <map>

// 自定义消息，用于主线程异步处理
#define WM_START_NEW_MACRO (WM_USER + 100)
#define WM_STOP_MACRO (WM_USER + 101)

enum class ActivationMode
{
    Global, // 所有宏全局激活
    Group   // 仅当前激活的宏组内的宏可触发
};

enum class StepType
{
    KeyPress,
    KeyRelease,
    MousePress,
    MouseRelease,
    MouseScroll,
    Delay
};

struct MacroStep
{
    StepType type = StepType::Delay;
    DWORD keyOrButton = 0;
    int scrollDelta = 0;
    double delaySec = 0.0;
};

struct Macro
{
    QString name;
    QString triggerKey;
    int repeatMode = 0; // 0-不重复, 1-按住重复, 2-切换, 3-按住执行松开中断
    bool enabled = true;
    bool globalActivation = false;
    bool blockActionKeys = false;
    QVector<MacroStep> steps;
};

// 图像自学习数据（与 imagePaths 一一对应，centerX=-1 表示未学习）
struct ImageLearnData
{
    int centerX = -1; // 模板中心相对窗口左上角的 X 坐标
    int centerY = -1; // 模板中心相对窗口左上角的 Y 坐标
    int tw = 0;       // 模板宽度
    int th = 0;       // 模板高度
};

struct MacroGroup
{
    QString name;
    QString targetExe;
    QStringList imagePaths;                 // 目标图像路径列表
    double matchThreshold = 0.85;           // 图像匹配阈值（0~1）
    int imageTolerancePx = 50;              // 区域匹配容错像素
    int priority = 0;                       // 优先级（0最高）
    QVector<ImageLearnData> imageLearnData; // 自学习位置数据（与 imagePaths 一一对应）
    QStringList macroNames;                 // 仅存储宏名称引用，宏定义在 MacroEngine 的全局库存中
    bool imageAutoExecute = false;          // 检测到目标图像后自动移动鼠标并执行组内宏
};

class MacroEngine : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT

public:
    static MacroEngine *instance();
    ~MacroEngine();

    // ========== 全局宏库存（仓库） ==========
    const QVector<Macro> &macroInventory() const;
    int addToInventory(const Macro &macro);        // 返回添加后的索引
    bool removeFromInventory(const QString &name); // 从库存删除，同时清理所有组的引用
    Macro *findMutableMacro(const QString &name);
    const Macro *findMacro(const QString &name) const;
    int findMacroIndex(const QString &name) const;
    void updateInInventory(const QString &oldName, const Macro &newMacro); // 更新库存中的宏

    const QVector<MacroGroup> &groups() const;
    void addGroup(const MacroGroup &grp);
    void removeGroup(int index);
    MacroGroup *mutableGroup(int index);

    Macro currentMacro() const;
    void setCurrentMacro(const Macro &macro);

    void startRecording();
    void stopRecording();
    bool isRecording() const;

    // 设置录制排除区域（屏幕坐标），此区域内的鼠标点击不会被录制
    void setRecordExcludeRegion(const QRect &globalRect);
    void clearRecordExcludeRegion();

    void startBindRecording();
    void stopBindRecording();
    bool isBindRecording() const;

    void playMacro(const Macro &macro);
    void stopPlayback();

    bool saveToFile(const QString &filename);
    bool loadFromFile(const QString &filename);
    // 配置文件存在但解析失败/版本不支持（避免 UI 用演示数据覆盖原文件）
    bool configLoadFailed() const { return m_configLoadFailed; }

    QString keyToString(DWORD vk, bool isMouse = false, int mouseData = 0) const;
    QString mouseButtonToString(WORD button) const;
    // 将按键名称（如 "a", "esc", "f1"）转换为 VK 码，返回 0 表示无法转换
    DWORD keyNameToVkCode(const QString &keyName) const;

    // 重建热键VK码集合（仅含激活宏，当绑定/激活状态变化时调用）
    void rebuildHotkeyVkCodes();
    // 按需启动/停止钩子
    void updateHooksState();

    void finishBindRecording();
    void cancelBindRecording();

    // ========== 宏组激活相关 ==========
    ActivationMode activationMode() const;
    void setActivationMode(ActivationMode mode);

    int activeGroupIndex() const;
    QSet<int> activeGroupIndices() const;
    QString currentForegroundProcess() const;

    // 判断指定宏是否应该处于激活状态（可触发和执行）
    bool isMacroActive(int groupIndex, int macroIndex) const;
    // 判断宏名称是否处于激活状态（遍历所有组）
    bool isMacroActiveByName(const QString &macroName) const;

    // 宏跨组移动
    void moveMacroToGroup(int fromGroup, int macroIdx, int toGroup);
    void copyMacroToGroup(int fromGroup, int macroIdx, int toGroup);

    // 获取当前前台窗口对应的激活组索引，-1 表示无匹配
    int foregroundActiveGroupIndex() const;

    // 前台窗口轮询（由 QTimer 驱动）
    void checkForegroundWindow();

    // ========== 图像识别（照搬自 OpenCV_Clicker，加速版） ==========
    // 截取整个虚拟屏幕，返回 BGR 格式 cv::Mat（保留，供参考）
    static cv::Mat captureScreen();
    // 截取前台窗口区域，返回灰度 cv::Mat（毫秒级加速：只截窗口 + 单通道）
    static cv::Mat captureForegroundGray();
    // 对指定宏组进行目标图像匹配（使用预截取的灰度屏幕），任意一张匹配即返回 true
    // learnData: 自学习数据（可读写），tolerancePx: 区域匹配容错像素
    static bool matchGroupImages(const QStringList &imagePaths, double threshold,
                                 const cv::Mat &screenGray,
                                 QVector<ImageLearnData> &learnData,
                                 int tolerancePx);

    // ========== 后台图像匹配线程 ==========
    void startImageMatchWorker();
    void stopImageMatchWorker();
    void rebuildImageCheckItems();
    void startImageMatchForGroup(int groupIndex);
    void stopImageMatchForGroup(int groupIndex);
    void stopAllImageMatch();
    bool getImageMatchResult(int groupIndex) const;

    // 自学习回调（由后台线程通过 invokeMethod 调用）
    Q_INVOKABLE void onImageLearned(int groupIndex, int imageIndex, int cx, int cy, int tw, int th);
    Q_INVOKABLE void onImageMatched(int groupIndex);
    Q_INVOKABLE void onImageLost(int groupIndex);

    Q_INVOKABLE void clearBlockedActionKeys();

    // ========== 图像自动执行（移动鼠标+自动执行组内宏） ==========
    void startAutoExecute(int groupIndex, int screenCenterX, int screenCenterY);
    void stopAutoExecute();
    bool isAutoExecuting() const { return m_autoExecuting.load(); }

signals:
    void logMessage(const QString &msg);
    void recordingStarted();
    void recordingStopped();
    void stepRecorded(const MacroStep &step);
    void bindRecordingStarted();
    void triggerKeySet(const QString &key);
    void playbackStarted(const QString &macroName);
    void playbackFinished(const QString &macroName);
    void dataChanged();
    void activeGroupActivated(int groupIndex);
    void activeGroupDeactivated(int groupIndex);
    void foregroundProcessChanged(const QString &processName);
    void activationModeChanged(ActivationMode mode);

protected:
    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

private:
    explicit MacroEngine(QObject *parent = nullptr);

    void installHooks();
    void uninstallHooks();

    // 接收拷贝的值，不再使用指针
    Q_INVOKABLE void processKeyEvent(WPARAM wParam, DWORD vkCode, DWORD scanCode, DWORD flags, DWORD time);
    Q_INVOKABLE void processMouseEvent(WPARAM wParam, LONG ptX, LONG ptY, DWORD mouseData, DWORD flags, DWORD time);

    static void sendTrackedInput(UINT cInputs, LPINPUT pInputs);
    static void releaseOnlyPressedKeys();
    static void extractKeyFromInput(const INPUT &input, bool &isDown, WORD &vk);

    HHOOK m_keyboardHook = nullptr;
    HHOOK m_mouseHook = nullptr;
    DWORD m_mainThreadId = 0;

    bool m_recording = false;
    bool m_bindRecording = false;
    QVector<QString> m_bindPressedKeyNames; // 按顺序记录的键名
    QSet<DWORD> m_bindPressedVKs;           // 当前按下的 VK 码（键盘+鼠标）
    QSet<DWORD> m_physicalPressedVks;       // 当前物理按下的 VK（用于组合键热键匹配，仅主线程访问）
    QRect m_recordExcludeRegion;            // 录制时排除的屏幕区域（空rect=不排除）
    QVector<MacroStep> m_recordedSteps;
    QElapsedTimer m_recordTimer;
    QVector<qint64> m_stepTimestamps;

    QVector<Macro> m_macroInventory; // 全局宏库存
    QVector<MacroGroup> m_groups;
    Macro m_currentMacro;
    bool m_configLoadFailed = false; // 配置文件存在但加载失败

    QMap<QString, QString> m_triggerBindings;

    // 线程安全的热键VK码集合，用于快速检查（仅含激活宏的热键）
    std::set<DWORD> m_hotkeyVkCodes;
    std::set<DWORD> m_activeMouseVkCodes;        // 鼠标热键VK码（VK_LBUTTON等）
    std::set<DWORD> m_pressedHotkeyVks;          // 已按下且已触发宏的热键VK（防自动重复级联）
    std::set<DWORD> m_suppressedMouseVkButtons;  // 当前被钩子拦截的鼠标按键
    std::set<DWORD> m_suppressedKeyboardVkCodes; // 当前被钩子拦截的键盘按键
    mutable std::mutex m_hotkeyVkMutex;

    // 屏蔽动作键位：宏运行时屏蔽步骤序列中已有键位的真实键盘输入
    std::atomic<bool> m_blockActionKeys{false};
    std::set<DWORD> m_blockedActionKeyVkCodes;
    mutable std::mutex m_blockedActionKeyMutex;
    QMap<QString, QString> m_activeTriggeredByKey;

    std::shared_ptr<std::atomic_bool> m_playStopFlag;

    // ========== 宏组激活状态 ==========
    ActivationMode m_activationMode = ActivationMode::Group;
    QSet<int> m_activeGroupIndices;       // 当前激活的组索引集合
    QSet<int> m_pendingImageGroupIndices; // 正在图像识别的组索引集合
    QSet<int> m_previousActiveIndices;    // 上一次 checkForegroundWindow 时的激活组
    QSet<int> m_previousPendingIndices;   // 上一次 checkForegroundWindow 时的挂起组
    QString m_foregroundProcess;          // 当前前台进程名
    QTimer *m_foregroundTimer = nullptr;

    bool m_hookInstalled = false; // 钩子是否已安装

    // ========== 前台窗口检测 ==========
    QString getProcessNameFromWindow(HWND hwnd) const;

    // ========== 后台图像匹配线程数据 ==========
    struct ImageCheckItem
    {
        int groupIndex;
        QString targetExe; // 组目标进程名快照（避免后台线程无锁读取主线程容器 m_groups）
        QStringList imagePaths;
        double matchThreshold;
        int imageTolerancePx = 50;
        QVector<ImageLearnData> imageLearnData;
    };
    std::vector<ImageCheckItem> m_imageCheckItems;
    mutable std::mutex m_imageCheckMutex;
    mutable std::condition_variable m_imageCv;
    std::set<int> m_pendingMatchStateReset; // 需要重置 previousMatchState 的组索引
    std::map<int, bool> m_imageMatchResults;
    mutable std::mutex m_imageResultMutex;
    std::thread m_imageMatchWorker;
    std::atomic<bool> m_imageMatchStop{false};
    void imageMatchWorkerFunc();

    // ========== 世代机制与重复模式支持 ==========
    static std::atomic<uint64_t> s_macroGeneration;
    static std::atomic<bool> s_isHotkeyPressed;
    static std::atomic<bool> s_stopRequested;
    static std::atomic<int> s_activeMacroThreads;
    static std::atomic<bool> s_disableInput; // 紧急禁止宏模拟输入

    // 当前正在运行的宏信息
    std::atomic<uint64_t> m_currentGeneration{0};
    std::atomic<bool> m_isRepeatMode{false};

    // ========== 图像自动执行 ==========
    std::atomic<bool> m_autoExecuting{false};
    std::atomic<bool> m_autoExecuteStop{false};
    std::atomic<int> m_autoExecuteGroupIndex{-1}; // 当前自动执行来源组（用于只响应本组的图像丢失）
    std::atomic<int> m_mouseLockCenterX{0};
    std::atomic<int> m_mouseLockCenterY{0};
    std::thread m_autoExecuteThread;
    void autoExecuteThreadFunc(int groupIndex, uint64_t generation);
    std::atomic<uint64_t> m_autoExecuteGeneration{0};
    QTimer *m_mouseLockTimer = nullptr;

    // 宏线程主函数
    static void macroThreadFunc(const QVector<MacroStep> &steps, uint64_t myGeneration, bool isRepeatMode, bool breakAfterOneCycle = false);

    // 主线程处理新宏请求
    void requestNewMacroOnMainThread(const Macro &macro);
    // 主线程处理停止宏请求
    void requestStopMacroOnMainThread();

    // 处理宏触发（整合按住重复和按一下重复）
    void triggerMacro(const Macro &macro);

    static LRESULT CALLBACK keyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK mouseHookProc(int nCode, WPARAM wParam, LPARAM lParam);

    static MacroEngine *s_instance;
};

#endif // MACROENGINE_H