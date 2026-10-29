#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>
#include <QListView>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMessageBox>
#include <QGroupBox>
#include <QCheckBox>
#include <QTextEdit>
#include <QLineEdit>
#include <QFileDialog>
#include <QInputDialog>
#include <QAbstractItemView>
#include <QColor>
#include <QString>
#include <QSet>
#include <QVector>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QMouseEvent>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QSettings>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include <QProcess>
#include <QCloseEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include "MacroEngine.h"

#include <QDebug>
#include <QTime>
#include <iostream>

// ====================== VS Code 集成终端日志输出 ======================
// 将所有 qDebug 输出同步到 Windows 调试输出流（VS Code 终端可捕获）
#ifdef Q_OS_WIN
#include <windows.h>
static void vsCodeLogHandler(QtMsgType, const QMessageLogContext &, const QString &msg)
{
    QByteArray utf8 = msg.toUtf8();
    OutputDebugStringA(utf8.constData());
    OutputDebugStringA("\n");
    // 同时保留标准 stderr 输出
    fprintf(stderr, "%s\n", utf8.constData());
    fflush(stderr);
}
#endif

// 获取带毫秒的时间戳字符串
inline QString getTimestamp()
{
    return QTime::currentTime().toString("hh:mm:ss.zzz");
}

#ifdef _DEBUG
#define UI_LOG(msg) qDebug().noquote().nospace() << "[" << getTimestamp() << "][UI] " << msg
#else
#define UI_LOG(msg) ((void)0)
#endif

class TriggerCaptureDialog;
class GroupEditDialog;

// ================== 从文本中提取延迟值 ==================
static double extractDelayFromText(const QString &text)
{
    QString numStr;
    bool inNumber = false;
    for (int i = 0; i < text.length(); ++i)
    {
        QChar c = text[i];
        if ((c.isDigit() || c == '.') && text.left(i).contains("⌛"))
        {
            numStr += c;
            inNumber = true;
        }
        else if (inNumber && !c.isDigit() && c != '.')
        {
            break;
        }
    }
    bool ok;
    double val = numStr.toDouble(&ok);
    return ok ? val : 0.05;
}

// ================== 延迟编辑Delegate（带删除按钮） ==================
class DelayEditDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit DelayEditDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    ~DelayEditDelegate() override = default;

    QListWidget *findListWidget(QObject *obj) const
    {
        if (!obj)
            return nullptr;
        if (auto *lw = qobject_cast<QListWidget *>(obj))
            return lw;
        return findListWidget(obj->parent());
    }

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const override
    {
        Q_UNUSED(option);
        if (!index.isValid())
            return nullptr;

        auto *listWidget = qobject_cast<QListWidget *>(parent->parent());
        if (!listWidget)
            return nullptr;

        auto *item = listWidget->item(index.row());
        if (!item)
            return nullptr;

        QString text = item->text();
        if (text.startsWith("⌛"))
        {
            auto *editor = new QDoubleSpinBox(parent);
            editor->setRange(0.001, 3600.0);
            editor->setDecimals(3);
            editor->setSuffix(" 秒");
            editor->setSingleStep(0.01);
            editor->setValue(extractDelayFromText(text));
            editor->setKeyboardTracking(false);

            int currentRow = index.row();

            QObject::connect(editor, &QDoubleSpinBox::editingFinished, editor, [listWidget, currentRow]()
                             { QMetaObject::invokeMethod(QCoreApplication::instance(), [listWidget, currentRow]()
                                                         {
                    auto* item = listWidget->item(currentRow);
                    if (item)
                        listWidget->closePersistentEditor(item); }, Qt::QueuedConnection); });

            return editor;
        }
        return nullptr;
    }

    void setModelData(QWidget *editor, QAbstractItemModel *model,
                      const QModelIndex &index) const override
    {
        if (!index.isValid())
            return;

        auto *listWidget = findListWidget(editor->parent());
        if (!listWidget)
            return;

        auto *spinBox = qobject_cast<QDoubleSpinBox *>(editor);
        if (!spinBox)
            return;

        double value = spinBox->value();
        auto *item = listWidget->item(index.row());
        if (!item)
            return;

        item->setText(QString("⌛ %1 秒").arg(value, 0, 'f', 3));
        emit listWidget->itemChanged(item);
    }

    void updateEditorGeometry(QWidget *editor, const QStyleOptionViewItem &option,
                              const QModelIndex &index) const override
    {
        Q_UNUSED(index);
        editor->setGeometry(option.rect);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        // 根据文字中的符号设置颜色：▼红色，▲绿色
        QStyleOptionViewItem opt = option;
        QString text = index.data(Qt::DisplayRole).toString();
        if (text.contains(QStringLiteral("▼")))
        {
            opt.palette.setColor(QPalette::Text, QColor(255, 80, 80));
            opt.palette.setColor(QPalette::HighlightedText, QColor(255, 80, 80));
        }
        else if (text.contains(QStringLiteral("▲")))
        {
            opt.palette.setColor(QPalette::Text, QColor(0, 200, 0)); // 亮绿色
            opt.palette.setColor(QPalette::HighlightedText, QColor(0, 200, 0));
        }

        // 绘制文本（使用修改后的调色板）
        QStyledItemDelegate::paint(painter, opt, index);

        if (!index.isValid())
            return;

        const int btnWidth = 24;
        QRect itemRect = option.rect;
        QRect btnRect(itemRect.right() - btnWidth - 2, itemRect.top() + 2,
                      btnWidth, itemRect.height() - 4);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        if (option.state & QStyle::State_MouseOver)
        {
            QColor bgColor(220, 60, 60, 40);
            painter->setBrush(bgColor);
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(btnRect.adjusted(2, 2, -2, -2), 3, 3);
        }

        QColor iconColor(180, 40, 40);
        painter->setPen(iconColor);
        QFont font = painter->font();
        font.setPointSize(10);
        font.setBold(true);
        painter->setFont(font);
        painter->drawText(btnRect, Qt::AlignCenter, QStringLiteral("×"));

        painter->restore();
    }

    // 处理鼠标事件，检测 × 按钮点击
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (!index.isValid())
            return QStyledItemDelegate::editorEvent(event, model, option, index);

        if (event->type() == QEvent::MouseButtonRelease)
        {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            // 只响应左键：右键落在 × 热区时不能误删条目（列表同时提供右键菜单）
            if (mouseEvent->button() != Qt::LeftButton)
                return QStyledItemDelegate::editorEvent(event, model, option, index);
            const int btnWidth = 24;
            QRect itemRect = option.rect;
            QRect btnRect(itemRect.right() - btnWidth - 2, itemRect.top() + 2,
                          btnWidth, itemRect.height() - 4);

            if (btnRect.contains(mouseEvent->pos()))
            {
                emit deleteClicked(index.row());
                return true; // 事件已处理
            }
        }

        // 其他事件（双击等）交由默认处理
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

signals:
    // 当点击删除按钮时发出，row 为步骤索引
    void deleteClicked(int row);
};

// ================== 带删除按钮的列表Delegate（用于图像列表） ==================
class ImageDeleteDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ImageDeleteDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override
    {
        QStyledItemDelegate::paint(painter, option, index);

        if (!index.isValid())
            return;

        const int btnWidth = 24;
        QRect itemRect = option.rect;
        QRect btnRect(itemRect.right() - btnWidth - 2, itemRect.top() + 2,
                      btnWidth, itemRect.height() - 4);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        if (option.state & QStyle::State_MouseOver)
        {
            QColor bgColor(220, 60, 60, 40);
            painter->setBrush(bgColor);
            painter->setPen(Qt::NoPen);
            painter->drawRoundedRect(btnRect.adjusted(2, 2, -2, -2), 3, 3);
        }

        QColor iconColor(180, 40, 40);
        painter->setPen(iconColor);
        QFont font = painter->font();
        font.setPointSize(10);
        font.setBold(true);
        painter->setFont(font);
        painter->drawText(btnRect, Qt::AlignCenter, QStringLiteral("×"));

        painter->restore();
    }

    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                     const QStyleOptionViewItem &option,
                     const QModelIndex &index) override
    {
        if (!index.isValid())
            return QStyledItemDelegate::editorEvent(event, model, option, index);

        if (event->type() == QEvent::MouseButtonRelease)
        {
            auto *mouseEvent = static_cast<QMouseEvent *>(event);
            // 只响应左键：右键落在 × 热区时不能误删条目（列表同时提供右键菜单）
            if (mouseEvent->button() != Qt::LeftButton)
                return QStyledItemDelegate::editorEvent(event, model, option, index);
            const int btnWidth = 24;
            QRect itemRect = option.rect;
            QRect btnRect(itemRect.right() - btnWidth - 2, itemRect.top() + 2,
                          btnWidth, itemRect.height() - 4);

            if (btnRect.contains(mouseEvent->pos()))
            {
                emit deleteClicked(index.row());
                return true;
            }
        }

        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

signals:
    void deleteClicked(int row);
};

