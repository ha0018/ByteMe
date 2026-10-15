#pragma once
#include "discovery.h"
#include "transfermanager.h"
#include <QMainWindow>

class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTableWidget;
class QTextEdit;

struct AppOptions {
    quint16 tcpPort = 45455;
    quint16 udpPort = 45454;
    QString name;
    QString receiveDirectory;
    bool discovery = true;
};

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const AppOptions &options, QWidget *parent = nullptr);
protected:
    void closeEvent(QCloseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    void buildUi();
    void updatePeer(const Peer &peer);
    void removePeer(const QString &id);
    void addTask(const TaskInfo &task);
    void updateTask(const TaskInfo &task);
    void updateActions();
    void chooseFiles();
    void sendPaths(const QStringList &paths);
    void addManualPeer();
    void chooseReceiveDirectory();
    void askToReceive(const QString &id, const QString &name, const QString &peer, qint64 size);
    void log(const QString &message);
    void savePending();
    void restorePending();
    QString selectedTask() const;
    void startDiscovery();
    AppOptions m_options;
    Discovery m_discovery;
    TransferManager m_manager;
    QHash<QString, Peer> m_peers;
    QHash<QString, int> m_rows;
    QHash<QString, TaskInfo> m_tasks;
    QString m_deviceId;
    QString m_name;
    QString m_receiveDirectory;
    bool m_restoring = false;
    bool m_listening = false;
    QListWidget *m_peerList = nullptr;
    QTableWidget *m_table = nullptr;
    QTextEdit *m_log = nullptr;
    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_directoryEdit = nullptr;
    QLabel *m_networkLabel = nullptr;
    QLabel *m_peerCount = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_pauseButton = nullptr;
    QPushButton *m_resumeButton = nullptr;
    QPushButton *m_openButton = nullptr;
    QCheckBox *m_autoAccept = nullptr;
};
