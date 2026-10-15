#include "mainwindow.h"
#include "protocol.h"
#include <QCheckBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHeaderView>
#include <QHostInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTextEdit>
#include <QTextDocument>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

MainWindow::MainWindow(const AppOptions &options, QWidget *parent)
    : QMainWindow(parent), m_options(options)
{
    QSettings settings;
    m_deviceId = settings.value("deviceId").toString();
    if (m_deviceId.isEmpty()) {
        m_deviceId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        settings.setValue("deviceId", m_deviceId);
    }
    // Separate instances on one computer have separate discovery identities.
    m_deviceId += "-" + QString::number(QCoreApplication::applicationPid());
    m_name = options.name.isEmpty() ? settings.value("name", QHostInfo::localHostName()).toString() : options.name;
    m_receiveDirectory = options.receiveDirectory.isEmpty()
        ? settings.value("receiveDirectory", QStandardPaths::writableLocation(QStandardPaths::DownloadLocation) + "/ByteMe").toString()
        : options.receiveDirectory;
    buildUi();
    m_manager.setReceiveDirectory(m_receiveDirectory);
    m_manager.setAutoAccept(m_autoAccept->isChecked());
    connect(&m_manager, &TransferManager::taskAdded, this, &MainWindow::addTask);
    connect(&m_manager, &TransferManager::taskUpdated, this, &MainWindow::updateTask);
    connect(&m_manager, &TransferManager::incomingOffer, this, &MainWindow::askToReceive);
    connect(&m_manager, &TransferManager::error, this, &MainWindow::log);
    connect(&m_manager, &TransferManager::taskProgress, this,
            [this](const QString &id, qint64 done, qint64 total, double speed) {
        if (!m_rows.contains(id)) return;
        const int row = m_rows.value(id);
        auto *bar = qobject_cast<QProgressBar *>(m_table->cellWidget(row, 5));
        const int value = total ? int(double(done) * 1000.0 / double(total)) : 1000;
        bar->setValue(value);
        bar->setFormat(QStringLiteral("%1% · %2/s").arg(value / 10.0, 0, 'f', 1).arg(Wire::readableSize(qint64(speed))));
        bar->setToolTip(QStringLiteral("%1 / %2").arg(Wire::readableSize(done), Wire::readableSize(total)));
    });
    connect(&m_manager, &TransferManager::taskFinished, this,
            [this](const QString &, bool success, const QString &message, const QString &path) {
        log(QStringLiteral("%1 %2%3").arg(success ? QStringLiteral("✓") : QStringLiteral("•"), message,
                                         path.isEmpty() ? QString() : "  " + path));
        savePending();
    });
    connect(&m_discovery, &Discovery::peerSeen, this, &MainWindow::updatePeer);
    connect(&m_discovery, &Discovery::peerGone, this, &MainWindow::removePeer);
    connect(&m_discovery, &Discovery::error, this, &MainWindow::log);
    m_listening = m_manager.listen(options.tcpPort);
    m_networkLabel->setText(m_listening
        ? QStringLiteral("●  TCP %1  ·  %2").arg(m_manager.port()).arg(options.discovery
            ? QStringLiteral("UDP %1").arg(options.udpPort) : QStringLiteral("发现已关闭"))
        : QStringLiteral("●  接收服务未启动"));
    if (m_listening) startDiscovery();
    restorePending();
    log(QStringLiteral("接收目录：%1").arg(QDir::toNativeSeparators(m_receiveDirectory)));
    log(QStringLiteral("选择设备后发送文件，也可拖入文件；中断后选择任务并点击“继续”。"));
    updateActions();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("ByteMe · 局域网文件传输"));
    resize(1180, 790);
    setMinimumSize(980, 660);
    setAcceptDrops(true);
    setFont(QFont(QStringLiteral("Microsoft YaHei UI"), 10));
    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget#root { background: #f3f6fa; color: #172b43; }
        QLabel { color: #172b43; }
        QLabel#brand { font-size: 30px; font-weight: 700; }
        QLabel#subtitle { color: #718298; font-size: 12px; }
        QLabel#section { font-size: 16px; font-weight: 600; }
        QLabel#network { color: #008b7b; background: #ddf3ee; padding: 9px 14px; border-radius: 15px; }
        QFrame#card { background: white; border: 1px solid #e2e9f0; border-radius: 12px; }
        QPushButton { background: #edf2f8; border: 1px solid #dae3ed; border-radius: 7px;
                      padding: 9px 14px; color: #263d57; }
        QPushButton:hover { background: #e0e9f3; }
        QPushButton:disabled { color: #9baabb; background: #f4f6f9; }
        QPushButton#primary { background: #008b7b; color: white; border: none; font-weight: 600; }
        QPushButton#primary:hover { background: #00786c; }
        QPushButton#primary:disabled { background: #b4d9d4; color: #f4fffd; }
        QLineEdit, QSpinBox { padding: 7px; border: 1px solid #dae3ed; border-radius: 6px; background: white; }
        QListWidget { border: none; background: white; outline: none; }
        QListWidget::item { padding: 13px 10px; border-bottom: 1px solid #edf2f7; border-radius: 6px; }
        QListWidget::item:selected { background: #e0f3ef; color: #006d63; }
        QTableWidget { border: none; background: white; gridline-color: #edf2f7; selection-background-color: #e6f3f0; selection-color: #172b43; }
        QHeaderView::section { background: #f6f8fb; color: #75849a; border: none; padding: 11px 7px; }
        QProgressBar { border: none; background: #edf3f6; border-radius: 5px; text-align: center; color: #243c50; font-size: 11px; }
        QProgressBar::chunk { background: #83d4c4; border-radius: 5px; }
        QTextEdit { border: none; background: #f6f8fb; color: #718298; padding: 5px; font-size: 11px; }
        QStatusBar { background: #f3f6fa; color: #718298; }
        QSplitter::handle { background: transparent; }
    )"));
    auto *root = new QWidget;
    root->setObjectName("root");
    setCentralWidget(root);
    auto *layout = new QVBoxLayout(root);
    layout->setContentsMargins(26, 22, 26, 12);
    layout->setSpacing(18);
    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    auto *brand = new QLabel(QStringLiteral("ByteMe"));
    brand->setObjectName("brand");
    titles->addWidget(brand);
    auto *subtitle = new QLabel(QStringLiteral("让文件在局域网里，自由抵达。"));
    subtitle->setObjectName("subtitle");
    titles->addWidget(subtitle);
    header->addLayout(titles);
    header->addStretch();
    m_networkLabel = new QLabel;
    m_networkLabel->setObjectName("network");
    header->addWidget(m_networkLabel);
    layout->addLayout(header);

    auto *settingsCard = new QFrame;
    settingsCard->setObjectName("card");
    auto *settingsLayout = new QHBoxLayout(settingsCard);
    settingsLayout->setContentsMargins(16, 14, 16, 14);
    settingsLayout->addWidget(new QLabel(QStringLiteral("本机名称")));
    m_nameEdit = new QLineEdit(m_name);
    m_nameEdit->setMaxLength(80);
    m_nameEdit->setCursorPosition(0);
    m_nameEdit->setMaximumWidth(175);
    settingsLayout->addWidget(m_nameEdit);
    settingsLayout->addSpacing(10);
    settingsLayout->addWidget(new QLabel(QStringLiteral("保存到")));
    m_directoryEdit = new QLineEdit(QDir::toNativeSeparators(m_receiveDirectory));
    m_directoryEdit->setReadOnly(true);
    settingsLayout->addWidget(m_directoryEdit, 1);
    auto *directoryButton = new QPushButton(QStringLiteral("更改"));
    settingsLayout->addWidget(directoryButton);
    m_autoAccept = new QCheckBox(QStringLiteral("自动接收"));
    m_autoAccept->setToolTip(QStringLiteral("启用后，收到文件时直接写入接收目录"));
    m_autoAccept->setChecked(QSettings().value("autoAccept", false).toBool());
    settingsLayout->addWidget(m_autoAccept);
    layout->addWidget(settingsCard);
    connect(directoryButton, &QPushButton::clicked, this, &MainWindow::chooseReceiveDirectory);
    connect(m_autoAccept, &QCheckBox::toggled, this, [this](bool checked) {
        m_manager.setAutoAccept(checked);
        QSettings().setValue("autoAccept", checked);
    });
    connect(m_nameEdit, &QLineEdit::editingFinished, this, [this] {
        const auto name = m_nameEdit->text().trimmed();
        if (name.isEmpty()) { m_nameEdit->setText(m_name); return; }
        m_name = name;
        QSettings().setValue("name", m_name);
        startDiscovery();
    });

    auto *splitter = new QSplitter;
    auto *deviceCard = new QFrame;
    deviceCard->setObjectName("card");
    auto *devices = new QVBoxLayout(deviceCard);
    devices->setContentsMargins(16, 18, 16, 16);
    auto *deviceHeader = new QHBoxLayout;
    auto *deviceTitle = new QLabel(QStringLiteral("附近设备"));
    deviceTitle->setObjectName("section");
    deviceHeader->addWidget(deviceTitle);
    deviceHeader->addStretch();
    m_peerCount = new QLabel(QStringLiteral("0 台在线"));
    m_peerCount->setObjectName("subtitle");
    deviceHeader->addWidget(m_peerCount);
    devices->addLayout(deviceHeader);
    auto *hint = new QLabel(QStringLiteral("同一网络内的设备会自动出现"));
    hint->setObjectName("subtitle");
    devices->addWidget(hint);
    m_peerList = new QListWidget;
    devices->addWidget(m_peerList, 1);
    auto *deviceActions = new QHBoxLayout;
    auto *refreshButton = new QPushButton(QStringLiteral("刷新"));
    auto *manualButton = new QPushButton(QStringLiteral("输入 IP"));
    deviceActions->addWidget(refreshButton);
    deviceActions->addWidget(manualButton);
    devices->addLayout(deviceActions);
    m_sendButton = new QPushButton(QStringLiteral("＋  发送文件"));
    m_sendButton->setObjectName("primary");
    devices->addWidget(m_sendButton);
    splitter->addWidget(deviceCard);
    connect(refreshButton, &QPushButton::clicked, &m_discovery, [this] { m_discovery.probe(); });
    connect(manualButton, &QPushButton::clicked, this, &MainWindow::addManualPeer);
    connect(m_sendButton, &QPushButton::clicked, this, &MainWindow::chooseFiles);
    connect(m_peerList, &QListWidget::itemSelectionChanged, this, &MainWindow::updateActions);
    connect(m_peerList, &QListWidget::itemDoubleClicked, this, [this] { chooseFiles(); });

    auto *transferCard = new QFrame;
    transferCard->setObjectName("card");
    auto *transfers = new QVBoxLayout(transferCard);
    transfers->setContentsMargins(18, 18, 18, 16);
    auto *transferHeader = new QHBoxLayout;
    auto *transferTitle = new QLabel(QStringLiteral("文件传输"));
    transferTitle->setObjectName("section");
    transferHeader->addWidget(transferTitle);
    transferHeader->addStretch();
    auto *transferHint = new QLabel(QStringLiteral("支持多任务 · 断点续传"));
    transferHint->setObjectName("subtitle");
    transferHeader->addWidget(transferHint);
    transfers->addLayout(transferHeader);
    m_table = new QTableWidget(0, 6);
    m_table->setHorizontalHeaderLabels({QStringLiteral("文件"), QStringLiteral("大小"), QStringLiteral("方向"),
                                        QStringLiteral("设备"), QStringLiteral("状态"), QStringLiteral("进度")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(54);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->setColumnWidth(1, 82);
    m_table->setColumnWidth(2, 56);
    m_table->setColumnWidth(3, 116);
    m_table->setColumnWidth(4, 142);
    m_table->setColumnWidth(5, 146);
    transfers->addWidget(m_table, 1);
    auto *dropHint = new QLabel(QStringLiteral("将文件拖入窗口，发送给当前选中的设备"));
    dropHint->setObjectName("subtitle");
    transfers->addWidget(dropHint);
    auto *taskActions = new QHBoxLayout;
    m_pauseButton = new QPushButton(QStringLiteral("暂停"));
    m_resumeButton = new QPushButton(QStringLiteral("继续"));
    m_openButton = new QPushButton(QStringLiteral("打开文件夹"));
    taskActions->addWidget(m_pauseButton);
    taskActions->addWidget(m_resumeButton);
    taskActions->addStretch();
    taskActions->addWidget(m_openButton);
    transfers->addLayout(taskActions);
    splitter->addWidget(transferCard);
    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({265, 830});
    layout->addWidget(splitter, 1);
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateActions);
    connect(m_pauseButton, &QPushButton::clicked, this, [this] { m_manager.pause(selectedTask()); });
    connect(m_resumeButton, &QPushButton::clicked, this, [this] {
        if (!m_manager.resume(selectedTask()))
            log(QStringLiteral("任务尚未停止或无法继续，请稍后重试"));
    });
    connect(m_openButton, &QPushButton::clicked, this, [this] {
        const auto task = m_tasks.value(selectedTask());
        const QString folder = task.outgoing ? QFileInfo(task.source).absolutePath()
            : (task.savedPath.isEmpty() ? m_receiveDirectory : QFileInfo(task.savedPath).absolutePath());
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    });
    m_log = new QTextEdit;
    m_log->setReadOnly(true);
    m_log->document()->setMaximumBlockCount(300);
    m_log->setMaximumHeight(96);
    layout->addWidget(m_log);
    statusBar()->showMessage(QStringLiteral("ByteMe 1.0  /  选择设备，发送文件  /  接收完成后自动校验文件"));
}

void MainWindow::startDiscovery()
{
    if (!m_options.discovery || !m_listening)
        return;
    m_discovery.stop();
    for (auto it = m_peers.begin(); it != m_peers.end();) {
        if (!it.key().startsWith("manual:")) it = m_peers.erase(it);
        else ++it;
    }
    for (int row = m_peerList->count() - 1; row >= 0; --row) {
        if (!m_peerList->item(row)->data(Qt::UserRole).toString().startsWith("manual:"))
            delete m_peerList->takeItem(row);
    }
    m_discovery.start(m_deviceId, m_name, m_manager.port(), m_options.udpPort);
    m_peerCount->setText(QStringLiteral("%1 台设备").arg(m_peerList->count()));
}

void MainWindow::updatePeer(const Peer &peer)
{
    m_peers.insert(peer.id, peer);
    QListWidgetItem *item = nullptr;
    for (int row = 0; row < m_peerList->count(); ++row) {
        if (m_peerList->item(row)->data(Qt::UserRole).toString() == peer.id) {
            item = m_peerList->item(row);
            break;
        }
    }
    if (!item) {
        item = new QListWidgetItem(m_peerList);
        item->setData(Qt::UserRole, peer.id);
    }
    item->setText(QStringLiteral("%1\n%2 : %3").arg(peer.name, peer.address.toString()).arg(peer.port));
    item->setToolTip(peer.id.startsWith("manual:") ? QStringLiteral("手动添加的地址") : QStringLiteral("自动发现的局域网设备"));
    m_peerCount->setText(QStringLiteral("%1 台设备").arg(m_peerList->count()));
}

void MainWindow::removePeer(const QString &id)
{
    m_peers.remove(id);
    for (int row = 0; row < m_peerList->count(); ++row) {
        if (m_peerList->item(row)->data(Qt::UserRole).toString() == id) {
            delete m_peerList->takeItem(row);
            break;
        }
    }
    m_peerCount->setText(QStringLiteral("%1 台设备").arg(m_peerList->count()));
    updateActions();
}

void MainWindow::addTask(const TaskInfo &task)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_rows.insert(task.id, row);
    for (int column = 0; column < 6; ++column)
        m_table->setItem(row, column, new QTableWidgetItem);
    m_table->item(row, 0)->setData(Qt::UserRole, task.id);
    auto *progress = new QProgressBar;
    progress->setRange(0, 1000);
    progress->setValue(0);
    progress->setFormat(QStringLiteral("等待传输"));
    m_table->setCellWidget(row, 5, progress);
    updateTask(task);
}

void MainWindow::updateTask(const TaskInfo &task)
{
    if (!m_rows.contains(task.id)) return;
    m_tasks.insert(task.id, task);
    const int row = m_rows.value(task.id);
    const QStringList values{task.name, Wire::readableSize(task.size), task.outgoing ? QStringLiteral("发送") : QStringLiteral("接收"),
                             task.peer, task.state};
    for (int column = 0; column < values.size(); ++column) {
        m_table->item(row, column)->setText(values.at(column));
        m_table->item(row, column)->setToolTip(values.at(column));
    }
    if (!task.active) {
        auto *bar = qobject_cast<QProgressBar *>(m_table->cellWidget(row, 5));
        if (task.success) { bar->setValue(1000); bar->setFormat(QStringLiteral("已完成 ✓")); }
        else bar->setFormat(QStringLiteral("可继续"));
    }
    updateActions();
    savePending();
}

QString MainWindow::selectedTask() const
{
    const int row = m_table->currentRow();
    return row >= 0 ? m_table->item(row, 0)->data(Qt::UserRole).toString() : QString();
}

void MainWindow::updateActions()
{
    const auto task = m_tasks.value(selectedTask());
    m_sendButton->setEnabled(m_peerList->currentItem() != nullptr);
    m_pauseButton->setEnabled(task.active);
    m_resumeButton->setEnabled(!task.id.isEmpty() && task.outgoing && !task.active && !task.success);
    m_resumeButton->setToolTip(task.outgoing ? QStringLiteral("重新连接并从接收端确认的位置继续")
        : QStringLiteral("接收任务由发送方点击继续发起"));
    m_openButton->setEnabled(!task.id.isEmpty());
}

void MainWindow::chooseFiles()
{
    if (!m_peerList->currentItem()) return;
    const auto paths = QFileDialog::getOpenFileNames(this, QStringLiteral("选择要发送的文件"));
    if (!paths.isEmpty()) sendPaths(paths);
}

void MainWindow::sendPaths(const QStringList &paths)
{
    if (!m_peerList->currentItem()) {
        log(QStringLiteral("请先选择接收设备"));
        return;
    }
    const auto peer = m_peers.value(m_peerList->currentItem()->data(Qt::UserRole).toString());
    for (const auto &path : paths) {
        if (!QFileInfo(path).isFile()) { log(QStringLiteral("跳过目录或不存在的文件：%1").arg(path)); continue; }
        m_manager.sendFile(path, peer.address.toString(), peer.port);
    }
}

void MainWindow::addManualPeer()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("添加接收设备"));
    auto *layout = new QFormLayout(&dialog);
    QLineEdit address(QStringLiteral("192.168.1."));
    QSpinBox port;
    port.setRange(1, 65535);
    port.setValue(45455);
    layout->addRow(QStringLiteral("IPv4 地址"), &address);
    layout->addRow(QStringLiteral("TCP 端口"), &port);
    QDialogButtonBox buttons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addRow(&buttons);
    connect(&buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(&buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const QHostAddress host(address.text().trimmed());
    if (host.isNull() || host.protocol() != QAbstractSocket::IPv4Protocol) {
        log(QStringLiteral("请输入有效的 IPv4 地址"));
        return;
    }
    const auto id = QStringLiteral("manual:%1:%2").arg(host.toString()).arg(port.value());
    updatePeer({id, QStringLiteral("手动设备"), host, quint16(port.value()), QDateTime::currentDateTimeUtc()});
    for (int row = 0; row < m_peerList->count(); ++row)
        if (m_peerList->item(row)->data(Qt::UserRole).toString() == id) m_peerList->setCurrentRow(row);
}

void MainWindow::chooseReceiveDirectory()
{
    const auto path = QFileDialog::getExistingDirectory(this, QStringLiteral("选择接收目录"), m_receiveDirectory);
    if (path.isEmpty()) return;
    m_receiveDirectory = path;
    m_directoryEdit->setText(QDir::toNativeSeparators(path));
    m_manager.setReceiveDirectory(path);
    QSettings().setValue("receiveDirectory", path);
    log(QStringLiteral("新的接收任务将保存到：%1").arg(path));
}

void MainWindow::askToReceive(const QString &id, const QString &name, const QString &peer, qint64 size)
{
    auto *box = new QMessageBox(QMessageBox::Question, QStringLiteral("收到文件"),
        QStringLiteral("%1 希望发送：\n\n%2\n大小：%3\n\n接收此文件？").arg(peer, name, Wire::readableSize(size)),
        QMessageBox::Yes | QMessageBox::No, this);
    box->setTextFormat(Qt::PlainText);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setDefaultButton(QMessageBox::No);
    connect(box, &QMessageBox::finished, this, [this, id](int result) { m_manager.approve(id, result == QMessageBox::Yes); });
    connect(&m_manager, &TransferManager::taskFinished, box,
        [box, id](const QString &taskId, bool, const QString &, const QString &) { if (taskId == id) box->reject(); });
    box->open();
}

void MainWindow::log(const QString &message)
{
    m_log->append(QStringLiteral("%1  %2").arg(QTime::currentTime().toString("HH:mm:ss"), message.toHtmlEscaped()));
}

void MainWindow::savePending()
{
    if (m_restoring) return;
    QSettings settings;
    settings.beginWriteArray("pending");
    int index = 0;
    for (const auto &task : m_manager.tasks()) {
        if (!task.outgoing || task.success || index >= 100) continue;
        settings.setArrayIndex(index++);
        settings.setValue("path", task.source);
        settings.setValue("host", task.host);
        settings.setValue("port", task.port);
    }
    settings.endArray();
}

void MainWindow::restorePending()
{
    m_restoring = true;
    QSettings settings;
    const int count = qMin(settings.beginReadArray("pending"), 100);
    for (int index = 0; index < count; ++index) {
        settings.setArrayIndex(index);
        const auto path = settings.value("path").toString();
        const auto host = settings.value("host").toString();
        const int port = settings.value("port").toInt();
        if (!path.isEmpty() && !host.isEmpty() && port > 0 && port <= 65535)
            m_manager.restore(path, host, quint16(port));
    }
    settings.endArray();
    m_restoring = false;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    savePending();
    m_discovery.stop();
    m_manager.shutdown();
    QMainWindow::closeEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    QStringList paths;
    for (const auto &url : event->mimeData()->urls())
        if (url.isLocalFile()) paths.append(url.toLocalFile());
    sendPaths(paths);
    event->acceptProposedAction();
}