// ================== 键盘布局控件 ==================
class KeyboardWidget : public QWidget
{
    Q_OBJECT
public:
    explicit KeyboardWidget(QWidget *parent = nullptr) : QWidget(parent)
    {
        setFixedSize(820, 700);
        initUi();
    }

signals:
    void keyClicked(const QString &key);

private:
    void initUi()
    {
        auto *mainLayout = new QVBoxLayout(this);
        mainLayout->setSpacing(6);
        mainLayout->setContentsMargins(4, 4, 4, 4);

        auto *rowFunc = new QHBoxLayout();
        rowFunc->setSpacing(2);
        QStringList funcKeys = {"esc", "f1", "f2", "f3", "f4", "f5", "f6", "f7",
                                "f8", "f9", "f10", "f11", "f12"};
        for (const auto &key : funcKeys)
        {
            auto *btn = new QPushButton(key.toUpper());
            btn->setFixedSize(48, 30);
            connect(btn, &QPushButton::clicked, this, [this, key]()
                    { emit keyClicked(key); });
            rowFunc->addWidget(btn);
        }
        rowFunc->addStretch();
        mainLayout->addLayout(rowFunc);

        auto *rowNum = new QHBoxLayout();
        rowNum->setSpacing(2);
        QStringList numKeys = {"`", "1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "-", "="};
        for (const auto &k : numKeys)
        {
            auto *btn = new QPushButton(k);
            btn->setFixedSize(48, 30);
            connect(btn, &QPushButton::clicked, this, [this, k]()
                    { emit keyClicked(k); });
            rowNum->addWidget(btn);
        }
        auto *btnBs = new QPushButton("⌫");
        btnBs->setFixedSize(90, 30);
        connect(btnBs, &QPushButton::clicked, this, [this]()
                { emit keyClicked("backspace"); });
        rowNum->addWidget(btnBs);
        mainLayout->addLayout(rowNum);

        auto *rowQ1 = new QHBoxLayout();
        rowQ1->setSpacing(2);
        struct KeyInfo
        {
            QString label;
            QString key;
            int width;
        };
        QList<KeyInfo> q1Keys = {
            {"Tab", "tab", 75}, {"Q", "q", 48}, {"W", "w", 48}, {"E", "e", 48}, {"R", "r", 48}, {"T", "t", 48}, {"Y", "y", 48}, {"U", "u", 48}, {"I", "i", 48}, {"O", "o", 48}, {"P", "p", 48}, {"[", "[", 48}, {"]", "]", 48}, {"\\", "\\", 75}};
        for (const auto &info : q1Keys)
        {
            auto *btn = new QPushButton(info.label);
            btn->setFixedSize(info.width, 30);
            connect(btn, &QPushButton::clicked, this, [this, key = info.key]()
                    { emit keyClicked(key); });
            rowQ1->addWidget(btn);
        }
        mainLayout->addLayout(rowQ1);

        auto *rowQ2 = new QHBoxLayout();
        rowQ2->setSpacing(2);
        QList<KeyInfo> q2Keys = {
            {"Caps", "caps lock", 85}, {"A", "a", 48}, {"S", "s", 48}, {"D", "d", 48}, {"F", "f", 48}, {"G", "g", 48}, {"H", "h", 48}, {"J", "j", 48}, {"K", "k", 48}, {"L", "l", 48}, {";", ";", 48}, {"'", "'", 48}, {"⏎", "enter", 95}};
        for (const auto &info : q2Keys)
        {
            auto *btn = new QPushButton(info.label);
            btn->setFixedSize(info.width, 30);
            connect(btn, &QPushButton::clicked, this, [this, key = info.key]()
                    { emit keyClicked(key); });
            rowQ2->addWidget(btn);
        }
        mainLayout->addLayout(rowQ2);

        auto *rowQ3 = new QHBoxLayout();
        rowQ3->setSpacing(2);
        QList<KeyInfo> q3Keys = {
            {"Shift", "shift", 105}, {"Z", "z", 48}, {"X", "x", 48}, {"C", "c", 48}, {"V", "v", 48}, {"B", "b", 48}, {"N", "n", 48}, {"M", "m", 48}, {",", ",", 48}, {".", ".", 48}, {"/", "/", 48}, {"Shift", "shift", 115}};
        for (const auto &info : q3Keys)
        {
            auto *btn = new QPushButton(info.label);
            btn->setFixedSize(info.width, 30);
            connect(btn, &QPushButton::clicked, this, [this, key = info.key]()
                    { emit keyClicked(key); });
            rowQ3->addWidget(btn);
        }
        mainLayout->addLayout(rowQ3);

        auto *rowMod = new QHBoxLayout();
        rowMod->setSpacing(2);
        QList<KeyInfo> modKeys = {
            {"Ctrl", "ctrl", 75}, {"Win", "left windows", 75}, {"Alt", "alt", 75}, {"Space", "space", 250}, {"AltGr", "alt gr", 75}, {"Win", "right windows", 75}, {"Menu", "menu", 75}, {"Ctrl", "right ctrl", 75}};
        for (const auto &info : modKeys)
        {
            auto *btn = new QPushButton(info.label);
            btn->setFixedSize(info.width, 30);
            connect(btn, &QPushButton::clicked, this, [this, key = info.key]()
                    { emit keyClicked(key); });
            rowMod->addWidget(btn);
        }
        mainLayout->addLayout(rowMod);

        auto *lower = new QHBoxLayout();
        lower->setSpacing(15);

        auto *leftColumn = new QVBoxLayout();
        leftColumn->setSpacing(0);

        auto *editArrowsWidget = new QWidget();
        editArrowsWidget->setFixedWidth(175);
        auto *editArrows = new QVBoxLayout(editArrowsWidget);
        editArrows->setSpacing(4);
        editArrows->setContentsMargins(0, 0, 0, 0);

        auto *edit1 = new QHBoxLayout();
        edit1->setSpacing(0);
        edit1->setContentsMargins(0, 0, 0, 0);
        struct
        {
            QString key;
            QString label;
        } editLabels1[] = {
            {"insert", "Ins"}, {"home", "HM"}, {"page up", "PgUp"}};
        for (const auto &e : editLabels1)
        {
            auto *btn = new QPushButton(e.label);
            btn->setFixedSize(55, 30);
            connect(btn, &QPushButton::clicked, this, [this, key = e.key]()
                    { emit keyClicked(key); });
            edit1->addWidget(btn);
        }
        editArrows->addLayout(edit1);

        auto *edit2 = new QHBoxLayout();
        edit2->setSpacing(0);
        edit2->setContentsMargins(0, 0, 0, 0);
        struct
        {
            QString key;
            QString label;
        } editLabels2[] = {
            {"delete", "Del"}, {"end", "End"}, {"page down", "PgDn"}};
        for (const auto &e : editLabels2)
        {
            auto *btn = new QPushButton(e.label);
            btn->setFixedSize(55, 30);
            connect(btn, &QPushButton::clicked, this, [this, key = e.key]()
                    { emit keyClicked(key); });
            edit2->addWidget(btn);
        }
        editArrows->addLayout(edit2);

        auto *arrowsContainer = new QVBoxLayout();
        arrowsContainer->setSpacing(2);
        arrowsContainer->setContentsMargins(0, 0, 0, 0);

        auto *upRow = new QHBoxLayout();
        upRow->setSpacing(0);
        upRow->setContentsMargins(0, 0, 0, 0);
        upRow->addStretch();
        upRow->addWidget(makeButton("↑", "up", 55, 30));
        upRow->addStretch();
        arrowsContainer->addLayout(upRow);

        auto *lrRow = new QHBoxLayout();
        lrRow->setSpacing(0);
        lrRow->setContentsMargins(0, 0, 0, 0);
        lrRow->addWidget(makeButton("←", "left", 55, 30));
        lrRow->addWidget(makeButton("↓", "down", 55, 30));
        lrRow->addWidget(makeButton("→", "right", 55, 30));
        arrowsContainer->addLayout(lrRow);

        editArrows->addLayout(arrowsContainer);
        leftColumn->addWidget(editArrowsWidget);
        lower->addLayout(leftColumn);

        auto *numpadColumn = new QVBoxLayout();
        numpadColumn->setSpacing(4);

        auto *sysRow = new QHBoxLayout();
        sysRow->setSpacing(2);
        struct
        {
            QString label;
            QString key;
        } sysKeys[] = {
            {"PrtSc", "print screen"}, {"ScrLk", "scroll lock"}, {"Pause", "pause"}};
        for (const auto &sk : sysKeys)
        {
            auto *btn = new QPushButton(sk.label);
            btn->setFixedSize(55, 30);
            connect(btn, &QPushButton::clicked, this, [this, key = sk.key]()
                    { emit keyClicked(key); });
            sysRow->addWidget(btn);
        }
        sysRow->addStretch();
        numpadColumn->addLayout(sysRow);

        auto *numpadLayout = new QGridLayout();
        numpadLayout->setSpacing(2);
        struct NumpadKey
        {
            QString label;
            QString key;
            int row;
            int col;
        };
        QList<NumpadKey> numpadKeys = {
            {"num\nlock", "num lock", 0, 0}, {"/", "num /", 0, 1}, {"*", "num *", 0, 2}, {"-", "num -", 0, 3}, {"7", "num7", 1, 0}, {"8", "num8", 1, 1}, {"9", "num9", 1, 2}, {"4", "num4", 2, 0}, {"5", "num5", 2, 1}, {"6", "num6", 2, 2}, {"1", "num1", 3, 0}, {"2", "num2", 3, 1}, {"3", "num3", 3, 2}, {".", "num .", 4, 2}};
        for (const auto &nk : numpadKeys)
        {
            auto *btn = new QPushButton(nk.label);
            btn->setFixedSize(45, 30);
            connect(btn, &QPushButton::clicked, this, [this, key = nk.key]()
                    { emit keyClicked(key); });
            numpadLayout->addWidget(btn, nk.row, nk.col);
        }

        auto *btn0 = new QPushButton("0");
        btn0->setFixedSize(90, 30);
        connect(btn0, &QPushButton::clicked, this, [this]()
                { emit keyClicked("num0"); });
        numpadLayout->addWidget(btn0, 4, 0, 1, 2);

        auto *btnPlus = new QPushButton("+");
        btnPlus->setFixedSize(45, 60);
        connect(btnPlus, &QPushButton::clicked, this, [this]()
                { emit keyClicked("num +"); });
        numpadLayout->addWidget(btnPlus, 1, 3, 2, 1);

        auto *btnEnter = new QPushButton("⏎");
        btnEnter->setFixedSize(45, 60);
        connect(btnEnter, &QPushButton::clicked, this, [this]()
                { emit keyClicked("num enter"); });
        numpadLayout->addWidget(btnEnter, 3, 3, 2, 1);

        numpadColumn->addLayout(numpadLayout);

        auto *numpadContainer = new QWidget();
        numpadContainer->setFixedWidth(190);
        numpadContainer->setLayout(numpadColumn);
        lower->addWidget(numpadContainer);

        mainLayout->addLayout(lower);

        mainLayout->addSpacing(10);
        auto *mouseRow = new QHBoxLayout();
        mouseRow->setSpacing(4);
        mouseRow->addWidget(new QLabel("鼠标按键："));
        struct
        {
            QString label;
            QString value;
        } mouseBtns[] = {
            {"左键", "left"}, {"右键", "right"}, {"中键", "middle"}, {"第四键", "x1"}, {"第五键", "x2"}, {"滚轮↑", "wheel_up"}, {"滚轮↓", "wheel_down"}};
        for (const auto &mb : mouseBtns)
        {
            auto *btn = new QPushButton(mb.label);
            btn->setFixedSize(65, 30);
            connect(btn, &QPushButton::clicked, this, [this, val = mb.value]()
                    { emit keyClicked(val); });
            mouseRow->addWidget(btn);
        }
        mouseRow->addStretch();
        mainLayout->addLayout(mouseRow);

        mainLayout->addStretch();
    }

    QPushButton *makeButton(const QString &text, const QString &key, int w, int h)
    {
        auto *btn = new QPushButton(text);
        btn->setFixedSize(w, h);
        connect(btn, &QPushButton::clicked, this, [this, key]()
                { emit keyClicked(key); });
        return btn;
    }
};

// ================== 带分类和拖拽支持的 QListWidget ==================
class CategoryListWidget : public QListWidget
{
    Q_OBJECT
public:
    explicit CategoryListWidget(QWidget *parent = nullptr) : QListWidget(parent)
    {
        setDragDropMode(QAbstractItemView::InternalMove);
        setDefaultDropAction(Qt::MoveAction);
        setDragEnabled(true);
    }

    // 判断一个 item 是否为分类头（不可拖拽）
    static bool isHeader(QListWidgetItem *item)
    {
        if (!item)
            return false;
        QVariant v = item->data(Qt::UserRole + 1);
        return v.isValid() && !item->data(Qt::UserRole).isValid();
    }

    void setupItemFlags(QListWidgetItem *item)
    {
        if (!item)
            return;
        if (isHeader(item))
        {
            item->setFlags(Qt::ItemIsEnabled);
        }
        else
        {
            item->setFlags(item->flags() | Qt::ItemIsDragEnabled | Qt::ItemIsEditable);
        }
    }

signals:
    void itemsReordered();

protected:
    void dropEvent(QDropEvent *event) override
    {
        QListWidget::dropEvent(event);
        emit itemsReordered();
    }
};

class TriggerCaptureDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TriggerCaptureDialog(QWidget *parent = nullptr) : QDialog(parent)
    {
        setWindowTitle("等待按键...");
        setFixedSize(200, 80);
    }
};

// ================== image 文件夹图片选择对话框（缩略图+文件名） ==================
class ImageFolderPickerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ImageFolderPickerDialog(QWidget *parent = nullptr) : QDialog(parent)
    {
        setWindowTitle("从 image 文件夹选择图片");
        setMinimumSize(500, 400);

        auto *layout = new QVBoxLayout(this);

        m_listWidget = new QListWidget();
        m_listWidget->setIconSize(QSize(64, 64));
        m_listWidget->setViewMode(QListView::IconMode);
        m_listWidget->setResizeMode(QListView::Adjust);
        m_listWidget->setMovement(QListView::Static);
        m_listWidget->setSpacing(8);
        layout->addWidget(m_listWidget);

        // 加载 image 文件夹中的图片
        QString imageDir = QCoreApplication::applicationDirPath() + "/image";
        QDir dir(imageDir);
        if (dir.exists())
        {
            QStringList filters;
            filters << "*.png" << "*.jpg" << "*.jpeg" << "*.bmp" << "*.gif";
            QFileInfoList files = dir.entryInfoList(filters, QDir::Files, QDir::Name);
            for (const QFileInfo &fi : files)
            {
                auto *item = new QListWidgetItem();
                QPixmap pixmap(fi.absoluteFilePath());
                if (!pixmap.isNull())
                {
                    item->setIcon(QIcon(pixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                }
                item->setText(fi.fileName());
                item->setData(Qt::UserRole, fi.fileName());
                item->setTextAlignment(Qt::AlignCenter);
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                m_listWidget->addItem(item);
            }
        }

        // 双击选中
        connect(m_listWidget, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item)
                {
                    m_selectedFile = item->data(Qt::UserRole).toString();
                    accept(); });

        auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(btnBox, &QDialogButtonBox::accepted, this, [this]()
                {
                    auto *item = m_listWidget->currentItem();
                    if (item)
                    {
                        m_selectedFile = item->data(Qt::UserRole).toString();
                        accept();
                    } });
        connect(btnBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(btnBox);
    }

    QString selectedFile() const { return m_selectedFile; }

private:
    QListWidget *m_listWidget;
    QString m_selectedFile;
};

class GroupEditDialog : public QDialog
{
    Q_OBJECT
public:
    explicit GroupEditDialog(MacroEngine *engine, int groupIndex, QWidget *parent = nullptr)
        : QDialog(parent), m_engine(engine), m_groupIndex(groupIndex)
    {
        setWindowTitle("编辑宏组");
        setMinimumSize(500, 500);

        auto *layout = new QVBoxLayout(this);

        // 组名称
        auto *nameLayout = new QHBoxLayout();
        nameLayout->addWidget(new QLabel("组名称:"));
        m_nameEdit = new QLineEdit();
        nameLayout->addWidget(m_nameEdit);
        layout->addLayout(nameLayout);

        // 目标程序
        auto *exeLayout = new QHBoxLayout();
        exeLayout->addWidget(new QLabel("目标程序:"));
        m_targetExeEdit = new QLineEdit();
        m_targetExeEdit->setPlaceholderText("例如 ZenlessZoneZero.exe（留空则不自动激活）");
        exeLayout->addWidget(m_targetExeEdit);
        layout->addLayout(exeLayout);

        auto *hintLabel = new QLabel("当前台窗口属于此程序时，自动激活本宏组（仅「宏组激活」模式下生效）");
        hintLabel->setStyleSheet("color: gray; font-size: 11px;");
        hintLabel->setWordWrap(true);
        layout->addWidget(hintLabel);

        // 匹配阈值
        auto *thresLayout = new QHBoxLayout();
        thresLayout->addWidget(new QLabel("图像匹配阈值:"));
        m_thresholdSpin = new QDoubleSpinBox();
        m_thresholdSpin->setRange(0.0, 1.0);
        m_thresholdSpin->setDecimals(3);
        m_thresholdSpin->setSingleStep(0.01);
        m_thresholdSpin->setValue(0.85);
        thresLayout->addWidget(m_thresholdSpin);
        thresLayout->addSpacing(20);
        thresLayout->addWidget(new QLabel("区域容错像素:"));
        m_toleranceSpin = new QSpinBox();
        m_toleranceSpin->setRange(10, 500);
        m_toleranceSpin->setValue(50);
        m_toleranceSpin->setSuffix(" px");
        m_toleranceSpin->setToolTip("自学习后区域匹配的扩展容错范围");
        thresLayout->addWidget(m_toleranceSpin);
        thresLayout->addStretch();
        layout->addLayout(thresLayout);

        // 优先级
        auto *prioLayout = new QHBoxLayout();
        prioLayout->addWidget(new QLabel("优先级 (越小越优先):"));
        m_prioritySpin = new QSpinBox();
        m_prioritySpin->setRange(0, 999);
        m_prioritySpin->setValue(0);
        prioLayout->addWidget(m_prioritySpin);
        prioLayout->addStretch();
        layout->addLayout(prioLayout);

        // 图像自动执行
        m_imageAutoExecuteCheck = new QCheckBox("移动鼠标到目标图像中央并自动执行组内宏");
        m_imageAutoExecuteCheck->setToolTip("检测到目标图像后自动移动鼠标到图像中央并循环执行组内所有宏，图像消失后自动停止");
        layout->addWidget(m_imageAutoExecuteCheck);

        // 目标图像
        layout->addWidget(new QLabel("目标图像（可选）:"));
        m_imageListWidget = new QListWidget();
        auto *imageDelegate = new ImageDeleteDelegate(this);
        m_imageListWidget->setItemDelegate(imageDelegate);
        connect(imageDelegate, &ImageDeleteDelegate::deleteClicked, this, [this](int row)
                {
                    if (row >= 0 && row < m_imagePaths.size())
                    {
                        UI_LOG("[GROUP-EDIT] 移除目标图像[" << row << "]: " << m_imagePaths[row]);
                        m_imagePaths.removeAt(row);
                        // imageLearnData 与 imagePaths 必须按下标一一对应，删除图像时同步删除学习数据
                        if (row < m_imageLearnData.size())
                            m_imageLearnData.removeAt(row);
                        refreshImageList();
                    } });
        layout->addWidget(m_imageListWidget);

        auto *imgBtnLayout = new QHBoxLayout();
        auto *btnAddImage = new QPushButton("添加图像");
        connect(btnAddImage, &QPushButton::clicked, this, [this]()
                {
                    QString file = QFileDialog::getOpenFileName(this, "选择目标图像", QString(),
                                                                "图像文件 (*.png *.jpg *.bmp *.gif)");
                    if (!file.isEmpty())
                    {
                        // 复制到 image/ 文件夹
                        QString imageDir = QCoreApplication::applicationDirPath() + "/image";
                        QDir().mkpath(imageDir);
                        QFileInfo fi(file);
                        QString destPath = imageDir + "/" + fi.fileName();
                        // 同名文件：内容一致才复用，否则自动加序号另存，避免静默沿用旧模板
                        bool useDest = false;
                        if (QFile::exists(destPath))
                        {
                            QFile existingFile(destPath);
                            QFile incomingFile(file);
                            bool same = false;
                            if (existingFile.open(QIODevice::ReadOnly) && incomingFile.open(QIODevice::ReadOnly))
                                same = (existingFile.size() == incomingFile.size()) &&
                                       (existingFile.readAll() == incomingFile.readAll());
                            if (same)
                            {
                                useDest = true;
                            }
                            else
                            {
                                const QString base = fi.completeBaseName();
                                const QString ext = fi.suffix().isEmpty() ? QString() : ("." + fi.suffix());
                                int n = 1;
                                while (QFile::exists(imageDir + "/" + base + "_" + QString::number(n) + ext))
                                    ++n;
                                destPath = imageDir + "/" + base + "_" + QString::number(n) + ext;
                                useDest = QFile::copy(file, destPath);
                            }
                        }
                        else
                        {
                            useDest = QFile::copy(file, destPath);
                        }
                        QString finalPath = useDest ? QDir::toNativeSeparators(destPath) : file;
                        if (!useDest)
                            QMessageBox::warning(this, "复制失败", QString("无法复制图像到 image 目录：\n%1").arg(file));
                        if (!m_imagePaths.contains(finalPath))
                        {
                            m_imagePaths.append(finalPath);
                            m_imageLearnData.append(ImageLearnData());
                            UI_LOG("[GROUP-EDIT] 添加目标图像: " << finalPath << " (组内共" << m_imagePaths.size() << "张)");
                            refreshImageList();
                        }
                        else
                        {
                            UI_LOG("[GROUP-EDIT] 图像已存在，跳过: " << finalPath);
                        }
                    } });
        imgBtnLayout->addWidget(btnAddImage);
        auto *btnPickFromFolder = new QPushButton("从 image 文件夹选择");
        connect(btnPickFromFolder, &QPushButton::clicked, this, [this]()
                {
                    ImageFolderPickerDialog dlg(this);
                    if (dlg.exec() == QDialog::Accepted)
                    {
                        QString fileName = dlg.selectedFile();
                        if (!fileName.isEmpty())
                        {
                            QString imageDir = QCoreApplication::applicationDirPath() + "/image";
                            QString fullPath = QDir::toNativeSeparators(imageDir + "/" + fileName);
                            if (!m_imagePaths.contains(fullPath))
                            {
                                m_imagePaths.append(fullPath);
                                m_imageLearnData.append(ImageLearnData());
                                UI_LOG("[GROUP-EDIT] 从 image 文件夹添加: " << fullPath);
                                refreshImageList();
                            }
                        }
                    } });
        imgBtnLayout->addWidget(btnPickFromFolder);
        imgBtnLayout->addStretch();
        layout->addLayout(imgBtnLayout);

        // 组内宏列表
        layout->addWidget(new QLabel("组内宏列表（从宏库存中选择）:"));
        m_macroListWidget = new QListWidget();
        auto *groupMacroDelegate = new ImageDeleteDelegate(this);
        m_macroListWidget->setItemDelegate(groupMacroDelegate);
        connect(groupMacroDelegate, &ImageDeleteDelegate::deleteClicked, this, [this](int row)
                {
                    if (row >= 0 && row < m_groupMacroNames.size())
                    {
                        m_groupMacroNames.removeAt(row);
                        refreshMacroList();
                    } });
        layout->addWidget(m_macroListWidget);

        auto *btnLayout = new QHBoxLayout();
        auto *btnAddMacro = new QPushButton("从库存添加宏");
        connect(btnAddMacro, &QPushButton::clicked, this, &GroupEditDialog::addExistingMacro);
        btnLayout->addWidget(btnAddMacro);
        btnLayout->addStretch();
        layout->addLayout(btnLayout);

        // OK/Cancel
        auto *dlgBtns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(dlgBtns, &QDialogButtonBox::accepted, this, &GroupEditDialog::onAccept);
        connect(dlgBtns, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(dlgBtns);

        loadData();
    }

private slots:
    void addExistingMacro()
    {
        QStringList candidates;
        const auto &inventory = m_engine->macroInventory();
        for (const auto &m : inventory)
        {
            if (!m_groupMacroNames.contains(m.name))
                candidates.append(m.name);
        }

        if (candidates.isEmpty())
        {
            QMessageBox::information(this, "提示", "没有可添加的宏。请先在宏库存中创建宏。");
            return;
        }

        QDialog picker(this);
        picker.setWindowTitle("从库存选择宏");
        picker.setMinimumSize(400, 350);
        auto *pl = new QVBoxLayout(&picker);
        auto *list = new QListWidget();
        for (int i = 0; i < candidates.size(); ++i)
        {
            list->addItem(candidates[i]);
            list->item(i)->setData(Qt::UserRole, i);
        }
        pl->addWidget(list);

        auto *pbtn = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(pbtn, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
        connect(pbtn, &QDialogButtonBox::rejected, &picker, &QDialog::reject);
        pl->addWidget(pbtn);

        if (picker.exec() == QDialog::Accepted && list->currentRow() >= 0)
        {
            int idx = list->currentItem()->data(Qt::UserRole).toInt();
            m_groupMacroNames.append(candidates[idx]);
            refreshMacroList();
        }
    }

    void removeMacroFromGroup()
    {
        int row = m_macroListWidget->currentRow();
        if (row >= 0 && row < m_groupMacroNames.size())
        {
            m_groupMacroNames.removeAt(row);
            refreshMacroList();
        }
    }

    void onAccept()
    {
        if (m_nameEdit->text().trimmed().isEmpty())
        {
            QMessageBox::warning(this, "错误", "组名称不能为空。");
            return;
        }

        auto *grp = m_engine->mutableGroup(m_groupIndex);
        if (!grp)
            return;

        grp->name = m_nameEdit->text().trimmed();
        grp->targetExe = m_targetExeEdit->text().trimmed();
        grp->matchThreshold = m_thresholdSpin->value();
        grp->imageTolerancePx = m_toleranceSpin->value();
        grp->priority = m_prioritySpin->value();
        grp->imageAutoExecute = m_imageAutoExecuteCheck->isChecked();

        // 清理失效/重复的宏引用（按名称引用库存，失效引用会被自动执行静默跳过）
        QStringList cleanedMacroNames;
        for (const QString &name : m_groupMacroNames)
        {
            if (m_engine->findMacro(name) && !cleanedMacroNames.contains(name))
                cleanedMacroNames.append(name);
        }
        if (cleanedMacroNames.size() != m_groupMacroNames.size())
            UI_LOG("[GROUP-EDIT] 清理失效/重复宏引用: " << m_groupMacroNames.size() << " -> " << cleanedMacroNames.size());
        grp->macroNames = cleanedMacroNames;

        // imagePaths 与 imageLearnData 必须一一对应：按路径保留旧学习数据，新增图像置未学习
        const QStringList oldImagePaths = grp->imagePaths;
        const QVector<ImageLearnData> oldLearnData = grp->imageLearnData;
        QVector<ImageLearnData> alignedLearnData;
        alignedLearnData.reserve(m_imagePaths.size());
        for (const QString &path : m_imagePaths)
        {
            int oldIdx = oldImagePaths.indexOf(path);
            if (oldIdx >= 0 && oldIdx < oldLearnData.size())
                alignedLearnData.append(oldLearnData[oldIdx]);
            else
                alignedLearnData.append(ImageLearnData());
        }
        grp->imagePaths = m_imagePaths;
        grp->imageLearnData = alignedLearnData;

        UI_LOG("[GROUP-EDIT] 保存宏组: \"" << grp->name
                                           << "\" targetExe=\"" << grp->targetExe
                                           << "\" 阈值=" << grp->matchThreshold
                                           << " 图像数=" << grp->imagePaths.size()
                                           << " 宏数=" << grp->macroNames.size());
        for (int i = 0; i < grp->imagePaths.size(); ++i)
            UI_LOG("[GROUP-EDIT]   图像[" << i << "]: " << grp->imagePaths[i]);

        m_engine->rebuildImageCheckItems();
        // 组内宏名单变化会影响热键集合，必须重建并更新钩子状态
        m_engine->rebuildHotkeyVkCodes();
        m_engine->updateHooksState();

        emit m_engine->dataChanged();
        accept();
    }

private:
    void loadData()
    {
        auto *grp = m_engine->mutableGroup(m_groupIndex);
        if (!grp)
            return;

        m_nameEdit->setText(grp->name);
        m_targetExeEdit->setText(grp->targetExe);
        m_thresholdSpin->setValue(grp->matchThreshold);
        m_toleranceSpin->setValue(grp->imageTolerancePx);
        m_prioritySpin->setValue(grp->priority);
        m_imageAutoExecuteCheck->setChecked(grp->imageAutoExecute);
        m_groupMacroNames = grp->macroNames;
        m_imagePaths = grp->imagePaths;
        m_imageLearnData = grp->imageLearnData;
        refreshMacroList();
        refreshImageList();
    }

    void refreshMacroList()
    {
        m_macroListWidget->clear();
        for (const auto &name : m_groupMacroNames)
            m_macroListWidget->addItem(name);
    }

    void refreshImageList()
    {
        m_imageListWidget->clear();
        for (const auto &path : m_imagePaths)
        {
            QFileInfo fi(path);
            m_imageListWidget->addItem(fi.fileName());
        }
    }

    MacroEngine *m_engine;
    int m_groupIndex;
    QLineEdit *m_nameEdit;
    QLineEdit *m_targetExeEdit;
    QDoubleSpinBox *m_thresholdSpin;
    QSpinBox *m_toleranceSpin;
    QSpinBox *m_prioritySpin;
    QCheckBox *m_imageAutoExecuteCheck;
    QListWidget *m_macroListWidget;
    QListWidget *m_imageListWidget;
    QStringList m_groupMacroNames;
    QStringList m_imagePaths;
    QVector<ImageLearnData> m_imageLearnData; // 与 m_imagePaths 一一对应
};

// ================== 主窗口 ==================
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr) : QMainWindow(parent)
    {
        engine = MacroEngine::instance();
        UI_LOG(QString("MainWindow constructed"));

        setWindowTitle("按键宏");
        initUi();
        loadCategoryData();

        // 配置文件存在但加载失败：不注入演示数据，避免覆盖用户原有配置
        if (engine->configLoadFailed())
        {
            QMessageBox::warning(this, "配置加载失败",
                                 "配置文件 macros.ini 无法解析（可能已损坏或被其他程序占用）。\n\n"
                                 "为避免覆盖原文件，本次不会自动写入演示数据；\n"
                                 "退出时请确认是否仍要覆盖保存（旧文件会备份为 macros.ini.bak）。");
        }
        else
        {
            // 保证有默认组
            if (engine->groups().isEmpty())
            {
                MacroGroup defaultGroup;
                defaultGroup.name = "默认宏组";
                engine->addGroup(defaultGroup);
            }

            // 如果库存为空，添加演示宏
            if (engine->macroInventory().isEmpty())
            {
                Macro demo;
                demo.name = "演示宏";
                demo.steps = {
                    {StepType::KeyPress, 'A'},
                    {StepType::Delay, 0, 0, 0.05},
                    {StepType::KeyRelease, 'A'}};
                engine->addToInventory(demo);
                // 同时将演示宏添加到默认组的引用列表
                auto *grp = engine->mutableGroup(0);
                if (grp && !grp->macroNames.contains("演示宏"))
                    grp->macroNames.append("演示宏");
                engine->setCurrentMacro(demo);
            }
        }

        refreshGroupAndMacroLists();
        refreshMacroListFromInventory();
        // Select first non-header item
        for (int i = 0; i < macroList->count(); ++i)
        {
            auto *it = macroList->item(i);
            if (it && it->data(Qt::UserRole).isValid())
            {
                macroList->setCurrentItem(it);
                break;
            }
        }

        connect(engine, &MacroEngine::recordingStarted, this, [this]()
                {
            btnRecordToggle->setText("停止录制 (F12)");
            engine->setRecordExcludeRegion(QRect(btnRecordToggle->mapToGlobal(QPoint(0, 0)), btnRecordToggle->size())); });
        connect(engine, &MacroEngine::recordingStopped, this, [this]()
                {
            btnRecordToggle->setText("开始录制 (F11)");
            engine->clearRecordExcludeRegion();
            refreshStepList(); });
        connect(engine, &MacroEngine::stepRecorded, this, [this](const MacroStep &step)
                { appendStepToList(step); });
        connect(engine, &MacroEngine::triggerKeySet, this, [this](const QString &key)
                { triggerLabel->setText(key); });
        connect(engine, &MacroEngine::dataChanged, this, [this]()
                {
            refreshGroupAndMacroLists();
            refreshMacroListFromInventory(); });
        // 引擎的保存/加载日志接入 UI 日志区（否则保存失败对用户完全不可见）
        connect(engine, &MacroEngine::logMessage, this, &MainWindow::log);
        m_activeGroups.clear();
        auto updateActiveLabel = [this]()
        {
            QStringList names;
            for (int idx : m_activeGroups)
            {
                if (idx >= 0 && idx < engine->groups().size())
                    names << engine->groups()[idx].name;
            }
            if (names.isEmpty())
                activeGroupLabel->setText("当前激活组：无");
            else
                activeGroupLabel->setText(QString("当前激活组：%1").arg(names.join(", ")));
        };
        connect(engine, &MacroEngine::activeGroupActivated, this, [this, updateActiveLabel](int idx)
                {
            m_activeGroups.insert(idx);
            updateActiveLabel();
            refreshGroupAndMacroLists(); });
        connect(engine, &MacroEngine::activeGroupDeactivated, this, [this, updateActiveLabel](int idx)
                {
            m_activeGroups.remove(idx);
            updateActiveLabel();
            refreshGroupAndMacroLists(); });
        connect(engine, &MacroEngine::foregroundProcessChanged, this, [this](const QString &proc)
                { foregroundProcLabel->setText(QString("前台进程：%1").arg(proc.isEmpty() ? "未知" : proc)); });
        // 恢复窗口位置和大小
        restoreWindowGeometry();

        // 初始化托盘
        initTrayIcon();

        // 单实例服务端
        localServer = new QLocalServer(this);
        localServer->listen("KeyMacro_SingleInstance_Server");
        connect(localServer, &QLocalServer::newConnection, this, [this]()
                {
                    QLocalSocket *client = localServer->nextPendingConnection();
                    client->waitForReadyRead(500);
                    client->deleteLater();
                    showMainWindow(); });

        // 根据设置决定是否最小化启动
        if (loadMinimizeOnStart())
        {
            QTimer::singleShot(100, this, [this]()
                               {
                    hide();
                    if (hideTrayAction->isChecked())
                        trayIcon->hide(); });
        }
    }

    void log(const QString &msg)
    {
        UI_LOG(QString("Log: %1").arg(msg));
        logText->append(msg);
    }

protected:
    void closeEvent(QCloseEvent *event) override
    {
        // 保存窗口位置和大小
        saveWindowGeometry();
        saveCategoryData();

        // 配置此前加载失败时，覆盖前必须让用户确认，避免把损坏/不可读的原文件彻底覆盖
        if (engine->configLoadFailed())
        {
            auto ret = QMessageBox::question(this, "配置加载失败",
                                             "配置文件此前加载失败，是否仍用当前数据覆盖 macros.ini？\n"
                                             "旧文件会备份为 macros.ini.bak。",
                                             QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ret != QMessageBox::Yes)
            {
                hide();
                event->ignore();
                return;
            }
        }

        QString filePath = QCoreApplication::applicationDirPath() + "/macros.ini";
        if (!engine->saveToFile(filePath))
            QMessageBox::warning(this, "保存失败", "配置保存失败：请检查程序目录是否可写、磁盘空间是否充足。");

        // 点击关闭按钮时，最小化到托盘而不是退出
        hide();
        if (hideTrayAction->isChecked())
        {
            trayIcon->hide();
        }
        event->ignore();
    }

    void showMainWindow()
    {
        // 显示窗口并恢复托盘图标
        if (hideTrayAction->isChecked())
        {
            trayIcon->show();
        }
        // 最小化状态下直接 show() 不会还原窗口，需要先退出最小化
        if (isMinimized())
            showNormal();
        else
            show();
        raise();
        activateWindow();
    }

public:
    void quitApplication()
    {
        // 保存窗口位置和大小
        saveWindowGeometry();
        saveCategoryData();

        QString filePath = QCoreApplication::applicationDirPath() + "/macros.ini";
        if (!engine->saveToFile(filePath))
            QMessageBox::warning(this, "保存失败", "配置保存失败：请检查程序目录是否可写、磁盘空间是否充足。");
        UI_LOG("应用程序退出");
        qApp->quit();
    }

    void moveEvent(QMoveEvent *event) override
    {
        QMainWindow::moveEvent(event);
        updateRecordExcludeRegion();
    }

    void resizeEvent(QResizeEvent *event) override
    {
        QMainWindow::resizeEvent(event);
        updateRecordExcludeRegion();
    }

    void updateRecordExcludeRegion()
    {
        if (engine->isRecording())
        {
            engine->setRecordExcludeRegion(QRect(btnRecordToggle->mapToGlobal(QPoint(0, 0)), btnRecordToggle->size()));
        }
    }

private slots:
    void newMacro()
    {
        bool ok;
        QString name = QInputDialog::getText(this, "新建宏", "宏名称:", QLineEdit::Normal, "新宏", &ok);
        if (!ok)
            return;
        name = name.trimmed();
        if (name.isEmpty())
        {
            QMessageBox::warning(this, "名称无效", "宏名称不能为空。");
            return;
        }
        // 宏在库存/组引用/热键绑定中都按名称索引，重名会导致编辑、删除、绑定错宏
        if (engine->findMacro(name))
        {
            QMessageBox::warning(this, "名称重复", QString("已存在名为 \"%1\" 的宏，请换一个名称。").arg(name));
            return;
        }
        Macro m;
        m.name = name;
        int idx = engine->addToInventory(m);
        engine->setCurrentMacro(m);
        refreshMacroListFromInventory();
        // Find visual row by UserRole (inventory index)
        for (int i = 0; i < macroList->count(); ++i)
        {
            auto *it = macroList->item(i);
            if (it && it->data(Qt::UserRole).isValid() && it->data(Qt::UserRole).toInt() == idx)
            {
                macroList->setCurrentItem(it);
                break;
            }
        }
        refreshGroupAndMacroLists();
        // refresh 过程 blockSignals，不会触发 currentItemChanged，需手工同步右侧面板
        onMacroSelectionChanged(idx);
        UI_LOG(QString("New macro: %1").arg(name));
    }

    void deleteMacro()
    {
        auto *cur = macroList->currentItem();
        if (!cur || !cur->data(Qt::UserRole).isValid())
            return;
        int idx = cur->data(Qt::UserRole).toInt();
        const auto &inv = engine->macroInventory();
        if (idx < 0 || idx >= inv.size())
            return;
        // 显式拷贝名称，避免删除后引用悬垂
        const QString name = inv[idx].name;
        engine->removeFromInventory(name);
        remapIndicesAfterMacroRemoved(idx);
        refreshMacroListFromInventory();
        refreshGroupAndMacroLists();
        if (!engine->macroInventory().isEmpty())
            onMacroSelectionChanged(0);
        else
            resetMacroPanel();
        UI_LOG(QString("Deleted macro: %1").arg(name));
    }

    void onMacroSelectionChanged(int row)
    {
        // 录制期间禁止切换宏：stopRecording 会把录制步骤追加进 currentMacro，切换会污染目标宏
        if (engine->isRecording())
        {
            QMessageBox::information(this, "正在录制", "录制过程中不能切换宏，请先停止录制。");
            refreshMacroListFromInventory(); // 恢复列表选中项到当前宏
            return;
        }
        const auto &inv = engine->macroInventory();
        if (row < 0 || row >= inv.size())
            return;
        const Macro &m = inv[row];
        engine->setCurrentMacro(m);
        // 先清空步骤列表，防止 syncStepsFromList 将旧宏的视觉顺序误同步到新宏
        stepList->clear();
        refreshStepList();
        nameEdit->setText(m.name);
        triggerLabel->setText(m.triggerKey.isEmpty() ? "未设置" : m.triggerKey);
        repeatCombo->setCurrentIndex(m.repeatMode);
        enableCheck->setChecked(m.enabled);
        globalActivationCheck->setChecked(m.globalActivation);
        blockActionKeysCheck->setChecked(m.blockActionKeys);
        UI_LOG(QString("Selected macro: %1").arg(m.name));
    }

    void addDelayStep()
    {
        syncStepsFromList();
        Macro macro = engine->currentMacro();
        macro.steps.append({StepType::Delay, 0, 0, 0.05});
        engine->setCurrentMacro(macro);
        refreshStepList();
    }

    void deleteSelectedStep()
    {
        syncStepsFromList();
        int row = stepList->currentRow();
        Macro macro = engine->currentMacro();
        if (row >= 0 && row < macro.steps.size())
        {
            macro.steps.removeAt(row);
            engine->setCurrentMacro(macro);
            refreshStepList();
        }
    }

    void clearSteps()
    {
        syncStepsFromList();
        Macro macro = engine->currentMacro();
        macro.steps.clear();
        engine->setCurrentMacro(macro);
        refreshStepList();
    }

    void saveWindowGeometry()
    {
        QSettings settings("KeyMacroUI", "Settings");
        settings.setValue("windowGeometry", saveGeometry());
    }

    void restoreWindowGeometry()
    {
        QSettings settings("KeyMacroUI", "Settings");
        QByteArray geometry = settings.value("windowGeometry").toByteArray();
        if (!geometry.isEmpty())
        {
            restoreGeometry(geometry);
        }
        else
        {
            // 默认大小和位置
            resize(1000, 700);
            move(100, 100);
        }
    }

    void saveData()
    {
        QString filePath = QCoreApplication::applicationDirPath() + "/macros.ini";
        if (!engine->saveToFile(filePath))
            QMessageBox::warning(this, "保存失败", "配置保存失败：请检查程序目录是否可写、磁盘空间是否充足。");
        else
            log("所有数据已保存");
        saveCategoryData();
    }

    // ========== 分类持久化 ==========
    void saveCategoryData()
    {
        QSettings settings("KeyMacroUI", "Categories");
        // 保存组分类
        QVariantMap gcm;
        for (auto it = m_groupCategories.begin(); it != m_groupCategories.end(); ++it)
            gcm[QString::number(it.key())] = it.value();
        settings.setValue("groupCategories", gcm);
        // 保存宏分类
        QVariantMap mcm;
        for (auto it = m_macroCategories.begin(); it != m_macroCategories.end(); ++it)
            mcm[QString::number(it.key())] = it.value();
        settings.setValue("macroCategories", mcm);
        // 保存折叠状态
        QStringList gcollapsed;
        for (const auto &c : m_collapsedGroupCategories)
            gcollapsed << ("G:" + c);
        for (const auto &c : m_collapsedMacroCategories)
            gcollapsed << ("M:" + c);
        settings.setValue("collapsedCategories", gcollapsed);
        // 保存空分类
        QStringList empty;
        for (const auto &c : m_emptyGroupCategories)
            empty << ("G:" + c);
        for (const auto &c : m_emptyMacroCategories)
            empty << ("M:" + c);
        settings.setValue("emptyCategories", empty);
    }

    // 删除组后，组索引整体前移：重映射分类映射与激活组集合（引擎内部已在 removeGroup 中重映射自己的状态）
    void remapIndicesAfterGroupRemoved(int removedIndex)
    {
        QHash<int, QString> newCats;
        for (auto it = m_groupCategories.begin(); it != m_groupCategories.end(); ++it)
        {
            if (it.key() == removedIndex)
                continue;
            newCats[it.key() > removedIndex ? it.key() - 1 : it.key()] = it.value();
        }
        m_groupCategories = newCats;
        QSet<int> newActive;
        for (int idx : m_activeGroups)
        {
            if (idx == removedIndex)
                continue;
            newActive.insert(idx > removedIndex ? idx - 1 : idx);
        }
        m_activeGroups = newActive;
        saveCategoryData();
    }

    // 删除库存宏后，宏索引整体前移：重映射宏分类映射
    void remapIndicesAfterMacroRemoved(int removedIndex)
    {
        QHash<int, QString> newCats;
        for (auto it = m_macroCategories.begin(); it != m_macroCategories.end(); ++it)
        {
            if (it.key() == removedIndex)
                continue;
            newCats[it.key() > removedIndex ? it.key() - 1 : it.key()] = it.value();
        }
        m_macroCategories = newCats;
        saveCategoryData();
    }

    void loadCategoryData()
    {
        QSettings settings("KeyMacroUI", "Categories");        QVariantMap gcm = settings.value("groupCategories").toMap();
        for (auto it = gcm.begin(); it != gcm.end(); ++it)
        {
            bool ok;
            int idx = it.key().toInt(&ok);
            if (ok && !it.value().toString().trimmed().isEmpty())
                m_groupCategories[idx] = it.value().toString().trimmed();
        }
        QVariantMap mcm = settings.value("macroCategories").toMap();
        for (auto it = mcm.begin(); it != mcm.end(); ++it)
        {
            bool ok;
            int idx = it.key().toInt(&ok);
            if (ok && !it.value().toString().trimmed().isEmpty())
                m_macroCategories[idx] = it.value().toString().trimmed();
        }
        QStringList collapsed = settings.value("collapsedCategories").toStringList();
        for (const auto &c : collapsed)
        {
            if (c.startsWith("G:"))
                m_collapsedGroupCategories.insert(c.mid(2));
            else if (c.startsWith("M:"))
                m_collapsedMacroCategories.insert(c.mid(2));
        }
        QStringList empty = settings.value("emptyCategories").toStringList();
        for (const auto &c : empty)
        {
            if (c.startsWith("G:"))
                m_emptyGroupCategories.insert(c.mid(2));
            else if (c.startsWith("M:"))
                m_emptyMacroCategories.insert(c.mid(2));
        }
    }

    // ========== 拖拽后从视觉顺序更新分类映射 ==========
    void updateGroupCategoriesFromVisualOrder()
    {
        QString currentCat; // empty = 默认分类（无头）
        QSet<QString> seenCats; // categories that have items
        for (int i = 0; i < groupList->count(); ++i)
        {
            QListWidgetItem *it = groupList->item(i);
            if (!it)
                continue;
            if (CategoryListWidget::isHeader(it))
            {
                currentCat = it->data(Qt::UserRole + 1).toString();
                continue;
            }
            QVariant v = it->data(Qt::UserRole);
            if (!v.isValid())
                continue;
            int gidx = v.toInt();
            QString newCat = currentCat.trimmed();
            // Update the item's own category data
            it->setData(Qt::UserRole + 1, newCat);
            if (newCat.isEmpty())
                m_groupCategories.remove(gidx);
            else
            {
                m_groupCategories[gidx] = newCat;
                seenCats.insert(newCat);
            }
        }
        // Remove categories that now have items from empty set
        for (const auto &cat : seenCats)
            m_emptyGroupCategories.remove(cat);
        saveCategoryData();
    }

    void updateMacroCategoriesFromVisualOrder()
    {
        QString currentCat;
        QSet<QString> seenCats;
        for (int i = 0; i < macroList->count(); ++i)
        {
            QListWidgetItem *it = macroList->item(i);
            if (!it)
                continue;
            if (CategoryListWidget::isHeader(it))
            {
                currentCat = it->data(Qt::UserRole + 1).toString();
                continue;
            }
            QVariant v = it->data(Qt::UserRole);
            if (!v.isValid())
                continue;
            int invIdx = v.toInt();
            QString newCat = currentCat.trimmed();
            it->setData(Qt::UserRole + 1, newCat);
            if (newCat.isEmpty())
                m_macroCategories.remove(invIdx);
            else
            {
                m_macroCategories[invIdx] = newCat;
                seenCats.insert(newCat);
            }
        }
        for (const auto &cat : seenCats)
            m_emptyMacroCategories.remove(cat);
        saveCategoryData();
    }

    void onStepsReordered(const QModelIndex &parent, int start, int end, const QModelIndex &destination, int row);

public:
    bool loadMinimizeOnStart()
    {
        QSettings settings("KeyMacroUI", "Settings");
        return settings.value("minimizeOnStart", true).toBool();
    }

    void saveMinimizeOnStart(bool minimize)
    {
        QSettings settings("KeyMacroUI", "Settings");
        settings.setValue("minimizeOnStart", minimize);
    }

    bool isAutoStartEnabled()
    {
        // 同时检查新旧任务名，兼容旧版本
        QProcess proc;
        proc.start("schtasks", {"/query", "/tn", "按键宏开机自动启动"}, QIODevice::ReadOnly);
        proc.waitForFinished(3000);
        return proc.exitCode() == 0;
    }

    void setAutoStart(bool enable)
    {
        // 清理旧的 Run 注册表项（迁移）
        {
            QSettings oldSettings("HKEY_CURRENT_USER\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
            oldSettings.remove("KeyMacroUI");
        }

        // 清理旧任务名
        {
            QProcess p;
            p.start("schtasks", {"/delete", "/tn", "KeyMacro", "/f"}, QIODevice::ReadOnly);
            p.waitForFinished(3000);
        }

        QString appPath = QCoreApplication::applicationFilePath();
        appPath.replace('/', '\\');

        if (enable)
        {
            // 用 PowerShell 创建，确保无交流电源限制
            QString psCmd = QString(
                                "$a=New-ScheduledTaskAction -Execute '%1';"
                                "$t=New-ScheduledTaskTrigger -AtLogon;"
                                "$s=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries;"
                                "$p=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -RunLevel Highest;"
                                "Register-ScheduledTask -TaskName '按键宏开机自动启动' -Action $a -Trigger $t -Settings $s -Principal $p -Force")
                                .arg(appPath);
            QProcess proc;
            proc.start("powershell", {"-NoProfile", "-Command", psCmd}, QIODevice::ReadOnly);
            proc.waitForFinished(10000);
        }
        else
        {
            QProcess proc;
            proc.start("schtasks", {"/delete", "/tn", "按键宏开机自动启动", "/f"}, QIODevice::ReadOnly);
            proc.waitForFinished(3000);
        }
    }

    bool loadHideTray()
    {
        QSettings settings("KeyMacroUI", "Settings");
        return settings.value("hideTrayIcon", false).toBool();
    }

    void saveHideTray(bool hide)
    {
        QSettings settings("KeyMacroUI", "Settings");
        settings.setValue("hideTrayIcon", hide);
    }

private:
    CategoryListWidget *groupList;
    CategoryListWidget *macroList;
    QLabel *activeGroupLabel;
    QLineEdit *nameEdit;
    QLabel *triggerLabel;
    QComboBox *repeatCombo;
    QCheckBox *enableCheck;
    QCheckBox *globalActivationCheck;
    QCheckBox *blockActionKeysCheck;
    QListWidget *stepList;
    QPushButton *btnRecordToggle;
    QTextEdit *logText;
    QSystemTrayIcon *trayIcon;
    QMenu *trayMenu;
    QAction *showHideAction;
    QAction *minimizeOnStartAction;
    QAction *autoStartAction;
    QAction *hideTrayAction;
    QAction *quitAction;
    QLocalServer *localServer;

    MacroEngine *engine;
    QLabel *foregroundProcLabel;
    QSet<int> m_activeGroups;
    int m_selectedGroupIndex = 0;

    // UI-only category mappings (visual only, not persisted to engine)
    QHash<int, QString> m_groupCategories;    // groupIndex -> category name
    QHash<int, QString> m_macroCategories;    // inventoryIndex -> category name
    QSet<QString> m_collapsedGroupCategories; // collapsed category names for groups
    QSet<QString> m_collapsedMacroCategories; // collapsed category names for macros
    QSet<QString> m_emptyGroupCategories;     // categories with no items (still shown)
    QSet<QString> m_emptyMacroCategories;     // categories with no items (still shown)

    void initTrayIcon()
    {
        trayIcon = new QSystemTrayIcon(this);

        // 使用应用程序图标作为托盘图标
        trayIcon->setIcon(QApplication::windowIcon());
        trayIcon->setToolTip("按键宏管理");

        trayMenu = new QMenu(this);

        showHideAction = new QAction("显示/隐藏", this);
        connect(showHideAction, &QAction::triggered, this, [this]()
                {
            if (isVisible()) {
                hide();
                if (hideTrayAction->isChecked())
                    trayIcon->hide();
            } else {
                showMainWindow();
            } });
        trayMenu->addAction(showHideAction);

        trayMenu->addSeparator();

        minimizeOnStartAction = new QAction("以最小化启动", this);
        minimizeOnStartAction->setCheckable(true);
        minimizeOnStartAction->setChecked(loadMinimizeOnStart());
        connect(minimizeOnStartAction, &QAction::toggled, this, &MainWindow::saveMinimizeOnStart);
        trayMenu->addAction(minimizeOnStartAction);

        autoStartAction = new QAction("开机自动启动", this);
        autoStartAction->setCheckable(true);
        autoStartAction->setChecked(isAutoStartEnabled());
        connect(autoStartAction, &QAction::toggled, this, &MainWindow::setAutoStart);
        trayMenu->addAction(autoStartAction);

        hideTrayAction = new QAction("隐藏托盘区图标", this);
        hideTrayAction->setCheckable(true);
        hideTrayAction->setChecked(loadHideTray());
        connect(hideTrayAction, &QAction::toggled, this, [this](bool checked)
                {
                    saveHideTray(checked);
                    if (!isVisible())
                    {
                        if (checked)
                            trayIcon->hide();
                        else
                            trayIcon->show();
                    } });
        trayMenu->addAction(hideTrayAction);

        trayMenu->addSeparator();

        quitAction = new QAction("退出", this);
        connect(quitAction, &QAction::triggered, this, &MainWindow::quitApplication);
        trayMenu->addAction(quitAction);

        trayIcon->setContextMenu(trayMenu);
        trayIcon->show();

        // 双击托盘图标：切换显示/隐藏
        connect(trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason)
                {
            if (reason == QSystemTrayIcon::DoubleClick) {
                if (isVisible()) {
                    hide();
                    if (hideTrayAction->isChecked())
                        trayIcon->hide();
                } else {
                    showMainWindow();
                }
            } });
    }

    void initUi()
    {
        auto *central = new QWidget();
        setCentralWidget(central);
        auto *mainLayout = new QHBoxLayout(central);

        auto *leftWidget = new QWidget();
        leftWidget->setFixedWidth(220);
        auto *leftLayout = new QVBoxLayout(leftWidget);
        leftLayout->setContentsMargins(0, 0, 0, 0);

        auto *groupBox = new QGroupBox("宏组");
        auto *gbLayout = new QVBoxLayout(groupBox);
        groupList = new CategoryListWidget();
        groupList->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
        auto *groupDelegate = new ImageDeleteDelegate(this);
        groupList->setItemDelegate(groupDelegate);
        connect(groupDelegate, &ImageDeleteDelegate::deleteClicked, this, [this](int row)
                {
                    if (row >= 0 && row < groupList->count())
                    {
                        QListWidgetItem *it = groupList->item(row);
                        if (it && it->data(Qt::UserRole).isValid())
                        {
                            int gidx = it->data(Qt::UserRole).toInt();
                            if (gidx >= 0 && gidx < engine->groups().size())
                            {
                                engine->removeGroup(gidx);
                                remapIndicesAfterGroupRemoved(gidx);
                                refreshGroupAndMacroLists();
                                refreshMacroListFromInventory();
                            }
                        }
                    } });
        connect(groupList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item)
                {
            if (!item) return;
            QVariant v = item->data(Qt::UserRole);
            if (!v.isValid()) return; // header
            int gidx = v.toInt();
            if (gidx >= 0 && gidx < engine->groups().size()) {
                QString newName = item->text().trimmed();
                if (!newName.isEmpty() && newName != engine->groups()[gidx].name) {
                    auto *grp = engine->mutableGroup(gidx);
                    if (grp) {
                        grp->name = newName;
                        emit engine->dataChanged();
                    }
                }
            } });
        groupList->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(groupList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos)
                {
            QListWidgetItem *item = groupList->itemAt(pos);
            if (!item) return;

            // 分类头右键菜单：重命名、删除、新建
            if (CategoryListWidget::isHeader(item))
            {
                QString oldCat = item->data(Qt::UserRole + 1).toString();
                QString displayCat = oldCat.isEmpty() ? QStringLiteral("默认分类") : oldCat;
                QMenu menu;
                QAction *renameAct = menu.addAction(QStringLiteral("重命名分类 \"%1\"").arg(displayCat));
                QAction *deleteAct = nullptr;
                if (!oldCat.isEmpty())
                    deleteAct = menu.addAction(QStringLiteral("删除分类 \"%1\"").arg(displayCat));
                menu.addSeparator();
                QAction *newAct = menu.addAction("新建分类");
                QAction *act = menu.exec(groupList->viewport()->mapToGlobal(pos));
                if (act == renameAct)
                {
                    bool ok;
                    QString newName = QInputDialog::getText(this, "重命名分类", "新名称:", QLineEdit::Normal, displayCat, &ok);
                    if (ok && !newName.trimmed().isEmpty() && newName.trimmed() != oldCat)
                    {
                        QString clean = newName.trimmed();
                        // Update all items in this category (collect keys first for safety)
                        QList<int> keysToUpdate;
                        for (auto it = m_groupCategories.begin(); it != m_groupCategories.end(); ++it)
                            if (it.value() == oldCat)
                                keysToUpdate.append(it.key());
                        for (int key : keysToUpdate)
                            m_groupCategories[key] = clean;
                        // Update collapsed state key
                        if (m_collapsedGroupCategories.remove(oldCat))
                            m_collapsedGroupCategories.insert(clean);
                        // Update empty category key
                        if (m_emptyGroupCategories.remove(oldCat))
                            m_emptyGroupCategories.insert(clean);
                        refreshGroupAndMacroLists();
                        saveCategoryData();
                    }
                }
                else if (deleteAct && act == deleteAct)
                {
                    // Remove category from all items
                    for (auto it = m_groupCategories.begin(); it != m_groupCategories.end();)
                    {
                        if (it.value() == oldCat)
                            it = m_groupCategories.erase(it);
                        else
                            ++it;
                    }
                    m_collapsedGroupCategories.remove(oldCat);
                    m_emptyGroupCategories.remove(oldCat);
                    refreshGroupAndMacroLists();
                    saveCategoryData();
                }
                else if (act == newAct)
                {
                    bool ok;
                    QString newName = QInputDialog::getText(this, "新建分类", "分类名称:", QLineEdit::Normal, "", &ok);
                    if (ok && !newName.trimmed().isEmpty())
                    {
                        QString clean = newName.trimmed();
                        m_emptyGroupCategories.insert(clean);
                        refreshGroupAndMacroLists();
                        saveCategoryData();
                    }
                }
                return;
            }

            // 普通项目右键菜单
            QVariant v = item->data(Qt::UserRole);
            if (!v.isValid()) return;
            int gidx = v.toInt();
            QMenu menu;
            QAction *setAct = menu.addAction("设置分类");
            QAction *clearAct = menu.addAction("清除分类");
            QAction *act = menu.exec(groupList->viewport()->mapToGlobal(pos));
            if (act == setAct)
            {
                bool ok;
                QString prev = m_groupCategories.value(gidx);
                QString cat = QInputDialog::getText(this, "设置分类", "分类名称:", QLineEdit::Normal, prev, &ok);
                if (ok)
                {
                    if (cat.trimmed().isEmpty())
                        m_groupCategories.remove(gidx);
                    else
                    {
                        QString clean = cat.trimmed();
                        m_groupCategories[gidx] = clean;
                        m_emptyGroupCategories.remove(clean);
                    }
                    refreshGroupAndMacroLists();
                    saveCategoryData();
                }
            }
            else if (act == clearAct)
            {
                m_groupCategories.remove(gidx);
                refreshGroupAndMacroLists();
                saveCategoryData();
            } });
        // 分类头点击折叠/展开
        connect(groupList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item)
                {
            if (!item || !CategoryListWidget::isHeader(item))
                return;
            QString cat = item->data(Qt::UserRole + 1).toString();
            if (m_collapsedGroupCategories.contains(cat))
                m_collapsedGroupCategories.remove(cat);
            else
                m_collapsedGroupCategories.insert(cat);
            refreshGroupAndMacroLists();
            saveCategoryData(); });
        // 拖拽完成后更新分类
        connect(groupList, &CategoryListWidget::itemsReordered, this, [this]()
                {
            updateGroupCategoriesFromVisualOrder();
            refreshGroupAndMacroLists(); });
        gbLayout->addWidget(groupList);

        auto *btnGpLayout = new QHBoxLayout();
        auto *btnAddGroup = new QPushButton("创建宏组");
        connect(btnAddGroup, &QPushButton::clicked, this, [this]()
                {
            bool ok;
            QString name = QInputDialog::getText(this, "创建宏组", "名称:", QLineEdit::Normal, "新组", &ok);
            if (ok && !name.isEmpty()) {
                MacroGroup grp;
                grp.name = name;
                engine->addGroup(grp);
            } });
        auto *btnEditGroup = new QPushButton("编辑");
        connect(btnEditGroup, &QPushButton::clicked, this, [this]()
                {
            auto *cur = groupList->currentItem();
            if (!cur || !cur->data(Qt::UserRole).isValid())
            {
                QMessageBox::warning(this, "错误", "请先选择一个宏组。");
                return;
            }
            int idx = cur->data(Qt::UserRole).toInt();
            if (idx < 0 || idx >= engine->groups().size())
            {
                QMessageBox::warning(this, "错误", "请先选择一个宏组。");
                return;
            }
            GroupEditDialog dlg(engine, idx, this);
            dlg.exec(); });
        btnGpLayout->addWidget(btnAddGroup);
        btnGpLayout->addWidget(btnEditGroup);
        gbLayout->addLayout(btnGpLayout);

        activeGroupLabel = new QLabel("当前激活组：无");
        gbLayout->addWidget(activeGroupLabel);

        foregroundProcLabel = new QLabel("前台进程：未知");
        gbLayout->addWidget(foregroundProcLabel);
        leftLayout->addWidget(groupBox);

        connect(groupList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current, QListWidgetItem *)
                {
            if (!current) return;
            QVariant v = current->data(Qt::UserRole);
            if (v.isValid())
            {
                int gidx = v.toInt();
                if (gidx >= 0 && gidx < engine->groups().size())
                {
                    m_selectedGroupIndex = gidx;
                    refreshGroupAndMacroLists();
                }
            } });

        auto *macroBox = new QGroupBox("宏");
        auto *mbLayout = new QVBoxLayout(macroBox);
        macroList = new CategoryListWidget();
        macroList->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
        auto *macroDelegate = new ImageDeleteDelegate(this);
        macroList->setItemDelegate(macroDelegate);
        connect(macroDelegate, &ImageDeleteDelegate::deleteClicked, this, [this](int row)
                {
                    if (row >= 0 && row < macroList->count())
                    {
                        QListWidgetItem *it = macroList->item(row);
                        if (it && it->data(Qt::UserRole).isValid())
                        {
                            int invIdx = it->data(Qt::UserRole).toInt();
                            const auto &inv = engine->macroInventory();
                            if (invIdx >= 0 && invIdx < inv.size())
                            {
                                // 先拷贝名称：removeFromInventory 内部 removeAt 会让 inv 内引用失效
                                const QString nameToRemove = inv[invIdx].name;
                                engine->removeFromInventory(nameToRemove);
                                remapIndicesAfterMacroRemoved(invIdx);
                                refreshMacroListFromInventory();
                                refreshGroupAndMacroLists();
                                // 删除后同步右侧面板：选中下一个宏，否则清空面板
                                if (!engine->macroInventory().isEmpty())
                                    onMacroSelectionChanged(0);
                                else
                                    resetMacroPanel();
                            }
                        }
                    } });
        connect(macroList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item)
                {
            if (!item) return;
            QVariant v = item->data(Qt::UserRole);
            if (!v.isValid()) return; // header
            int invIdx = v.toInt();
            const auto &inv = engine->macroInventory();
            if (invIdx >= 0 && invIdx < inv.size()) {
                QString newName = item->text().trimmed();
                if (newName.isEmpty()) {
                    item->setText(inv[invIdx].name);
                    return;
                }
                QString oldName = inv[invIdx].name;
                if (newName != oldName) {
                    if (engine->findMacro(newName)) {
                        // 宏名必须唯一（引擎按名称引用），冲突时恢复原名
                        item->setText(oldName);
                        QMessageBox::warning(this, "名称重复", QString("已存在名为 \"%1\" 的宏。").arg(newName));
                        return;
                    }
                    Macro macro = inv[invIdx];
                    macro.name = newName;
                    engine->updateInInventory(oldName, macro);
                }
            } });
        macroList->setSelectionMode(QAbstractItemView::SingleSelection);
        macroList->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(macroList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos)
                {
            QListWidgetItem *item = macroList->itemAt(pos);
            if (!item) return;

            // 分类头右键菜单：重命名、删除、新建
            if (CategoryListWidget::isHeader(item))
            {
                QString oldCat = item->data(Qt::UserRole + 1).toString();
                QString displayCat = oldCat.isEmpty() ? QStringLiteral("默认分类") : oldCat;
                QMenu menu;
                QAction *renameAct = menu.addAction(QStringLiteral("重命名分类 \"%1\"").arg(displayCat));
                QAction *deleteAct = nullptr;
                if (!oldCat.isEmpty())
                    deleteAct = menu.addAction(QStringLiteral("删除分类 \"%1\"").arg(displayCat));
                menu.addSeparator();
                QAction *newAct = menu.addAction("新建分类");
                QAction *act = menu.exec(macroList->viewport()->mapToGlobal(pos));
                if (act == renameAct)
                {
                    bool ok;
                    QString newName = QInputDialog::getText(this, "重命名分类", "新名称:", QLineEdit::Normal, displayCat, &ok);
                    if (ok && !newName.trimmed().isEmpty() && newName.trimmed() != oldCat)
                    {
                        QString clean = newName.trimmed();
                        QList<int> keysToUpdate;
                        for (auto it = m_macroCategories.begin(); it != m_macroCategories.end(); ++it)
                            if (it.value() == oldCat)
                                keysToUpdate.append(it.key());
                        for (int key : keysToUpdate)
                            m_macroCategories[key] = clean;
                        if (m_collapsedMacroCategories.remove(oldCat))
                            m_collapsedMacroCategories.insert(clean);
                        if (m_emptyMacroCategories.remove(oldCat))
                            m_emptyMacroCategories.insert(clean);
                        refreshMacroListFromInventory();
                        saveCategoryData();
                    }
                }
                else if (deleteAct && act == deleteAct)
                {
                    for (auto it = m_macroCategories.begin(); it != m_macroCategories.end();)
                    {
                        if (it.value() == oldCat)
                            it = m_macroCategories.erase(it);
                        else
                            ++it;
                    }
                    m_collapsedMacroCategories.remove(oldCat);
                    m_emptyMacroCategories.remove(oldCat);
                    refreshMacroListFromInventory();
                    saveCategoryData();
                }
                else if (act == newAct)
                {
                    bool ok;
                    QString newName = QInputDialog::getText(this, "新建分类", "分类名称:", QLineEdit::Normal, "", &ok);
                    if (ok && !newName.trimmed().isEmpty())
                    {
                        QString clean = newName.trimmed();
                        m_emptyMacroCategories.insert(clean);
                        refreshMacroListFromInventory();
                        saveCategoryData();
                    }
                }
                return;
            }

            // 普通项目右键菜单
            QVariant v = item->data(Qt::UserRole);
            if (!v.isValid()) return;
            int invIdx = v.toInt();
            QMenu menu;
            QAction *setAct = menu.addAction("设置分类");
            QAction *clearAct = menu.addAction("清除分类");
            QAction *act = menu.exec(macroList->viewport()->mapToGlobal(pos));
            if (act == setAct)
            {
                bool ok;
                QString prev = m_macroCategories.value(invIdx);
                QString cat = QInputDialog::getText(this, "设置分类", "分类名称:", QLineEdit::Normal, prev, &ok);
                if (ok)
                {
                    if (cat.trimmed().isEmpty())
                        m_macroCategories.remove(invIdx);
                    else
                    {
                        QString clean = cat.trimmed();
                        m_macroCategories[invIdx] = clean;
                        m_emptyMacroCategories.remove(clean);
                    }
                    refreshMacroListFromInventory();
                    saveCategoryData();
                }
            }
            else if (act == clearAct)
            {
                m_macroCategories.remove(invIdx);
                refreshMacroListFromInventory();
                saveCategoryData();
            } });
        // 分类头点击折叠/展开
        connect(macroList, &QListWidget::itemClicked, this, [this](QListWidgetItem *item)
                {
            if (!item || !CategoryListWidget::isHeader(item))
                return;
            QString cat = item->data(Qt::UserRole + 1).toString();
            if (m_collapsedMacroCategories.contains(cat))
                m_collapsedMacroCategories.remove(cat);
            else
                m_collapsedMacroCategories.insert(cat);
            refreshMacroListFromInventory();
            saveCategoryData(); });
        // 拖拽完成后更新分类
        connect(macroList, &CategoryListWidget::itemsReordered, this, [this]()
                {
            updateMacroCategoriesFromVisualOrder();
            refreshMacroListFromInventory(); });
        connect(macroList, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *current, QListWidgetItem *)
                {
            if (!current) return;
            QVariant v = current->data(Qt::UserRole);
            if (!v.isValid()) return;
            int invIdx = v.toInt();
            onMacroSelectionChanged(invIdx); });
        mbLayout->addWidget(macroList);

        auto *btnMlLayout = new QHBoxLayout();
        auto *btnAddMacro2 = new QPushButton("新建宏");
        connect(btnAddMacro2, &QPushButton::clicked, this, &MainWindow::newMacro);
        btnMlLayout->addWidget(btnAddMacro2);
        mbLayout->addLayout(btnMlLayout);
        leftLayout->addWidget(macroBox);
        mainLayout->addWidget(leftWidget);

        auto *keyGroup = new QGroupBox("键盘与鼠标");
        auto *keyLayout = new QVBoxLayout(keyGroup);
        auto *keyboardWidget = new KeyboardWidget();
        keyLayout->addWidget(keyboardWidget);
        connect(keyboardWidget, &KeyboardWidget::keyClicked, this, [this](const QString &key)
                {
            appendVirtualKeyToMacro(key);
            log(key); });
        mainLayout->addWidget(keyGroup, 1);

        auto *rightWidget = new QWidget();
        rightWidget->setFixedWidth(350);
        auto *rightLayout = new QVBoxLayout(rightWidget);
        rightLayout->setAlignment(Qt::AlignTop);

        auto *groupMacro = new QGroupBox("宏属性");
        auto *gLayout = new QVBoxLayout(groupMacro);
        auto *nameLayout = new QHBoxLayout();
        nameLayout->addWidget(new QLabel("名称:"));
        nameEdit = new QLineEdit("");
        nameLayout->addWidget(nameEdit);
        gLayout->addLayout(nameLayout);

        auto *triggerLayout = new QHBoxLayout();
        triggerLayout->addWidget(new QLabel("绑定键:"));
        triggerLabel = new QLabel("未设置");
        triggerLabel->setFrameStyle(QFrame::Box);
        triggerLayout->addWidget(triggerLabel);
        auto *btnCapture = new QPushButton("录制绑定");
        connect(btnCapture, &QPushButton::clicked, this, [this]()
                { engine->startBindRecording(); });
        triggerLayout->addWidget(btnCapture);
        gLayout->addLayout(triggerLayout);

        auto *repeatLayout = new QHBoxLayout();
        repeatLayout->addWidget(new QLabel("重复模式:"));
        repeatCombo = new QComboBox();
        repeatCombo->addItems({"不重复", "按住重复，松开中断", "按住执行，松开中断", "按一下重复，再按一下停止"});
        repeatLayout->addWidget(repeatCombo);
        gLayout->addLayout(repeatLayout);

        enableCheck = new QCheckBox("启用");
        enableCheck->setChecked(true);
        gLayout->addWidget(enableCheck);

        globalActivationCheck = new QCheckBox("全局激活");
        globalActivationCheck->setChecked(false);
        gLayout->addWidget(globalActivationCheck);

        blockActionKeysCheck = new QCheckBox("屏蔽动作键位");
        blockActionKeysCheck->setChecked(false);
        blockActionKeysCheck->setToolTip("勾选后，宏运行时将屏蔽步骤序列中已有键位的真实键盘输入，避免被真实按键干扰");
        gLayout->addWidget(blockActionKeysCheck);
        rightLayout->addWidget(groupMacro);

        auto *groupStep = new QGroupBox("步骤序列 (可拖拽重排，双击延迟项可直接编辑)");
        auto *stepLayout = new QVBoxLayout(groupStep);
        stepList = new QListWidget();
        stepList->setDragDropMode(QAbstractItemView::InternalMove);
        stepList->setDefaultDropAction(Qt::MoveAction);
        auto *stepDelegate = new DelayEditDelegate(this);
        stepList->setItemDelegate(stepDelegate);
        connect(stepList->model(), &QAbstractItemModel::rowsMoved, this, &MainWindow::onStepsReordered);
        connect(stepDelegate, &DelayEditDelegate::deleteClicked, this, [this](int row)
                {
            // 录制期间 stepList 的行数与 engine->currentMacro().steps 不一致，按行删除会删错步骤
            if (engine->isRecording())
                return;
            syncStepsFromList();
            Macro macro = engine->currentMacro();
            if (row >= 0 && row < macro.steps.size())
            {
                macro.steps.removeAt(row);
                engine->setCurrentMacro(macro);
                refreshStepList();
            } });

        connect(stepList, &QListWidget::doubleClicked, this, [this](const QModelIndex &index)
                {
            auto* item = stepList->item(index.row());
            if (!item)
                return;
            QString text = item->text();
            if (text.startsWith("⌛"))
            {
                stepList->openPersistentEditor(item);
            } });

        connect(stepList, &QListWidget::itemChanged, this, [this](QListWidgetItem *item)
                {
            if (!item)
                return;
            // 录制期间 UI 行号与 engine 步骤索引不一致，不能回写延迟值
            if (engine->isRecording())
                return;
            int row = stepList->row(item);
            auto macro = engine->currentMacro();
            if (row < 0 || row >= macro.steps.size())
                return;

            if (macro.steps[row].type == StepType::Delay)
            {
                double val = extractDelayFromText(item->text());
                if (val > 0)
                {
                    UI_LOG(QString("Delay updated: row=%1, value=%2").arg(row).arg(val));
                    macro.steps[row].delaySec = val;
                    engine->setCurrentMacro(macro);
                    auto updated = engine->currentMacro();
                    UI_LOG(QString("Verified: steps[%1].delaySec = %2").arg(row).arg(updated.steps[row].delaySec));
                    emit engine->dataChanged();
                }
            } });
        stepLayout->addWidget(stepList);

        btnRecordToggle = new QPushButton("开始录制 (F11)");
        connect(btnRecordToggle, &QPushButton::clicked, this, [this]()
                {
            if (engine->isRecording())
                engine->stopRecording();
            else
                engine->startRecording(); });
        auto *btnAddDelay = new QPushButton("添加延迟");
        connect(btnAddDelay, &QPushButton::clicked, this, &MainWindow::addDelayStep);
        auto *btnClearSteps = new QPushButton("清空步骤");
        connect(btnClearSteps, &QPushButton::clicked, this, &MainWindow::clearSteps);
        auto *row1 = new QHBoxLayout();
        row1->addWidget(btnRecordToggle);
        row1->addWidget(btnAddDelay);
        row1->addWidget(btnClearSteps);
        row1->addStretch();
        stepLayout->addLayout(row1);

        rightLayout->addWidget(groupStep);

        auto *saveBtn = new QPushButton("保存所有数据");
        connect(saveBtn, &QPushButton::clicked, this, &MainWindow::saveData);
        rightLayout->addWidget(saveBtn);

        logText = new QTextEdit();
        logText->setReadOnly(true);
        logText->setMaximumHeight(80);
        rightLayout->addWidget(new QLabel("日志:"));
        rightLayout->addWidget(logText);

        mainLayout->addWidget(rightWidget);

        // 控件同步
        connect(nameEdit, &QLineEdit::editingFinished, this, [this]()
                {
            auto *cur = macroList->currentItem();
            if (!cur || !cur->data(Qt::UserRole).isValid()) return;
            int invIdx = cur->data(Qt::UserRole).toInt();

            const auto &inv = engine->macroInventory();
            if (invIdx < 0 || invIdx >= inv.size()) return;

            QString newName = nameEdit->text().trimmed();
            if (newName.isEmpty()) {
                nameEdit->setText(inv[invIdx].name);
                return;
            }

            QString oldName = inv[invIdx].name;
            if (newName == oldName) return;

            // 宏名必须唯一（引擎按名称引用）
            if (engine->findMacro(newName)) {
                nameEdit->setText(oldName);
                QMessageBox::warning(this, "名称重复", QString("已存在名为 \"%1\" 的宏。").arg(newName));
                return;
            }

            // 更新库存（引擎内部会同步组引用、热键绑定等）
            Macro macro = inv[invIdx];
            macro.name = newName;
            engine->updateInInventory(oldName, macro);

            // 直接更新当前列表项的文字，避免重建列表和选择信号干扰
            QListWidgetItem* item = macroList->currentItem();
            if (item)
                item->setText(newName);

            UI_LOG(QString("Name changed: %1 -> %2").arg(oldName, newName)); });
        connect(repeatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx)
                {
            Macro macro = engine->currentMacro();
            macro.repeatMode = idx;
            engine->setCurrentMacro(macro);
            UI_LOG(QString("Repeat mode: %1").arg(idx)); });
        connect(enableCheck, &QCheckBox::toggled, this, [this](bool checked)
                {
            Macro macro = engine->currentMacro();
            macro.enabled = checked;
            engine->setCurrentMacro(macro);
            engine->rebuildHotkeyVkCodes();
            engine->updateHooksState();
            UI_LOG(QString("Enabled: %1").arg(checked)); });

        connect(globalActivationCheck, &QCheckBox::toggled, this, [this](bool checked)
                {
            Macro macro = engine->currentMacro();
            macro.globalActivation = checked;
            engine->setCurrentMacro(macro);
            engine->rebuildHotkeyVkCodes();
            engine->updateHooksState();
            UI_LOG(QString("Global activation: %1").arg(checked)); });

        connect(blockActionKeysCheck, &QCheckBox::toggled, this, [this](bool checked)
                {
            Macro macro = engine->currentMacro();
            macro.blockActionKeys = checked;
            engine->setCurrentMacro(macro);
            UI_LOG(QString("Block action keys: %1").arg(checked)); });
    }

    void refreshGroupAndMacroLists()
    {
        groupList->blockSignals(true);
        groupList->clear();
        const auto &groups = engine->groups();

        // Build category -> indices mapping (preserve encountered order)
        QHash<QString, QList<int>> catMap;
        QStringList catOrder;
        for (int i = 0; i < groups.size(); ++i)
        {
            QString cat = m_groupCategories.value(i).trimmed();
            if (!catOrder.contains(cat))
                catOrder.append(cat);
            catMap[cat].append(i);
        }
        // Include empty categories that have no items
        for (const auto &ec : m_emptyGroupCategories)
        {
            if (!catOrder.contains(ec))
                catOrder.append(ec);
        }
        // Clean up: categories that now have items are no longer empty
        for (const auto &cat : catOrder)
        {
            if (!cat.isEmpty() && catMap.contains(cat) && !catMap[cat].isEmpty())
                m_emptyGroupCategories.remove(cat);
        }

        // Populate list: headers + indented items
        for (const QString &cat : catOrder)
        {
            QString display = cat.isEmpty() ? QStringLiteral("默认分类") : cat;
            bool collapsed = m_collapsedGroupCategories.contains(cat);
            QString headerText = QString("%1 %2").arg(collapsed ? QChar(0x25B8) : QChar(0x25BE)).arg(display);
            auto *header = new QListWidgetItem(headerText);
            header->setData(Qt::UserRole + 1, cat); // mark as header
            QFont hf = header->font();
            hf.setBold(true);
            header->setFont(hf);
            groupList->setupItemFlags(header);
            groupList->addItem(header);

            for (int idx : catMap[cat])
            {
                auto *item = new QListWidgetItem(QString("    %1").arg(groups[idx].name));
                item->setData(Qt::UserRole, idx); // store engine index
                item->setData(Qt::UserRole + 1, cat);
                if (engine->activationMode() == ActivationMode::Group &&
                    engine->activeGroupIndices().contains(idx))
                {
                    item->setForeground(Qt::green);
                    QFont f = item->font();
                    f.setBold(true);
                    item->setFont(f);
                }
                groupList->setupItemFlags(item);
                groupList->addItem(item);
                if (collapsed)
                    item->setHidden(true);
            }
        }

        // Ensure selected group index is valid
        if (m_selectedGroupIndex >= groups.size())
            m_selectedGroupIndex = groups.isEmpty() ? 0 : groups.size() - 1;
        if (m_selectedGroupIndex < 0 && !groups.isEmpty())
            m_selectedGroupIndex = 0;

        // Select the item corresponding to m_selectedGroupIndex
        for (int i = 0; i < groupList->count(); ++i)
        {
            auto *it = groupList->item(i);
            if (!it)
                continue;
            QVariant v = it->data(Qt::UserRole);
            if (v.isValid() && v.toInt() == m_selectedGroupIndex)
            {
                groupList->setCurrentItem(it);
                break;
            }
        }

        groupList->blockSignals(false);
    }

    void refreshMacroListFromInventory()
    {
        macroList->blockSignals(true);
        macroList->clear();
        const auto &inv = engine->macroInventory();
        QString currentName = engine->currentMacro().name;

        // Build category -> indices mapping (preserve encountered order)
        QHash<QString, QList<int>> catMap;
        QStringList catOrder;
        for (int i = 0; i < inv.size(); ++i)
        {
            QString cat = m_macroCategories.value(i).trimmed();
            if (!catOrder.contains(cat))
                catOrder.append(cat);
            catMap[cat].append(i);
        }
        // Include empty categories that have no items
        for (const auto &ec : m_emptyMacroCategories)
        {
            if (!catOrder.contains(ec))
                catOrder.append(ec);
        }
        // Clean up: categories that now have items are no longer empty
        for (const auto &cat : catOrder)
        {
            if (!cat.isEmpty() && catMap.contains(cat) && !catMap[cat].isEmpty())
                m_emptyMacroCategories.remove(cat);
        }

        int selectIdx = -1;
        // Populate list: headers + indented items
        for (const QString &cat : catOrder)
        {
            QString display = cat.isEmpty() ? QStringLiteral("默认分类") : cat;
            bool collapsed = m_collapsedMacroCategories.contains(cat);
            QString headerText = QString("%1 %2").arg(collapsed ? QChar(0x25B8) : QChar(0x25BE)).arg(display);
            auto *header = new QListWidgetItem(headerText);
            header->setData(Qt::UserRole + 1, cat); // mark as header
            QFont hf = header->font();
            hf.setBold(true);
            header->setFont(hf);
            macroList->setupItemFlags(header);
            macroList->addItem(header);

            for (int idx : catMap[cat])
            {
                auto *item = new QListWidgetItem(QString("    %1").arg(inv[idx].name));
                item->setData(Qt::UserRole, idx); // store inventory index
                item->setData(Qt::UserRole + 1, cat);
                macroList->setupItemFlags(item);
                macroList->addItem(item);
                if (collapsed)
                    item->setHidden(true);
                if (inv[idx].name == currentName)
                    selectIdx = idx;
            }
        }

        if (selectIdx >= 0)
        {
            // Find the item with matching UserRole
            for (int i = 0; i < macroList->count(); ++i)
            {
                auto *it = macroList->item(i);
                if (!it)
                    continue;
                QVariant v = it->data(Qt::UserRole);
                if (v.isValid() && v.toInt() == selectIdx)
                {
                    macroList->setCurrentItem(it);
                    break;
                }
            }
        }
        macroList->blockSignals(false);
    }

    // 删除当前宏后清空右侧属性/步骤面板，避免继续编辑产生"空名幽灵宏"
    void resetMacroPanel()
    {
        engine->setCurrentMacro(Macro());
        stepList->clear();
        nameEdit->clear();
        triggerLabel->setText("未设置");
        repeatCombo->setCurrentIndex(0);
        enableCheck->setChecked(true);
        globalActivationCheck->setChecked(false);
        blockActionKeysCheck->setChecked(false);
        refreshStepList();
    }

    void refreshStepList()
    {
        // 先将当前列表的视觉顺序同步回数据模型（捕获拖拽重排的变更）
        syncStepsFromList();

        stepList->clear();
        const auto &steps = engine->currentMacro().steps;
        for (int i = 0; i < steps.size(); ++i)
        {
            auto *item = new QListWidgetItem(stepToString(steps[i]));
            // 存储步骤在 steps 向量中的索引，用于拖拽后恢复顺序
            item->setData(Qt::UserRole, i);
            stepList->addItem(item);
        }
    }

    // 将 QListWidget 的视觉顺序同步回 engine 的 steps 数据
    void syncStepsFromList()
    {
        if (stepList->count() == 0)
            return;

        const auto &currentSteps = engine->currentMacro().steps;
        if (stepList->count() != currentSteps.size())
            return; // 数量不一致，不能安全同步

        // 检查顺序是否真的变了
        bool orderChanged = false;
        for (int i = 0; i < stepList->count(); ++i)
        {
            QVariant idxVar = stepList->item(i)->data(Qt::UserRole);
            if (!idxVar.isValid())
                return;
            int storedIdx = idxVar.toInt();
            if (storedIdx != i)
            {
                orderChanged = true;
                break;
            }
        }
        if (!orderChanged)
            return;

        // 按视觉顺序重建 steps
        QVector<MacroStep> reordered;
        reordered.reserve(stepList->count());
        for (int i = 0; i < stepList->count(); ++i)
        {
            int storedIdx = stepList->item(i)->data(Qt::UserRole).toInt();
            if (storedIdx >= 0 && storedIdx < currentSteps.size())
                reordered.append(currentSteps[storedIdx]);
        }

        Macro macro = engine->currentMacro();
        macro.steps = reordered;
        // 用 blockSignals 阻止 dataChanged 导致宏列表失去选中项
        engine->blockSignals(true);
        engine->setCurrentMacro(macro);
        engine->blockSignals(false);
        // 手动触发一次 dataChanged 以保持 UI 同步
        emit engine->dataChanged();

        // 更新所有 stepList 项的 UserRole 索引以匹配新的视觉顺序，
        // 否则下次 syncStepsFromList 会用旧索引错误地重建顺序
        for (int i = 0; i < stepList->count(); ++i)
            stepList->item(i)->setData(Qt::UserRole, i);
    }

    void appendStepToList(const MacroStep &step)
    {
        // 录制期间 engine->currentMacro() 尚未更新，用列表当前位置作为索引
        // 录制结束后 stopRecording → recordingStopped → refreshStepList 会重建正确索引
        auto *item = new QListWidgetItem(stepToString(step));
        item->setData(Qt::UserRole, stepList->count());
        stepList->addItem(item);
    }

    void appendVirtualKeyToMacro(const QString &key)
    {
        syncStepsFromList();
        Macro macro = engine->currentMacro();
        const double defaultDelay = 0.05;

        if (key == "wheel_up" || key == "wheel_down")
        {
            MacroStep scroll;
            scroll.type = StepType::MouseScroll;
            scroll.scrollDelta = (key == "wheel_up") ? 120 : -120;
            macro.steps.append(scroll);
            MacroStep delay;
            delay.type = StepType::Delay;
            delay.delaySec = defaultDelay;
            macro.steps.append(delay);
            engine->setCurrentMacro(macro);
            refreshStepList();
            return;
        }

        static const QMap<QString, DWORD> mouseMap = {
            {"left", VK_LBUTTON}, {"right", VK_RBUTTON}, {"middle", VK_MBUTTON}, {"x1", VK_XBUTTON1}, {"x2", VK_XBUTTON2}};
        if (mouseMap.contains(key))
        {
            DWORD btn = mouseMap[key];
            MacroStep press;
            press.type = StepType::MousePress;
            press.keyOrButton = btn;
            macro.steps.append(press);
            MacroStep delay1;
            delay1.type = StepType::Delay;
            delay1.delaySec = defaultDelay;
            macro.steps.append(delay1);
            MacroStep release;
            release.type = StepType::MouseRelease;
            release.keyOrButton = btn;
            macro.steps.append(release);
            MacroStep delay2;
            delay2.type = StepType::Delay;
            delay2.delaySec = defaultDelay;
            macro.steps.append(delay2);
            engine->setCurrentMacro(macro);
            refreshStepList();
            return;
        }

        DWORD vk = engine->keyNameToVkCode(key);
        if (vk == 0)
        {
            UI_LOG(QString("Unknown key: %1").arg(key));
            return;
        }

        MacroStep kPress;
        kPress.type = StepType::KeyPress;
        kPress.keyOrButton = vk;
        macro.steps.append(kPress);
        MacroStep kDelay1;
        kDelay1.type = StepType::Delay;
        kDelay1.delaySec = defaultDelay;
        macro.steps.append(kDelay1);
        MacroStep kRelease;
        kRelease.type = StepType::KeyRelease;
        kRelease.keyOrButton = vk;
        macro.steps.append(kRelease);
        MacroStep kDelay2;
        kDelay2.type = StepType::Delay;
        kDelay2.delaySec = defaultDelay;
        macro.steps.append(kDelay2);
        engine->setCurrentMacro(macro);
        refreshStepList();
    }

    QString stepToString(const MacroStep &step) const
    {
        switch (step.type)
        {
        case StepType::KeyPress:
            return QString("▼ %1").arg(engine->keyToString(step.keyOrButton));
        case StepType::KeyRelease:
            return QString("▲ %1").arg(engine->keyToString(step.keyOrButton));
        case StepType::MousePress:
            return QString("▼ %1").arg(engine->keyToString(step.keyOrButton, true));
        case StepType::MouseRelease:
            return QString("▲ %1").arg(engine->keyToString(step.keyOrButton, true));
        case StepType::MouseScroll:
            return QString("滚轮 %1").arg(step.scrollDelta > 0 ? "向上" : "向下");
        case StepType::Delay:
            return QString("⌛ %1 秒").arg(step.delaySec, 0, 'f', 3);
        default:
            return "未知步骤";
        }
    }
};

int main(int argc, char *argv[])
{
    // ===== 将日志输出附加到父进程控制台（VS Code 集成终端）=====
    // WIN32 子系统不会创建独立命令行窗口，但也不会自动继承控制台。
    // 通过 AttachConsole 主动连接到 VS Code 的集成终端，使日志直接出现在其中。
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        FILE *dummy = nullptr;
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
        std::cout.clear();
        std::cerr.clear();
    }

    QApplication app(argc, argv);

    // ===== 单实例检测 =====
    const QString serverName = "KeyMacro_SingleInstance_Server";
    QLocalSocket socket;
    socket.connectToServer(serverName);
    if (socket.waitForConnected(500))
    {
        // 已有实例在运行，发送激活命令后退出
        socket.write("show");
        socket.waitForBytesWritten(500);
        socket.disconnectFromServer();
        return 0;
    }

    // 高DPI支持
    app.setAttribute(Qt::AA_EnableHighDpiScaling);
    app.setAttribute(Qt::AA_UseHighDpiPixmaps);

    // 设置应用程序显示名称
    app.setApplicationDisplayName("按键宏");

    // 设置应用程序图标
    app.setWindowIcon(QIcon(":/icon.ico"));

    // 安装消息处理器：将 qDebug 日志同步到 VS Code 调试控制台
#ifdef Q_OS_WIN
    qInstallMessageHandler(vsCodeLogHandler);
#endif

    MainWindow win;
    if (!win.loadMinimizeOnStart())
    {
        win.show();
    }
    return app.exec();
}

void MainWindow::onStepsReordered(const QModelIndex &, int, int, const QModelIndex &, int)
{
    syncStepsFromList();
}

#include "main.moc"